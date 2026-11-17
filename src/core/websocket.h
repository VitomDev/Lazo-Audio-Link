#pragma once

// The parts of RFC 6455 Lazo needs: the opening handshake and frames. The PC
// only sends unmasked frames; the browser's frames are always masked.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lazo::websocket {

enum Opcode : uint8_t {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA,
};

std::string Base64Encode(const void* data, size_t size);

// Sec-WebSocket-Accept for a client's Sec-WebSocket-Key: SHA-1 of the key and
// the RFC's GUID, in base64.
std::string AcceptKey(std::string_view clientKey);

// The 101 Switching Protocols response that completes the handshake.
std::string HandshakeResponse(std::string_view clientKey);

// A whole, unmasked frame from the server (FIN set).
std::string EncodeFrame(Opcode opcode, std::string_view payload);
// Just its header, for payloads written right after it.
std::string FrameHeader(Opcode opcode, uint64_t payloadSize);

struct Frame {
    bool fin = true;
    uint8_t opcode = 0;
    std::string payload;  // unmasked
};

enum class ParseResult { NeedMore, Complete, Error };

// Takes one frame off the front of `buffer` (bytes received from the
// browser). Unmasked frames, frames larger than `maxPayload` and broken
// control frames are errors: the connection must be closed.
ParseResult ParseClientFrame(std::string& buffer, Frame& frame, size_t maxPayload);

}  // namespace lazo::websocket
