#include "core/mdns.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>

namespace lazo {

namespace {

constexpr uint16_t kPort = 5353;
constexpr uint32_t kGroup = 0xE00000FB;  // 224.0.0.251
constexpr uint16_t kTypeA = 1;
constexpr uint16_t kTypeAny = 255;
constexpr uint16_t kClassIn = 1;
constexpr uint32_t kTtlSeconds = 120;

uint16_t Read16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

void Put16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void Put32(std::vector<uint8_t>& out, uint32_t value) {
    Put16(out, static_cast<uint16_t>(value >> 16));
    Put16(out, static_cast<uint16_t>(value));
}

void PutName(std::vector<uint8_t>& out, std::string_view name) {
    while (!name.empty()) {
        const size_t dot = name.find('.');
        const std::string_view label = name.substr(0, dot);
        out.push_back(static_cast<uint8_t>(std::min<size_t>(label.size(), 63)));
        out.insert(out.end(), label.begin(), label.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(label.size(), 63)));
        if (dot == std::string_view::npos) break;
        name.remove_prefix(dot + 1);
    }
    out.push_back(0);
}

// Reads a possibly compressed name at `offset`; moves `offset` past it.
bool ReadName(const uint8_t* data, size_t size, size_t& offset, std::string& name) {
    name.clear();
    size_t position = offset;
    bool jumped = false;
    for (int hops = 0; hops < 32; ++hops) {
        if (position >= size) return false;
        const uint8_t length = data[position];
        if (length == 0) {
            if (!jumped) offset = position + 1;
            return true;
        }
        if ((length & 0xC0) == 0xC0) {
            if (position + 1 >= size) return false;
            const size_t target = static_cast<size_t>(((length & 0x3F) << 8) | data[position + 1]);
            if (!jumped) offset = position + 2;
            jumped = true;
            position = target;
            continue;
        }
        if ((length & 0xC0) != 0 || position + 1 + length > size) return false;
        if (!name.empty()) name += '.';
        name.append(reinterpret_cast<const char*>(data + position + 1), length);
        position += 1 + static_cast<size_t>(length);
        if (name.size() > 255) return false;
    }
    return false;
}

}  // namespace

std::string LinkHostLabel(std::wstring_view computerName) {
    std::string label = "lazo-";
    bool lastWasHyphen = true;
    for (const wchar_t c : computerName) {
        if (c < 0x80 && std::isalnum(static_cast<unsigned char>(c))) {
            label += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            lastWasHyphen = false;
        } else if (!lastWasHyphen) {
            label += '-';
            lastWasHyphen = true;
        }
    }
    while (label.back() == '-') label.pop_back();
    if (label == "lazo") label = "lazo-pc";
    return label.substr(0, 63);
}

namespace mdns {

bool NameEquals(std::string_view a, std::string_view b) {
    if (!a.empty() && a.back() == '.') a.remove_suffix(1);
    if (!b.empty() && b.back() == '.') b.remove_suffix(1);
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::optional<Query> ParseQuery(const uint8_t* data, size_t size) {
    if (size < 12) return std::nullopt;
    const uint16_t flags = Read16(data + 2);
    if (flags & 0x8000) return std::nullopt;  // a response, not a query
    if ((flags >> 11) & 0x0F) return std::nullopt;  // only standard queries
    Query query;
    query.id = Read16(data);
    const uint16_t count = Read16(data + 4);
    size_t offset = 12;
    for (uint16_t i = 0; i < count && i < 64; ++i) {
        Question question;
        if (!ReadName(data, size, offset, question.name) || offset + 4 > size) return std::nullopt;
        question.type = Read16(data + offset);
        const uint16_t questionClass = Read16(data + offset + 2);
        question.unicastResponse = (questionClass & 0x8000) != 0;
        offset += 4;
        if ((questionClass & 0x7FFF) == kClassIn) query.questions.push_back(std::move(question));
    }
    return query;
}

std::vector<uint8_t> BuildAnswer(std::string_view name, uint32_t address, uint16_t id, bool echoQuestion) {
    std::vector<uint8_t> out;
    Put16(out, echoQuestion ? id : 0);
    Put16(out, 0x8400);  // response, authoritative
    Put16(out, echoQuestion ? 1 : 0);
    Put16(out, 1);
    Put16(out, 0);
    Put16(out, 0);
    if (echoQuestion) {
        PutName(out, name);
        Put16(out, kTypeA);
        Put16(out, kClassIn);
    }
    PutName(out, name);
    Put16(out, kTypeA);
    // Cache-flush: this is the only address for the name. Legacy answers
    // must not carry the bit.
    Put16(out, echoQuestion ? kClassIn : static_cast<uint16_t>(0x8000 | kClassIn));
    Put32(out, echoQuestion ? 10 : kTtlSeconds);
    Put16(out, 4);
    Put32(out, address);
    return out;
}

}  // namespace mdns

bool MdnsResponder::Start(const std::string& name, uint32_t address) {
    Stop();
    name_ = name;
    address_ = address;
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) return false;
    // Windows' own mDNS service shares the port.
    BOOL reuse = TRUE;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(kPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    ip_mreq membership{};
    membership.imr_multiaddr.s_addr = htonl(kGroup);
    membership.imr_interface.s_addr = htonl(address);
    in_addr interfaceAddress{};
    interfaceAddress.s_addr = htonl(address);
    DWORD ttl = 255;
    if (bind(socket_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0 ||
        setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&membership), sizeof(membership)) != 0 ||
        setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&interfaceAddress), sizeof(interfaceAddress)) != 0) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }
    setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    running_ = true;
    thread_ = std::thread(&MdnsResponder::Run, this);
    return true;
}

void MdnsResponder::Stop() {
    if (!thread_.joinable()) return;
    running_ = false;
    closesocket(socket_);
    socket_ = INVALID_SOCKET;
    thread_.join();
}

void MdnsResponder::Send(const std::vector<uint8_t>& packet, const sockaddr_in& to) {
    sendto(socket_, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

void MdnsResponder::Run() {
    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(kPort);
    group.sin_addr.s_addr = htonl(kGroup);
    // Announce the name once, so caches pick up a new address at once.
    Send(mdns::BuildAnswer(name_, address_, 0, false), group);

    uint8_t buffer[1500];
    while (running_) {
        sockaddr_in from{};
        int fromSize = sizeof(from);
        const int received = recvfrom(socket_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                                      reinterpret_cast<sockaddr*>(&from), &fromSize);
        if (received <= 0) {
            if (!running_) return;
            continue;
        }
        const auto query = mdns::ParseQuery(buffer, static_cast<size_t>(received));
        if (!query) continue;
        for (const auto& question : query->questions) {
            if ((question.type != kTypeA && question.type != kTypeAny) || !mdns::NameEquals(question.name, name_)) continue;
            const bool legacy = ntohs(from.sin_port) != kPort;
            if (legacy) {
                Send(mdns::BuildAnswer(name_, address_, query->id, true), from);
            } else {
                Send(mdns::BuildAnswer(name_, address_, 0, false), question.unicastResponse ? from : group);
            }
            break;
        }
    }
}

}  // namespace lazo
