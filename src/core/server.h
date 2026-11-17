#pragma once

#include "core/http.h"
#include "core/pcm.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace lazo {

// Posted to the notify window as wParam of its message, from server threads.
enum class ServerEvent : WPARAM { ListenersChanged = 1 };

struct Listener {
    uint32_t id = 0;
    std::wstring device;   // "iPhone"
    uint32_t address = 0;  // host byte order
    ULONGLONG since = 0;   // GetTickCount64
    uint32_t dropped = 0;  // packets skipped because the network fell behind
    bool saver = false;    // gets the data saver stream
    bool hasStats = false;
    ListenerStats stats;   // as last reported by the page
};

struct ServerOptions {
    uint32_t address = 0;  // host byte order; the server listens on this address only
    int port = 0;          // the first of ten ports tried
    std::string token;
    // The host the link names: "lazo-pc.local" (answered by MdnsResponder),
    // or empty for the IP address.
    std::string hostName;
    std::wstring pcName;
    std::string touchIcon;       // PNG for the phone's home screen, may be empty
    bool allowLoopback = false;  // tests talk to 127.0.0.1
};

// The small HTTP and WebSocket server the phone talks to: it serves the page
// and streams the audio to every phone listening. Every request must carry
// the token; only peers on a private network are accepted. Windows sockets
// only, no HTTP library.
class Server {
public:
    Server(HWND notifyWindow = nullptr, UINT notifyMessage = 0);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void SetNotifyWindow(HWND window, UINT message) {
        notifyWindow_ = window;
        notifyMessage_ = message;
    }

    bool Start(const ServerOptions& options);
    // Disconnects every phone.
    void Stop();

    bool IsRunning() const { return running_; }
    int Port() const { return port_; }
    // http://lazo-pc.local:8765/?t=<token> (or the IP), what the QR code holds.
    std::wstring Url() const;
    // The same without the token, to show on screen.
    std::wstring Address() const;

    // The audio's format. Phones get it before any audio, and again when it
    // changes (another output with another sample rate).
    void SetFormat(uint32_t rate, size_t framesPerPacket);
    // Sends one packet of 16-bit stereo to every phone. A phone whose network
    // falls behind skips packets instead of piling up delay. Any thread.
    void Broadcast(const int16_t* stereo, size_t frames);

    std::vector<Listener> Listeners() const;
    size_t ListenerCount() const;
    // GetTickCount64 of when the last phone stopped listening, or now while
    // one listens.
    ULONGLONG LastListening() const;

private:
    struct Client;

    void AcceptLoop();
    void Serve(SOCKET socket, uint32_t peer);
    bool Handle(SOCKET socket, const RequestHead& request, uint32_t peer);
    void ServeWebSocket(SOCKET socket, const RequestHead& request, uint32_t peer);
    std::string ConfigFrame(bool saver) const;  // needs mutex_
    void Notify();

    HWND notifyWindow_;
    UINT notifyMessage_;

    std::atomic<bool> running_{false};
    SOCKET listener_ = INVALID_SOCKET;
    std::thread acceptThread_;
    ServerOptions options_;
    int port_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable idle_;
    std::set<SOCKET> connections_;
    std::vector<std::shared_ptr<Client>> clients_;
    uint32_t nextId_ = 1;
    uint32_t rate_ = 0;
    size_t framesPerPacket_ = 0;
    SaverDownsampler saverDownsampler_;
    std::vector<int16_t> saverSamples_;
    ULONGLONG lastListening_ = 0;
};

}  // namespace lazo
