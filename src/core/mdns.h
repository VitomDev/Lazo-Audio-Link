#pragma once

// A minimal multicast DNS responder (RFC 6762) for one name: it answers
// "lazo-<pc>.local" with the PC's address, so the phone's link keeps
// working when the router gives the PC another IP. Phones, Macs and Windows
// resolve .local names this way.

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace lazo {

// "lazo-desktop-12ab" from "DESKTOP-12AB": a valid DNS label, lower case.
std::string LinkHostLabel(std::wstring_view computerName);

namespace mdns {

struct Question {
    std::string name;  // dotted, without the final dot: "lazo-pc.local"
    uint16_t type = 0;
    bool unicastResponse = false;  // the QU bit
};

struct Query {
    uint16_t id = 0;
    std::vector<Question> questions;
};

// Reads the questions of a query; responses and broken packets give nothing.
std::optional<Query> ParseQuery(const uint8_t* data, size_t size);

// An authoritative answer with one A record. Legacy (non-5353) queries get
// their id and question echoed back, as RFC 6762 section 6.7 asks.
std::vector<uint8_t> BuildAnswer(std::string_view name, uint32_t address, uint16_t id, bool echoQuestion);

bool NameEquals(std::string_view a, std::string_view b);

}  // namespace mdns

class MdnsResponder {
public:
    MdnsResponder() = default;
    ~MdnsResponder() { Stop(); }
    MdnsResponder(const MdnsResponder&) = delete;
    MdnsResponder& operator=(const MdnsResponder&) = delete;

    // Answers `name` (e.g. "lazo-pc.local") with `address` (host byte order),
    // listening on that address's interface only.
    bool Start(const std::string& name, uint32_t address);
    void Stop();
    bool IsRunning() const { return thread_.joinable(); }

private:
    void Run();
    void Send(const std::vector<uint8_t>& packet, const sockaddr_in& to);

    std::string name_;
    uint32_t address_ = 0;
    SOCKET socket_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace lazo
