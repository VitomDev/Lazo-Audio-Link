#pragma once

// Small, dependency-free pieces of the HTTP server, kept apart so they can be
// tested without sockets.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lazo {

struct RequestHead {
    std::string method;
    std::string path;   // decoded, without the query
    std::string query;  // raw, without the '?'
    std::string host;
    std::string origin;
    std::string userAgent;
    // WebSocket upgrade (RFC 6455): all of these must be right to upgrade.
    bool upgradeWebSocket = false;  // "Upgrade: websocket"
    bool connectionUpgrade = false; // "Connection" lists "upgrade"
    std::string webSocketKey;
    std::string webSocketVersion;
    uint64_t contentLength = 0;
    bool keepAlive = true;
};

// Parses the request line and headers (everything before the blank line).
std::optional<RequestHead> ParseRequestHead(std::string_view head);

// True when the request asks for a WebSocket this server can accept.
bool IsWebSocketUpgrade(const RequestHead& request);

// Percent-decoding; '+' becomes a space when `plusIsSpace` (query strings).
std::string UrlDecode(std::string_view text, bool plusIsSpace = true);

// The decoded value of `name` in a query string, if present.
std::optional<std::string> QueryParam(std::string_view query, std::string_view name);

std::string JsonEscape(std::string_view text);

// 32 random hex characters from the system's cryptographic generator.
std::string GenerateToken();
bool IsValidToken(std::string_view token);
// Compares in constant time, so the token cannot be guessed byte by byte.
bool TokensEqual(std::string_view a, std::string_view b);

// IPv4 address in host byte order.
bool IsPrivateIPv4(uint32_t address);
std::wstring FormatIPv4(uint32_t address);

// "iPhone", "Android phone"... a short name for the PC window, from the
// browser's User-Agent.
std::wstring DeviceNameFromUserAgent(std::string_view userAgent);

}  // namespace lazo
