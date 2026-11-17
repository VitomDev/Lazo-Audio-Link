#include "core/http.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace lazo {

namespace {

bool EqualsNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool ContainsNoCase(std::string_view text, std::string_view part) {
    if (part.size() > text.size()) return false;
    for (size_t i = 0; i + part.size() <= text.size(); ++i) {
        if (EqualsNoCase(text.substr(i, part.size()), part)) return true;
    }
    return false;
}

std::string_view TrimSpaces(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

// "keep-alive, Upgrade" contains the token "upgrade".
bool ListHasToken(std::string_view list, std::string_view token) {
    while (!list.empty()) {
        const size_t comma = list.find(',');
        if (EqualsNoCase(TrimSpaces(list.substr(0, comma)), token)) return true;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::optional<RequestHead> ParseRequestHead(std::string_view head) {
    RequestHead request;
    size_t lineEnd = head.find("\r\n");
    const std::string_view requestLine = head.substr(0, lineEnd);

    const size_t firstSpace = requestLine.find(' ');
    if (firstSpace == std::string_view::npos) return std::nullopt;
    const size_t secondSpace = requestLine.find(' ', firstSpace + 1);
    if (secondSpace == std::string_view::npos) return std::nullopt;
    request.method = requestLine.substr(0, firstSpace);
    const std::string_view target = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    const std::string_view version = requestLine.substr(secondSpace + 1);
    if (target.empty() || target.front() != '/' || !version.starts_with("HTTP/1.")) return std::nullopt;
    request.keepAlive = version != "HTTP/1.0";

    const size_t question = target.find('?');
    request.path = UrlDecode(target.substr(0, question), false);
    if (question != std::string_view::npos) request.query = target.substr(question + 1);

    while (lineEnd != std::string_view::npos) {
        const size_t start = lineEnd + 2;
        lineEnd = head.find("\r\n", start);
        const std::string_view line = head.substr(start, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - start);
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string_view name = TrimSpaces(line.substr(0, colon));
        const std::string_view value = TrimSpaces(line.substr(colon + 1));
        if (EqualsNoCase(name, "Content-Length")) {
            uint64_t length = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), length);
            if (result.ec != std::errc() || result.ptr != value.data() + value.size()) return std::nullopt;
            request.contentLength = length;
        } else if (EqualsNoCase(name, "Transfer-Encoding")) {
            // The page never sends bodies; refuse rather than misread one.
            return std::nullopt;
        } else if (EqualsNoCase(name, "Connection")) {
            if (ListHasToken(value, "close")) request.keepAlive = false;
            if (ListHasToken(value, "keep-alive")) request.keepAlive = true;
            if (ListHasToken(value, "upgrade")) request.connectionUpgrade = true;
        } else if (EqualsNoCase(name, "Upgrade")) {
            request.upgradeWebSocket = EqualsNoCase(value, "websocket");
        } else if (EqualsNoCase(name, "Sec-WebSocket-Key")) {
            request.webSocketKey = value;
        } else if (EqualsNoCase(name, "Sec-WebSocket-Version")) {
            request.webSocketVersion = value;
        } else if (EqualsNoCase(name, "Host")) {
            request.host = value;
        } else if (EqualsNoCase(name, "Origin")) {
            request.origin = value;
        } else if (EqualsNoCase(name, "User-Agent")) {
            request.userAgent = value;
        }
    }
    return request;
}

bool IsWebSocketUpgrade(const RequestHead& request) {
    // The key is 16 random bytes in base64: always 24 characters.
    return request.method == "GET" && request.upgradeWebSocket && request.connectionUpgrade &&
           request.webSocketVersion == "13" && request.webSocketKey.size() == 24;
}

std::string UrlDecode(std::string_view text, bool plusIsSpace) {
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '%' && i + 2 < text.size()) {
            const int high = HexValue(text[i + 1]);
            const int low = HexValue(text[i + 2]);
            if (high >= 0 && low >= 0) {
                result += static_cast<char>(high * 16 + low);
                i += 2;
                continue;
            }
        }
        result += plusIsSpace && c == '+' ? ' ' : c;
    }
    return result;
}

std::optional<std::string> QueryParam(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        const size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        const size_t equals = pair.find('=');
        if (UrlDecode(pair.substr(0, equals)) == name) {
            return equals == std::string_view::npos ? std::string() : UrlDecode(pair.substr(equals + 1));
        }
        if (amp == std::string_view::npos) break;
        query.remove_prefix(amp + 1);
    }
    return std::nullopt;
}

std::string JsonEscape(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case '<': result += "\\u003c"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
                    result += escaped;
                } else {
                    result += c;
                }
        }
    }
    return result;
}

std::string GenerateToken() {
    uint8_t bytes[16];
    BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string token;
    for (const uint8_t byte : bytes) {
        token += kHex[byte >> 4];
        token += kHex[byte & 15];
    }
    SecureZeroMemory(bytes, sizeof(bytes));
    return token;
}

bool IsValidToken(std::string_view token) {
    return token.size() == 32 && std::all_of(token.begin(), token.end(), [](char c) { return HexValue(c) >= 0; });
}

bool TokensEqual(std::string_view a, std::string_view b) {
    if (a.size() != b.size() || a.empty()) return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < a.size(); ++i) difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    return difference == 0;
}

bool IsPrivateIPv4(uint32_t address) {
    const uint32_t a = address >> 24;
    const uint32_t b = (address >> 16) & 0xFF;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
           // Link-local: direct links without DHCP.
           (a == 169 && b == 254);
}

std::wstring FormatIPv4(uint32_t address) {
    return std::to_wstring(address >> 24) + L"." + std::to_wstring((address >> 16) & 0xFF) + L"." +
           std::to_wstring((address >> 8) & 0xFF) + L"." + std::to_wstring(address & 0xFF);
}

std::optional<ListenerStats> ParseListenerStats(std::string_view json) {
    const auto number = [&](std::string_view key) -> std::optional<int> {
        const std::string quoted = "\"" + std::string(key) + "\":";
        const size_t at = json.find(quoted);
        if (at == std::string_view::npos) return std::nullopt;
        std::string_view rest = json.substr(at + quoted.size());
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        double value = 0;
        const auto result = std::from_chars(rest.data(), rest.data() + rest.size(), value);
        if (result.ec != std::errc() || !(value >= 0) || value > 1e7) return std::nullopt;
        return static_cast<int>(value + 0.5);
    };
    if (json.empty() || json.front() != '{') return std::nullopt;
    const auto buffer = number("buffer");
    if (!buffer) return std::nullopt;
    ListenerStats stats;
    stats.bufferMs = *buffer;
    stats.outputMs = number("output").value_or(0);
    stats.dropouts = number("dropouts").value_or(0);
    stats.skips = number("skips").value_or(0);
    return stats;
}

std::wstring DeviceNameFromUserAgent(std::string_view userAgent) {
    // Order matters: iPad's Safari may claim to be a Mac, Android ones Linux.
    if (ContainsNoCase(userAgent, "iPhone")) return L"iPhone";
    if (ContainsNoCase(userAgent, "iPad")) return L"iPad";
    if (ContainsNoCase(userAgent, "Android")) {
        return ContainsNoCase(userAgent, "Mobile") ? L"Android phone" : L"Android tablet";
    }
    if (ContainsNoCase(userAgent, "Macintosh")) return L"Mac";
    if (ContainsNoCase(userAgent, "Windows")) return L"Windows PC";
    if (ContainsNoCase(userAgent, "CrOS")) return L"Chromebook";
    if (ContainsNoCase(userAgent, "Linux")) return L"Linux PC";
    return L"Browser";
}

}  // namespace lazo
