#include "core/server.h"

#include "core/pcm.h"
#include "core/text.h"
#include "core/web_page.h"
#include "core/websocket.h"

#include <ws2tcpip.h>

#include <algorithm>

namespace lazo {

namespace {

constexpr size_t kMaxHeadBytes = 16 * 1024;
constexpr size_t kMaxConnections = 32;
constexpr size_t kMaxListeners = 8;
constexpr DWORD kHttpTimeoutMs = 30'000;
// A phone that stops reading for this long is dropped.
constexpr DWORD kSendTimeoutMs = 5'000;
constexpr int kPortsToTry = 10;
// Audio waiting for a slow phone: about 170 ms at 48 kHz in Lazo's queue,
// plus the same in the socket's own buffer. Past that, packets are skipped.
constexpr size_t kMaxQueuedBytes = 32 * 1024;
constexpr int kSocketSendBuffer = 32 * 1024;
// The page sends nothing but control frames.
constexpr size_t kMaxIncomingFrame = 4096;

constexpr char kCommonHeaders[] =
    "Cache-Control: no-store\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    // The token is in the URL; never pass it on to another site.
    "Referrer-Policy: no-referrer\r\n";

bool SendAll(SOCKET socket, const char* data, size_t size) {
    while (size > 0) {
        const int sent = send(socket, data, static_cast<int>(std::min<size_t>(size, 1 << 20)), 0);
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

const char* StatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}

bool SendResponse(SOCKET socket, int status, const char* contentType, std::string_view body, bool keepAlive,
                  const std::string& extraHeaders = {}) {
    std::string response = "HTTP/1.1 " + std::to_string(status) + " " + StatusText(status) + "\r\n";
    if (status != 204) {
        response += "Content-Type: ";
        response += contentType;
        response += "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    }
    response += kCommonHeaders;
    response += extraHeaders;
    response += keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    response += body;
    return SendAll(socket, response.data(), response.size());
}

bool SendError(SOCKET socket, int status) {
    // Errors close the connection: what is left of the request is unknown.
    SendResponse(socket, status, "text/plain; charset=utf-8", StatusText(status), false);
    return false;
}

}  // namespace

struct Server::Client {
    Listener info;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::shared_ptr<const std::string>> queue;
    size_t queuedBytes = 0;
    bool configured = false;  // got the format, so audio may follow
    bool closing = false;
};

Server::Server(HWND notifyWindow, UINT notifyMessage) : notifyWindow_(notifyWindow), notifyMessage_(notifyMessage) {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
}

Server::~Server() {
    Stop();
    WSACleanup();
}

bool Server::Start(const ServerOptions& options) {
    Stop();
    options_ = options;
    for (int offset = 0; offset < kPortsToTry; ++offset) {
        const int port = options.port + offset;
        if (port > 65535) break;
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) return false;
        // No other program may take over the port while Lazo uses it.
        BOOL exclusive = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<u_short>(port));
        address.sin_addr.s_addr = htonl(options.address);
        if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
            listen(listener, SOMAXCONN) == 0) {
            listener_ = listener;
            port_ = port;
            {
                std::lock_guard lock(mutex_);
                lastListening_ = GetTickCount64();
            }
            running_ = true;
            acceptThread_ = std::thread(&Server::AcceptLoop, this);
            return true;
        }
        closesocket(listener);
    }
    return false;
}

void Server::Stop() {
    if (!running_.exchange(false)) return;
    closesocket(listener_);
    listener_ = INVALID_SOCKET;
    if (acceptThread_.joinable()) acceptThread_.join();

    std::unique_lock lock(mutex_);
    for (const SOCKET connection : connections_) shutdown(connection, SD_BOTH);
    for (const auto& client : clients_) {
        std::lock_guard clientLock(client->mutex);
        client->closing = true;
        client->wake.notify_all();
    }
    idle_.wait(lock, [this] { return connections_.empty(); });
}

std::wstring Server::Address() const {
    const std::wstring host = options_.hostName.empty() ? FormatIPv4(options_.address) : FromUtf8(options_.hostName);
    return L"http://" + host + L":" + std::to_wstring(port_);
}

std::wstring Server::Url() const { return Address() + L"/?t=" + FromUtf8(options_.token); }

std::string Server::ConfigFrame(bool saver) const {
    const std::string json = saver ? StreamConfigJson(rate_ / 2, 1, framesPerPacket_ / 2, options_.pcName)
                                   : StreamConfigJson(rate_, 2, framesPerPacket_, options_.pcName);
    return websocket::EncodeFrame(websocket::kText, json);
}

void Server::SetFormat(uint32_t rate, size_t framesPerPacket) {
    std::lock_guard lock(mutex_);
    const bool changed = rate != rate_ || framesPerPacket != framesPerPacket_;
    rate_ = rate;
    framesPerPacket_ = framesPerPacket;
    if (changed) saverDownsampler_.Reset();
    const auto full = std::make_shared<const std::string>(ConfigFrame(false));
    const auto saver = std::make_shared<const std::string>(ConfigFrame(true));
    for (const auto& client : clients_) {
        std::lock_guard clientLock(client->mutex);
        // The page starts over on every format message: only send what is new.
        if (client->configured && !changed) continue;
        client->configured = true;
        const auto& frame = client->info.saver ? saver : full;
        client->queue.push_back(frame);
        client->queuedBytes += frame->size();
        client->wake.notify_all();
    }
}

void Server::Broadcast(const int16_t* stereo, size_t frames) {
    std::lock_guard lock(mutex_);
    if (clients_.empty()) return;
    // x64 is little-endian, the byte order the page reads.
    const std::string_view payload(reinterpret_cast<const char*>(stereo), frames * 2 * sizeof(int16_t));
    const auto full = std::make_shared<const std::string>(websocket::EncodeFrame(websocket::kBinary, payload));
    // The data saver stream is only made while someone wants it; its filter
    // keeps running between packets so the sound stays continuous.
    std::shared_ptr<const std::string> saver;
    const bool anySaver = std::any_of(clients_.begin(), clients_.end(), [](const auto& client) { return client->info.saver; });
    if (anySaver) {
        saverSamples_.clear();
        saverDownsampler_.Process(stereo, frames, saverSamples_);
        saver = std::make_shared<const std::string>(websocket::EncodeFrame(
            websocket::kBinary,
            std::string_view(reinterpret_cast<const char*>(saverSamples_.data()), saverSamples_.size() * sizeof(int16_t))));
    }
    for (const auto& client : clients_) {
        std::lock_guard clientLock(client->mutex);
        if (!client->configured || client->closing) continue;
        const auto& frame = client->info.saver ? saver : full;
        if (client->queuedBytes + frame->size() > kMaxQueuedBytes) {
            ++client->info.dropped;
            continue;
        }
        client->queue.push_back(frame);
        client->queuedBytes += frame->size();
        client->wake.notify_all();
    }
}

std::vector<Listener> Server::Listeners() const {
    std::lock_guard lock(mutex_);
    std::vector<Listener> listeners;
    for (const auto& client : clients_) {
        std::lock_guard clientLock(client->mutex);
        listeners.push_back(client->info);
    }
    return listeners;
}

size_t Server::ListenerCount() const {
    std::lock_guard lock(mutex_);
    return clients_.size();
}

ULONGLONG Server::LastListening() const {
    std::lock_guard lock(mutex_);
    return clients_.empty() ? lastListening_ : GetTickCount64();
}

void Server::Notify() {
    if (notifyWindow_) PostMessageW(notifyWindow_, notifyMessage_, static_cast<WPARAM>(ServerEvent::ListenersChanged), 0);
}

void Server::AcceptLoop() {
    for (;;) {
        sockaddr_in peer{};
        int peerSize = sizeof(peer);
        const SOCKET connection = accept(listener_, reinterpret_cast<sockaddr*>(&peer), &peerSize);
        if (connection == INVALID_SOCKET) {
            if (!running_) return;
            continue;
        }
        const uint32_t peerAddress = ntohl(peer.sin_addr.s_addr);
        const bool loopback = (peerAddress >> 24) == 127;
        if (!(IsPrivateIPv4(peerAddress) || (loopback && options_.allowLoopback))) {
            closesocket(connection);
            continue;
        }
        {
            std::lock_guard lock(mutex_);
            if (!running_ || connections_.size() >= kMaxConnections) {
                closesocket(connection);
                continue;
            }
            connections_.insert(connection);
        }
        // A stalled phone cannot hold a connection forever.
        const DWORD timeout = kHttpTimeoutMs;
        setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::thread([this, connection, peerAddress] {
            Serve(connection, peerAddress);
            std::lock_guard lock(mutex_);
            connections_.erase(connection);
            closesocket(connection);
            idle_.notify_all();
        }).detach();
    }
}

void Server::Serve(SOCKET socket, uint32_t peer) {
    std::string buffer;
    while (running_) {
        size_t headEnd;
        while ((headEnd = buffer.find("\r\n\r\n")) == std::string::npos) {
            if (buffer.size() > kMaxHeadBytes) return;
            char chunk[4096];
            const int received = recv(socket, chunk, sizeof(chunk), 0);
            if (received <= 0) return;
            buffer.append(chunk, static_cast<size_t>(received));
        }
        const auto request = ParseRequestHead(std::string_view(buffer).substr(0, headEnd));
        // Nothing the page sends has a body.
        if (!request || request->contentLength > 0) {
            SendError(socket, 400);
            return;
        }
        buffer.erase(0, headEnd + 4);
        if (request->path == "/ws") {
            ServeWebSocket(socket, *request, peer);
            return;
        }
        if (!Handle(socket, *request, peer) || !request->keepAlive) return;
    }
}

bool Server::Handle(SOCKET socket, const RequestHead& request, uint32_t) {
    if (request.method != "GET" && request.method != "HEAD") return SendError(socket, 405);

    // The home screen icon carries no token; it is only a picture.
    if (request.path == "/apple-touch-icon.png" || request.path == "/apple-touch-icon-precomposed.png" ||
        request.path == "/favicon.ico") {
        if (options_.touchIcon.empty()) return SendResponse(socket, 404, "text/plain", "", request.keepAlive);
        return SendResponse(socket, 200, "image/png", options_.touchIcon, request.keepAlive,
                            "Cache-Control: max-age=86400\r\n");
    }

    const std::string token = QueryParam(request.query, "t").value_or("");
    const bool authorized = TokensEqual(token, options_.token);
    if (request.path == "/") {
        // Old links and guessed addresses end here.
        if (!authorized) return SendResponse(socket, 403, "text/html; charset=utf-8", ExpiredPageHtml(), false);
        const std::string csp = "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; "
                                "style-src 'unsafe-inline'; img-src 'self' data:; connect-src 'self' ws://" +
                                request.host +
                                "; form-action 'none'; frame-ancestors 'none'; base-uri 'none'\r\n";
        return SendResponse(socket, 200, "text/html; charset=utf-8", PageHtml(), request.keepAlive, csp);
    }
    if (request.path == "/api/ping") {
        // Lets a page that cannot reconnect tell "link changed" from "PC away".
        return SendResponse(socket, authorized ? 204 : 403, "text/plain", "", request.keepAlive);
    }
    return SendError(socket, 404);
}

void Server::ServeWebSocket(SOCKET socket, const RequestHead& request, uint32_t peer) {
    const std::string token = QueryParam(request.query, "t").value_or("");
    if (!IsWebSocketUpgrade(request) || !TokensEqual(token, options_.token)) {
        SendError(socket, IsWebSocketUpgrade(request) ? 403 : 400);
        return;
    }
    // Only Lazo's own page may connect: a browser always sends its Origin.
    if (!request.origin.empty() && request.origin != "http://" + request.host) {
        SendError(socket, 403);
        return;
    }

    auto client = std::make_shared<Client>();
    {
        std::lock_guard lock(mutex_);
        if (clients_.size() >= kMaxListeners) {
            SendError(socket, 503);
            return;
        }
        client->info.id = nextId_++;
        client->info.device = DeviceNameFromUserAgent(request.userAgent);
        client->info.address = peer;
        client->info.since = GetTickCount64();
        client->info.saver = QueryParam(request.query, "q").value_or("") == "saver";
        if (client->info.saver && std::none_of(clients_.begin(), clients_.end(), [](const auto& other) { return other->info.saver; })) {
            saverDownsampler_.Reset();
        }
        if (rate_) {
            client->configured = true;
            const auto frame = std::make_shared<const std::string>(ConfigFrame(client->info.saver));
            client->queue.push_back(frame);
            client->queuedBytes = frame->size();
        }
    }

    const std::string handshake = websocket::HandshakeResponse(request.webSocketKey);
    if (!SendAll(socket, handshake.data(), handshake.size())) return;
    // Every packet leaves at once; a small socket buffer keeps a slow phone's
    // backlog visible to the queue above, where packets can be skipped.
    BOOL noDelay = TRUE;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    int sendBuffer = kSocketSendBuffer;
    setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBuffer), sizeof(sendBuffer));
    const DWORD sendTimeout = kSendTimeoutMs;
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));

    {
        std::lock_guard lock(mutex_);
        clients_.push_back(client);
    }
    Notify();

    std::string incoming;
    std::vector<std::shared_ptr<const std::string>> batch;
    bool open = true;
    while (open && running_) {
        {
            std::unique_lock lock(client->mutex);
            client->wake.wait_for(lock, std::chrono::milliseconds(50), [&] { return !client->queue.empty() || client->closing; });
            if (client->closing) break;
            batch.assign(client->queue.begin(), client->queue.end());
            client->queue.clear();
            client->queuedBytes = 0;
        }
        for (const auto& frame : batch) {
            if (!SendAll(socket, frame->data(), frame->size())) {
                open = false;
                break;
            }
        }
        batch.clear();
        if (!open) break;

        // Anything from the page? Never wait for it here.
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket, &readable);
        timeval immediately{0, 0};
        if (select(0, &readable, nullptr, nullptr, &immediately) <= 0) continue;
        char chunk[1024];
        const int received = recv(socket, chunk, sizeof(chunk), 0);
        if (received <= 0) break;
        incoming.append(chunk, static_cast<size_t>(received));
        websocket::Frame frame;
        websocket::ParseResult result;
        while ((result = websocket::ParseClientFrame(incoming, frame, kMaxIncomingFrame)) == websocket::ParseResult::Complete) {
            if (frame.opcode == websocket::kClose) {
                const std::string reply = websocket::EncodeFrame(websocket::kClose, frame.payload.substr(0, 2));
                SendAll(socket, reply.data(), reply.size());
                open = false;
                break;
            }
            if (frame.opcode == websocket::kText) {
                if (const auto stats = ParseListenerStats(frame.payload)) {
                    std::lock_guard lock(client->mutex);
                    client->info.stats = *stats;
                    client->info.hasStats = true;
                }
            }
            if (frame.opcode == websocket::kPing) {
                const std::string reply = websocket::EncodeFrame(websocket::kPong, frame.payload);
                if (!SendAll(socket, reply.data(), reply.size())) open = false;
            }
        }
        if (result == websocket::ParseResult::Error) break;
    }

    {
        std::lock_guard lock(mutex_);
        std::erase(clients_, client);
        if (clients_.empty()) lastListening_ = GetTickCount64();
    }
    Notify();
}

}  // namespace lazo
