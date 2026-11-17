// Lazo's automated tests: a minimal runner without dependencies.
//
//   LazoTests.exe
//
// The server tests run a real server on 127.0.0.1 and talk to it over
// sockets, as a browser would. The capture test records the PC's default
// output for a moment and is skipped where there is none.

#include "core/audio.h"
#include "core/http.h"
#include "core/pcm.h"
#include "core/qr.h"
#include "core/server.h"
#include "core/settings.h"
#include "core/text.h"
#include "core/web_page.h"
#include "core/websocket.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <ks.h>
#include <ksmedia.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace lazo;

namespace {

struct TestCase {
    const char* name;
    std::function<void()> run;
};

std::vector<TestCase>& Tests() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> run) { Tests().push_back({name, std::move(run)}); }
};

struct Failure {
    std::string message;
};
struct Skip {
    std::string reason;
};

#define TEST(name)                                  \
    static void name();                             \
    static Registrar name##_registrar(#name, name); \
    static void name()

#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) throw Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #expression}; \
    } while (false)

constexpr wchar_t kTestSettingsKey[] = L"Software\\Lazo Audio Link Tests";
constexpr wchar_t kTestRunValue[] = L"Lazo Audio Link Tests";
constexpr uint32_t kLoopback = 0x7F000001;
constexpr char kToken[] = "0123456789abcdef0123456789abcdef";

SOCKET Connect(int port, int receiveBuffer = 0) {
    SOCKET connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (receiveBuffer) {
        setsockopt(connection, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBuffer), sizeof(receiveBuffer));
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    address.sin_addr.s_addr = htonl(kLoopback);
    if (connect(connection, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(connection);
        return INVALID_SOCKET;
    }
    const DWORD timeout = 3000;
    setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return connection;
}

struct Response {
    int status = 0;
    std::string head;
    std::string body;
};

// One request on a fresh connection, read to the end.
Response Request(int port, const std::string& request) {
    Response response;
    SOCKET connection = Connect(port);
    if (connection == INVALID_SOCKET) return response;
    send(connection, request.data(), static_cast<int>(request.size()), 0);
    std::string data;
    char buffer[8192];
    int received;
    while ((received = recv(connection, buffer, sizeof(buffer), 0)) > 0) data.append(buffer, static_cast<size_t>(received));
    closesocket(connection);
    const size_t headEnd = data.find("\r\n\r\n");
    if (headEnd == std::string::npos) return response;
    response.head = data.substr(0, headEnd);
    response.body = data.substr(headEnd + 4);
    if (response.head.size() > 12) response.status = std::stoi(response.head.substr(9, 3));
    return response;
}

std::string Get(const std::string& target) { return "GET " + target + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"; }

std::string UpgradeRequest(const std::string& target, int port, const std::string& origin = "") {
    std::string request = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
                          "User-Agent: Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X)\r\n";
    if (!origin.empty()) request += "Origin: " + origin + "\r\n";
    return request + "\r\n";
}

// A masked frame, as browsers send them.
std::string ClientFrame(uint8_t opcode, const std::string& payload) {
    std::string frame;
    frame += static_cast<char>(0x80 | opcode);
    frame += static_cast<char>(0x80 | payload.size());
    const char mask[4] = {0x12, 0x34, 0x56, 0x78};
    frame.append(mask, 4);
    for (size_t i = 0; i < payload.size(); ++i) frame += static_cast<char>(payload[i] ^ mask[i & 3]);
    return frame;
}

// A WebSocket client just good enough for the tests.
struct WsClient {
    SOCKET socket = INVALID_SOCKET;
    std::string buffer;
    std::string handshake;

    ~WsClient() {
        if (socket != INVALID_SOCKET) closesocket(socket);
    }

    bool Open(int port, const std::string& target, const std::string& origin = "", int receiveBuffer = 0) {
        socket = Connect(port, receiveBuffer);
        if (socket == INVALID_SOCKET) return false;
        const std::string request = UpgradeRequest(target, port, origin);
        send(socket, request.data(), static_cast<int>(request.size()), 0);
        size_t end;
        while ((end = buffer.find("\r\n\r\n")) == std::string::npos) {
            if (!Receive()) return false;
        }
        handshake = buffer.substr(0, end);
        buffer.erase(0, end + 4);
        return handshake.starts_with("HTTP/1.1 101");
    }

    bool Receive() {
        char chunk[8192];
        const int received = recv(socket, chunk, sizeof(chunk), 0);
        if (received <= 0) return false;
        buffer.append(chunk, static_cast<size_t>(received));
        return true;
    }

    // The next server frame (unmasked), or opcode 0xFF on failure.
    websocket::Frame Next() {
        for (;;) {
            if (buffer.size() >= 2) {
                const auto byte = [&](size_t i) { return static_cast<uint8_t>(buffer[i]); };
                uint64_t length = byte(1) & 0x7F;
                size_t offset = 2;
                if (length == 126 && buffer.size() >= 4) {
                    length = (byte(2) << 8) | byte(3);
                    offset = 4;
                }
                if (length < 126 || offset == 4) {
                    if (buffer.size() >= offset + length) {
                        websocket::Frame frame;
                        frame.opcode = byte(0) & 0x0F;
                        frame.payload = buffer.substr(offset, static_cast<size_t>(length));
                        buffer.erase(0, offset + static_cast<size_t>(length));
                        return frame;
                    }
                }
            }
            if (!Receive()) return {true, 0xFF, {}};
        }
    }

    void SendFrame(uint8_t opcode, const std::string& payload) {
        const std::string frame = ClientFrame(opcode, payload);
        send(socket, frame.data(), static_cast<int>(frame.size()), 0);
    }
};

struct TestServer {
    Server server;

    explicit TestServer(int port = 18765) {
        ServerOptions options;
        options.address = kLoopback;
        options.port = port;
        options.token = kToken;
        options.pcName = L"TEST-PC";
        options.touchIcon = "\x89PNG-test";
        options.allowLoopback = true;
        if (!server.Start(options)) throw Failure{"server did not start"};
    }
    int port() const { return server.Port(); }
    std::string ws() const { return std::string("/ws?t=") + kToken; }
};

template <typename Predicate>
bool WaitFor(Predicate predicate, int milliseconds = 2000) {
    for (int waited = 0; waited < milliseconds; waited += 10) {
        if (predicate()) return true;
        Sleep(10);
    }
    return predicate();
}

std::vector<uint8_t> Float32Frames(const std::vector<float>& samples) {
    std::vector<uint8_t> bytes(samples.size() * 4);
    std::memcpy(bytes.data(), samples.data(), bytes.size());
    return bytes;
}

// --- HTTP pieces ------------------------------------------------------------------

TEST(ParsesRequestHead) {
    const auto request = ParseRequestHead(
        "GET /ws?t=abc HTTP/1.1\r\nHost: 192.168.1.20:8765\r\nUpgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\nOrigin: http://192.168.1.20:8765\r\n"
        "User-Agent: Mozilla/5.0 (iPhone)\r\n");
    CHECK(request);
    CHECK(request->method == "GET");
    CHECK(request->path == "/ws");
    CHECK(request->query == "t=abc");
    CHECK(request->host == "192.168.1.20:8765");
    CHECK(request->origin == "http://192.168.1.20:8765");
    CHECK(request->upgradeWebSocket && request->connectionUpgrade);
    CHECK(request->keepAlive);
    CHECK(IsWebSocketUpgrade(*request));
    CHECK(!ParseRequestHead("GET /x HTTP/1.0\r\n")->keepAlive);
    CHECK(!ParseRequestHead("GET / HTTP/1.1\r\nConnection: close")->keepAlive);
}

TEST(RecognizesOnlyValidUpgrades) {
    const std::string base = "GET /ws HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
    CHECK(IsWebSocketUpgrade(*ParseRequestHead(base + "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13")));
    // Old protocol versions, missing keys and missing Connection: Upgrade.
    CHECK(!IsWebSocketUpgrade(*ParseRequestHead(base + "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 8")));
    CHECK(!IsWebSocketUpgrade(*ParseRequestHead(base + "Sec-WebSocket-Version: 13")));
    CHECK(!IsWebSocketUpgrade(*ParseRequestHead(
        "GET /ws HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13")));
    CHECK(!IsWebSocketUpgrade(*ParseRequestHead(
        "POST /ws HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13")));
}

TEST(RejectsMalformedRequests) {
    CHECK(!ParseRequestHead("GARBAGE"));
    CHECK(!ParseRequestHead("GET nothing HTTP/1.1"));
    CHECK(!ParseRequestHead("GET / SPDY/3"));
    CHECK(!ParseRequestHead("GET / HTTP/1.1\r\nContent-Length: 12x"));
    CHECK(!ParseRequestHead("POST / HTTP/1.1\r\nTransfer-Encoding: chunked"));
}

TEST(DecodesQueries) {
    CHECK(QueryParam("t=abc&x=1", "t") == "abc");
    CHECK(QueryParam("a=1&t=a%20b", "t") == "a b");
    CHECK(QueryParam("flag&t=", "t") == "");
    CHECK(!QueryParam("a=1", "t"));
    CHECK(UrlDecode("%41%zz+") == "A%zz ");
}

TEST(EscapesJson) {
    CHECK(JsonEscape("a\"b\\c\n<") == "a\\\"b\\\\c\\n\\u003c");
    CHECK(JsonEscape(std::string("\x01", 1)) == "\\u0001");
}

TEST(GeneratesTokens) {
    const std::string a = GenerateToken();
    const std::string b = GenerateToken();
    CHECK(a.size() == 32 && IsValidToken(a));
    CHECK(a != b);
    CHECK(TokensEqual(a, a));
    CHECK(!TokensEqual(a, b));
    CHECK(!TokensEqual("", ""));
    CHECK(!IsValidToken("0123"));
    CHECK(!IsValidToken("0123456789abcdef0123456789abcdeg"));
}

TEST(RecognizesPrivateAddresses) {
    CHECK(IsPrivateIPv4(0x0A000001));   // 10.0.0.1
    CHECK(IsPrivateIPv4(0xAC100001));   // 172.16.0.1
    CHECK(IsPrivateIPv4(0xAC1FFFFF));   // 172.31.255.255
    CHECK(!IsPrivateIPv4(0xAC200001));  // 172.32.0.1
    CHECK(IsPrivateIPv4(0xC0A80114));   // 192.168.1.20
    CHECK(IsPrivateIPv4(0xA9FE0101));   // 169.254.1.1
    CHECK(!IsPrivateIPv4(0x08080808));  // 8.8.8.8
    CHECK(!IsPrivateIPv4(0x7F000001));  // 127.0.0.1: not a phone
    CHECK(!IsPrivateIPv4(0xC0A90001));  // 192.169.0.1
    CHECK(FormatIPv4(0xC0A80114) == L"192.168.1.20");
}

TEST(NamesDevicesFromUserAgents) {
    CHECK(DeviceNameFromUserAgent("Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) Safari") == L"iPhone");
    CHECK(DeviceNameFromUserAgent("Mozilla/5.0 (iPad; CPU OS 17_0 like Mac OS X)") == L"iPad");
    CHECK(DeviceNameFromUserAgent("Mozilla/5.0 (Linux; Android 14; K) Chrome/130 Mobile Safari") == L"Android phone");
    CHECK(DeviceNameFromUserAgent("Mozilla/5.0 (Linux; Android 14; K) Chrome/130 Safari") == L"Android tablet");
    CHECK(DeviceNameFromUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64)") == L"Windows PC");
    CHECK(DeviceNameFromUserAgent("") == L"Browser");
}

// --- WebSocket ----------------------------------------------------------------------

TEST(EncodesBase64) {
    CHECK(websocket::Base64Encode("", 0).empty());
    CHECK(websocket::Base64Encode("f", 1) == "Zg==");
    CHECK(websocket::Base64Encode("fo", 2) == "Zm8=");
    CHECK(websocket::Base64Encode("foo", 3) == "Zm9v");
    CHECK(websocket::Base64Encode("foobar", 6) == "Zm9vYmFy");
}

TEST(ComputesTheHandshakeAccept) {
    // The example from RFC 6455, section 1.3.
    CHECK(websocket::AcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    const std::string response = websocket::HandshakeResponse("dGhlIHNhbXBsZSBub25jZQ==");
    CHECK(response.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
    CHECK(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
    CHECK(response.ends_with("\r\n\r\n"));
}

TEST(EncodesFrameLengths) {
    CHECK(websocket::FrameHeader(websocket::kBinary, 0) == std::string("\x82\x00", 2));
    CHECK(websocket::FrameHeader(websocket::kText, 125) == "\x81\x7D");
    CHECK(websocket::FrameHeader(websocket::kBinary, 126) == "\x82\x7E\x00\x7E" ||
          websocket::FrameHeader(websocket::kBinary, 126) == std::string("\x82\x7E\x00\x7E", 4));
    CHECK(websocket::FrameHeader(websocket::kBinary, 1920) == std::string("\x82\x7E\x07\x80", 4));
    CHECK(websocket::FrameHeader(websocket::kBinary, 65535) == std::string("\x82\x7E\xFF\xFF", 4));
    CHECK(websocket::FrameHeader(websocket::kBinary, 65536) == std::string("\x82\x7F\x00\x00\x00\x00\x00\x01\x00\x00", 10));
    const std::string frame = websocket::EncodeFrame(websocket::kText, "hi");
    CHECK(frame == "\x81\x02hi");
}

TEST(ParsesMaskedClientFrames) {
    std::string buffer = ClientFrame(websocket::kPing, "hello");
    // Arrives in two pieces.
    std::string first = buffer.substr(0, 4);
    std::string rest = buffer.substr(4);
    websocket::Frame frame;
    CHECK(websocket::ParseClientFrame(first, frame, 1024) == websocket::ParseResult::NeedMore);
    first += rest;
    CHECK(websocket::ParseClientFrame(first, frame, 1024) == websocket::ParseResult::Complete);
    CHECK(frame.opcode == websocket::kPing && frame.fin && frame.payload == "hello");
    CHECK(first.empty());

    // Two frames back to back.
    std::string two = ClientFrame(websocket::kText, "a") + ClientFrame(websocket::kClose, "");
    CHECK(websocket::ParseClientFrame(two, frame, 1024) == websocket::ParseResult::Complete && frame.payload == "a");
    CHECK(websocket::ParseClientFrame(two, frame, 1024) == websocket::ParseResult::Complete && frame.opcode == websocket::kClose);
}

TEST(RejectsBadClientFrames) {
    websocket::Frame frame;
    std::string unmasked = "\x81\x02hi";
    CHECK(websocket::ParseClientFrame(unmasked, frame, 1024) == websocket::ParseResult::Error);
    std::string tooBig = ClientFrame(websocket::kText, std::string(100, 'x'));
    CHECK(websocket::ParseClientFrame(tooBig, frame, 50) == websocket::ParseResult::Error);
    std::string reserved = ClientFrame(websocket::kText, "x");
    reserved[0] = static_cast<char>(reserved[0] | 0x40);
    CHECK(websocket::ParseClientFrame(reserved, frame, 1024) == websocket::ParseResult::Error);
    std::string fragmentedPing = ClientFrame(websocket::kPing, "x");
    fragmentedPing[0] = static_cast<char>(websocket::kPing);  // FIN clear
    CHECK(websocket::ParseClientFrame(fragmentedPing, frame, 1024) == websocket::ParseResult::Error);
}

// --- PCM ------------------------------------------------------------------------------

TEST(ConvertsFloatToInt16) {
    CHECK(FloatToInt16(0.0f) == 0);
    CHECK(FloatToInt16(1.0f) == 32767);
    CHECK(FloatToInt16(-1.0f) == -32767);
    CHECK(FloatToInt16(0.5f) == 16384);  // rounded, not truncated
    CHECK(FloatToInt16(-0.5f) == -16384);
    // Overs clip instead of wrapping around.
    CHECK(FloatToInt16(1.7f) == 32767);
    CHECK(FloatToInt16(-3.0f) == -32767);
    CHECK(FloatToInt16(std::nanf("")) == 0);
}

TEST(DescribesMixFormats) {
    WAVEFORMATEXTENSIBLE extensible{};
    extensible.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    extensible.Format.nChannels = 2;
    extensible.Format.nSamplesPerSec = 48000;
    extensible.Format.wBitsPerSample = 32;
    extensible.Format.nBlockAlign = 8;
    extensible.Format.nAvgBytesPerSec = 48000 * 8;
    extensible.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    extensible.Samples.wValidBitsPerSample = 32;
    extensible.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    extensible.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    auto format = DescribeFormat(extensible.Format);
    CHECK(format && format->type == SampleType::Float32 && format->rate == 48000 && format->channels == 2);

    extensible.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    extensible.Format.wBitsPerSample = 24;
    extensible.Format.nBlockAlign = 6;
    format = DescribeFormat(extensible.Format);
    CHECK(format && format->type == SampleType::Int24);

    WAVEFORMATEX plain{WAVE_FORMAT_PCM, 1, 44100, 88200, 2, 16, 0};
    format = DescribeFormat(plain);
    CHECK(format && format->type == SampleType::Int16 && format->channels == 1 && format->rate == 44100);

    WAVEFORMATEX odd{WAVE_FORMAT_PCM, 2, 48000, 0, 2, 8, 0};
    CHECK(!DescribeFormat(odd));
}

TEST(ConvertsStereoAndMono) {
    SampleFormat stereo{SampleType::Float32, 48000, 2, 0, 8};
    const auto bytes = Float32Frames({0.25f, -0.25f, 1.5f, -1.5f});
    std::vector<int16_t> out;
    AppendStereo16(bytes.data(), 2, stereo, out);
    CHECK(out.size() == 4);
    CHECK(out[0] == 8192 && out[1] == -8192 && out[2] == 32767 && out[3] == -32767);

    SampleFormat mono{SampleType::Int16, 44100, 1, 0, 2};
    const int16_t samples[] = {1000, -2000};
    out.clear();
    AppendStereo16(reinterpret_cast<const uint8_t*>(samples), 2, mono, out);
    CHECK(out.size() == 4);
    CHECK(out[0] == out[1] && std::abs(out[0] - 1000) <= 1);
    CHECK(out[2] == out[3] && std::abs(out[2] + 2000) <= 1);
}

TEST(ConvertsIntegerFormats) {
    // 24-bit packed: 0x400000 is +0.5, 0xC00000 is -0.5.
    const uint8_t packed24[] = {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0};
    SampleFormat int24{SampleType::Int24, 48000, 2, 0, 6};
    std::vector<int16_t> out;
    AppendStereo16(packed24, 1, int24, out);
    CHECK(out[0] == 16384 && out[1] == -16384);

    const int32_t int32Samples[] = {INT32_MIN, INT32_MAX};
    SampleFormat int32{SampleType::Int32, 48000, 2, 0, 8};
    out.clear();
    AppendStereo16(reinterpret_cast<const uint8_t*>(int32Samples), 1, int32, out);
    CHECK(out[0] == -32767 && out[1] == 32767);
}

TEST(FoldsSurroundDownToStereo) {
    // 5.1: FL FR FC LFE BL BR
    SampleFormat surround{SampleType::Float32, 48000, 6, KSAUDIO_SPEAKER_5POINT1, 24};
    const auto bytes = Float32Frames({0.1f, 0.2f, 0.4f, 0.9f, 0.0f, 0.0f});
    std::vector<int16_t> out;
    AppendStereo16(bytes.data(), 1, surround, out);
    const float left = out[0] / 32767.0f;
    const float right = out[1] / 32767.0f;
    // Centre at -3 dB on both sides, LFE left out.
    CHECK(std::fabs(left - (0.1f + 0.4f * 0.7071f)) < 0.001f);
    CHECK(std::fabs(right - (0.2f + 0.4f * 0.7071f)) < 0.001f);

    // Rear channels go to their own side.
    const auto rear = Float32Frames({0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f});
    out.clear();
    AppendStereo16(rear.data(), 1, surround, out);
    CHECK(out[0] > 10000 && out[1] == 0);
}

TEST(MeasuresPeakLevel) {
    const int16_t samples[] = {0, -16384, 8000, 100};
    CHECK(std::fabs(PeakLevel(samples, 4) - 0.5f) < 0.001f);
    CHECK(PeakLevel(samples, 0) == 0.0f);
}

TEST(CutsTheStreamIntoPackets) {
    std::vector<size_t> sizes;
    std::vector<int16_t> received;
    Packetizer packetizer(480, [&](const int16_t* stereo, size_t frames) {
        sizes.push_back(frames);
        received.insert(received.end(), stereo, stereo + frames * 2);
    });
    std::vector<int16_t> input(1000 * 2);
    for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<int16_t>(i);
    packetizer.Push(input.data(), 300);
    CHECK(sizes.empty());
    packetizer.Push(input.data() + 600, 700);  // frames 300..999
    CHECK(sizes.size() == 2 && sizes[0] == 480 && sizes[1] == 480);
    // Nothing lost or reordered across pushes.
    for (size_t i = 0; i < received.size(); ++i) CHECK(received[i] == static_cast<int16_t>(i));
    packetizer.PushSilence(440);  // 40 left + 440
    CHECK(sizes.size() == 3);
    CHECK(received.back() == 0 && received[960 * 2] == static_cast<int16_t>(960 * 2));
    packetizer.PushSilence(100);
    packetizer.Reset();
    packetizer.PushSilence(479);
    CHECK(sizes.size() == 3);
}

TEST(FillsSilenceOnlyWhenAudioStops) {
    SilenceFill fill(1920);  // 40 ms at 48 kHz
    fill.Start(0);
    // Audio arriving every 10 ms: never any silence.
    for (uint64_t t = 480; t <= 48000; t += 480) {
        fill.OnAudio(t);
        CHECK(fill.Due(t + 100) == 0);
    }
    CHECK(!fill.Filling());
    // Audio stops at 48000. Within tolerance, nothing; then everything since
    // the stop, then just what time adds.
    CHECK(fill.Due(48000 + 1000) == 0);
    CHECK(fill.Due(48000 + 2000) == 2000);
    CHECK(fill.Filling());
    CHECK(fill.Due(48000 + 2480) == 480);
    CHECK(fill.Due(48000 + 2480) == 0);
    // Audio is back: no more silence.
    fill.OnAudio(51000);
    CHECK(!fill.Filling());
    CHECK(fill.Due(51100) == 0);
}

TEST(DescribesTheStream) {
    CHECK(StreamConfigJson(44100, 441, L"Desk \"PC\"") == "{\"rate\":44100,\"channels\":2,\"frame\":441,\"pc\":\"Desk \\\"PC\\\"\"}");
}

// --- Page, QR and settings ----------------------------------------------------------

TEST(PageIsSelfContained) {
    const std::string& page = PageHtml();
    CHECK(page.starts_with("<!doctype html>"));
    CHECK(page.ends_with("</html>\n"));
    CHECK(page.find("http://") == std::string::npos && page.find("https://") == std::string::npos);
    // The tuned playback: one ScriptProcessor reading a ring buffer, no
    // buffer source per packet.
    CHECK(page.find("createScriptProcessor(1024, 1, 2)") != std::string::npos);
    CHECK(page.find("navigator.audioSession.type = 'playback'") != std::string::npos);
    CHECK(page.find("/ws${location.search}") != std::string::npos);
    CHECK(ExpiredPageHtml().find("scan its QR code") != std::string::npos);
}

TEST(EncodesQrCodes) {
    const auto qr = EncodeQrCode(L"http://192.168.1.20:8765/?t=0123456789abcdef0123456789abcdef");
    CHECK(qr && qr->size >= 21 && qr->dark.size() == static_cast<size_t>(qr->size * qr->size));
    CHECK(!EncodeQrCode(L""));
}

TEST(SavesSettings) {
    RegDeleteTreeW(HKEY_CURRENT_USER, kTestSettingsKey);
    Settings defaults = Settings::Load(kTestSettingsKey);
    CHECK(defaults.port == kDefaultPort && defaults.idleStopMinutes == 0 && !defaults.muteSpeakers &&
          defaults.notifyConnections && defaults.token.empty() && defaults.outputId.empty());

    Settings settings;
    settings.port = 9000;
    settings.outputId = L"{0.0.0.00000000}.{abc}";
    settings.idleStopMinutes = 30;
    settings.muteSpeakers = true;
    settings.notifyConnections = false;
    settings.token = kToken;
    settings.mutedOutputId = L"{0.0.0.00000000}.{def}";
    CHECK(settings.Save(kTestSettingsKey));
    const Settings loaded = Settings::Load(kTestSettingsKey);
    CHECK(loaded.port == 9000 && loaded.outputId == settings.outputId && loaded.idleStopMinutes == 30 &&
          loaded.muteSpeakers && !loaded.notifyConnections && loaded.token == kToken && loaded.mutedOutputId == settings.mutedOutputId);

    // Values out of range fall back to the defaults.
    DWORD badPort = 80, badIdle = 7;
    RegSetKeyValueW(HKEY_CURRENT_USER, kTestSettingsKey, L"Port", REG_DWORD, &badPort, sizeof(badPort));
    RegSetKeyValueW(HKEY_CURRENT_USER, kTestSettingsKey, L"IdleStopMinutes", REG_DWORD, &badIdle, sizeof(badIdle));
    RegSetKeyValueW(HKEY_CURRENT_USER, kTestSettingsKey, L"Token", REG_SZ, L"short", 12);
    const Settings fixed = Settings::Load(kTestSettingsKey);
    CHECK(fixed.port == kDefaultPort && fixed.idleStopMinutes == 0 && fixed.token.empty());
    RegDeleteTreeW(HKEY_CURRENT_USER, kTestSettingsKey);
}

TEST(TogglesAutostart) {
    SetAutostartEnabled(false, L"", kTestRunValue);
    CHECK(!IsAutostartEnabled(kTestRunValue));
    CHECK(SetAutostartEnabled(true, L"C:\\Apps\\Lazo.exe", kTestRunValue));
    CHECK(IsAutostartEnabled(kTestRunValue));
    wchar_t command[512]{};
    DWORD size = sizeof(command);
    RegGetValueW(HKEY_CURRENT_USER, kRunKey, kTestRunValue, RRF_RT_REG_SZ, nullptr, command, &size);
    CHECK(std::wstring(command) == L"\"C:\\Apps\\Lazo.exe\" --background");
    CHECK(SetAutostartEnabled(false, L"", kTestRunValue));
    CHECK(!IsAutostartEnabled(kTestRunValue));
}

// --- Server -------------------------------------------------------------------------

TEST(ServerRequiresTheToken) {
    TestServer test;
    const Response page = Request(test.port(), Get(std::string("/?t=") + kToken));
    CHECK(page.status == 200);
    CHECK(page.body == PageHtml());
    CHECK(page.head.find("Content-Security-Policy:") != std::string::npos);
    CHECK(Request(test.port(), Get("/")).status == 403);
    CHECK(Request(test.port(), Get("/?t=ffffffffffffffffffffffffffffffff")).status == 403);
    CHECK(Request(test.port(), Get(std::string("/api/ping?t=") + kToken)).status == 204);
    CHECK(Request(test.port(), Get("/api/ping?t=old")).status == 403);
    CHECK(Request(test.port(), Get("/nothing")).status == 404);
    // The home screen icon needs no token.
    const Response icon = Request(test.port(), Get("/apple-touch-icon.png"));
    CHECK(icon.status == 200 && icon.body == "\x89PNG-test");
}

TEST(ServerRefusesBadUpgrades) {
    TestServer test;
    CHECK(Request(test.port(), UpgradeRequest("/ws?t=nope", test.port())).status == 403);
    CHECK(Request(test.port(), Get(test.ws())).status == 400);  // no upgrade headers
    // Another website open on the phone cannot connect, even with the token.
    CHECK(Request(test.port(), UpgradeRequest(test.ws(), test.port(), "http://evil.example")).status == 403);
    CHECK(test.server.ListenerCount() == 0);
}

TEST(ServerStreamsToListeners) {
    TestServer test;
    WsClient phone;
    CHECK(phone.Open(test.port(), test.ws(), "http://127.0.0.1:" + std::to_string(test.port())));
    CHECK(phone.handshake.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
    CHECK(WaitFor([&] { return test.server.ListenerCount() == 1; }));
    const auto listeners = test.server.Listeners();
    CHECK(listeners.size() == 1 && listeners[0].device == L"iPhone" && listeners[0].address == kLoopback);

    // No format yet: audio is held back until the page knows what it is.
    std::vector<int16_t> packet(480 * 2);
    for (size_t i = 0; i < packet.size(); ++i) packet[i] = static_cast<int16_t>(i * 7 - 3000);
    test.server.Broadcast(packet.data(), 480);
    test.server.SetFormat(48000, 480);
    websocket::Frame config = phone.Next();
    CHECK(config.opcode == websocket::kText);
    CHECK(config.payload == "{\"rate\":48000,\"channels\":2,\"frame\":480,\"pc\":\"TEST-PC\"}");

    test.server.Broadcast(packet.data(), 480);
    const websocket::Frame audio = phone.Next();
    CHECK(audio.opcode == websocket::kBinary);
    CHECK(audio.payload.size() == 1920);
    CHECK(std::memcmp(audio.payload.data(), packet.data(), 1920) == 0);

    // The same format again changes nothing; a new one is announced.
    test.server.SetFormat(48000, 480);
    test.server.SetFormat(44100, 441);
    CHECK(phone.Next().payload.find("\"rate\":44100") != std::string::npos);

    // Pings get pongs; a close is answered and the listener is gone.
    phone.SendFrame(websocket::kPing, "hi");
    websocket::Frame pong = phone.Next();
    CHECK(pong.opcode == websocket::kPong && pong.payload == "hi");
    phone.SendFrame(websocket::kClose, "");
    CHECK(phone.Next().opcode == websocket::kClose);
    CHECK(WaitFor([&] { return test.server.ListenerCount() == 0; }));
}

TEST(ServerSendsTheFormatToLateListeners) {
    TestServer test;
    test.server.SetFormat(48000, 480);
    WsClient phone;
    CHECK(phone.Open(test.port(), test.ws()));
    const websocket::Frame config = phone.Next();
    CHECK(config.opcode == websocket::kText && config.payload.find("\"rate\":48000") != std::string::npos);
}

TEST(ServerSkipsPacketsForSlowListeners) {
    TestServer test;
    test.server.SetFormat(48000, 480);
    WsClient phone;
    // A small receive window, so the PC's socket backs up quickly.
    CHECK(phone.Open(test.port(), test.ws(), "", 4096));
    CHECK(WaitFor([&] { return test.server.ListenerCount() == 1; }));
    std::vector<int16_t> packet(480 * 2, 1234);
    // Ten seconds of audio sent at once while the phone reads nothing.
    for (int i = 0; i < 1000; ++i) test.server.Broadcast(packet.data(), 480);
    CHECK(WaitFor([&] { return !test.server.Listeners().empty() && test.server.Listeners()[0].dropped > 0; }));
    // What did arrive is whole frames, not a pile-up of seconds of delay.
    size_t frames = 0;
    phone.Next();  // config
    const DWORD shortTimeout = 300;
    setsockopt(phone.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&shortTimeout), sizeof(shortTimeout));
    while (phone.Next().opcode == websocket::kBinary) ++frames;
    CHECK(frames > 0 && frames < 1000);
}

TEST(ServerStopsAndDisconnects) {
    TestServer test;
    WsClient phone;
    CHECK(phone.Open(test.port(), test.ws()));
    CHECK(WaitFor([&] { return test.server.ListenerCount() == 1; }));
    const int port = test.port();
    test.server.Stop();
    CHECK(!test.server.IsRunning());
    CHECK(phone.Next().opcode == 0xFF);
    CHECK(Request(port, Get(std::string("/?t=") + kToken)).status == 0);
}

TEST(ServerRejectsNonPrivatePeers) {
    Server server;
    ServerOptions options;
    options.address = kLoopback;
    options.port = 18790;
    options.token = kToken;
    CHECK(server.Start(options));  // loopback not allowed: 127.0.0.1 is not a phone
    CHECK(Request(server.Port(), Get(std::string("/?t=") + kToken)).status == 0);
}

TEST(ServerMovesToTheNextFreePort) {
    TestServer first(18800);
    TestServer second(18800);
    CHECK(first.port() == 18800);
    CHECK(second.port() == 18801);
}

TEST(ServerTracksListeningTime) {
    TestServer test;
    const ULONGLONG before = test.server.LastListening();
    WsClient phone;
    CHECK(phone.Open(test.port(), test.ws()));
    CHECK(WaitFor([&] { return test.server.ListenerCount() == 1; }));
    Sleep(50);
    CHECK(test.server.LastListening() >= before + 40);
    closesocket(phone.socket);
    phone.socket = INVALID_SOCKET;
    CHECK(WaitFor([&] { return test.server.ListenerCount() == 0; }));
}

// --- Capture ------------------------------------------------------------------------

TEST(CapturesTheDefaultOutput) {
    if (DefaultOutputId().empty()) throw Skip{"no audio output on this PC"};
    std::atomic<size_t> packets{0};
    std::atomic<size_t> badSizes{0};
    std::atomic<uint32_t> rate{0};
    std::atomic<size_t> framesPerPacket{0};
    Capture capture;
    capture.Start(
        L"",
        [&](const int16_t*, size_t frames) {
            if (frames != framesPerPacket) ++badSizes;
            ++packets;
        },
        [&](uint32_t r, size_t f) {
            framesPerPacket = f;
            rate = r;
        });
    // Silence included, 10 ms packets keep coming: about 100 per second.
    Sleep(1000);
    const size_t count = packets;
    const std::wstring name = capture.DeviceName();
    capture.Stop();
    CHECK(rate >= 8000 && framesPerPacket == rate / 100);
    CHECK(badSizes == 0);
    CHECK(count >= 80 && count <= 115);
    CHECK(!name.empty());
    CHECK(!capture.IsRunning() && capture.DeviceName().empty());
}

TEST(ListsOutputs) {
    const auto outputs = EnumerateOutputs();
    const std::wstring defaultId = DefaultOutputId();
    if (defaultId.empty()) throw Skip{"no audio output on this PC"};
    bool foundDefault = false;
    for (const auto& output : outputs) {
        CHECK(!output.id.empty());
        if (output.id == defaultId) foundDefault = true;
    }
    CHECK(foundDefault);
    CHECK(!OutputName(defaultId).empty());
}

}  // namespace

// --- Runner -----------------------------------------------------------------------------

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);

    int passed = 0, failed = 0, skipped = 0;
    for (const auto& test : Tests()) {
        try {
            test.run();
            ++passed;
            std::cout << "PASS " << test.name << "\n";
        } catch (const Skip& skip) {
            ++skipped;
            std::cout << "SKIP " << test.name << ": " << skip.reason << "\n";
        } catch (const Failure& failure) {
            ++failed;
            std::cout << "FAIL " << test.name << ": " << failure.message << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "FAIL " << test.name << ": exception: " << e.what() << "\n";
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed, " << skipped << " skipped\n";
    WSACleanup();
    CoUninitialize();
    return failed == 0 ? 0 : 1;
}
