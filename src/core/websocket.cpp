#include "core/websocket.h"

#include <windows.h>
#include <bcrypt.h>

namespace lazo::websocket {

namespace {

constexpr char kHandshakeGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

bool Sha1(std::string_view data, uint8_t (&digest)[20]) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0))) return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0)) &&
              BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                            static_cast<ULONG>(data.size()), 0)) &&
              BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

}  // namespace

std::string Base64Encode(const void* data, size_t size) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::string result;
    result.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= size; i += 3) {
        const uint32_t group = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        result += kAlphabet[(group >> 18) & 63];
        result += kAlphabet[(group >> 12) & 63];
        result += kAlphabet[(group >> 6) & 63];
        result += kAlphabet[group & 63];
    }
    if (i + 1 == size) {
        const uint32_t group = bytes[i] << 16;
        result += kAlphabet[(group >> 18) & 63];
        result += kAlphabet[(group >> 12) & 63];
        result += "==";
    } else if (i + 2 == size) {
        const uint32_t group = (bytes[i] << 16) | (bytes[i + 1] << 8);
        result += kAlphabet[(group >> 18) & 63];
        result += kAlphabet[(group >> 12) & 63];
        result += kAlphabet[(group >> 6) & 63];
        result += '=';
    }
    return result;
}

std::string AcceptKey(std::string_view clientKey) {
    uint8_t digest[20]{};
    if (!Sha1(std::string(clientKey) + kHandshakeGuid, digest)) return {};
    return Base64Encode(digest, sizeof(digest));
}

std::string HandshakeResponse(std::string_view clientKey) {
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " +
           AcceptKey(clientKey) + "\r\n\r\n";
}

std::string FrameHeader(Opcode opcode, uint64_t payloadSize) {
    std::string header;
    header += static_cast<char>(0x80 | opcode);
    if (payloadSize < 126) {
        header += static_cast<char>(payloadSize);
    } else if (payloadSize <= 0xFFFF) {
        header += static_cast<char>(126);
        header += static_cast<char>((payloadSize >> 8) & 0xFF);
        header += static_cast<char>(payloadSize & 0xFF);
    } else {
        header += static_cast<char>(127);
        for (int shift = 56; shift >= 0; shift -= 8) header += static_cast<char>((payloadSize >> shift) & 0xFF);
    }
    return header;
}

std::string EncodeFrame(Opcode opcode, std::string_view payload) {
    std::string frame = FrameHeader(opcode, payload.size());
    frame.append(payload);
    return frame;
}

ParseResult ParseClientFrame(std::string& buffer, Frame& frame, size_t maxPayload) {
    if (buffer.size() < 2) return ParseResult::NeedMore;
    const auto byte = [&](size_t i) { return static_cast<uint8_t>(buffer[i]); };
    const bool fin = (byte(0) & 0x80) != 0;
    const uint8_t opcode = byte(0) & 0x0F;
    if (byte(0) & 0x70) return ParseResult::Error;  // No extensions were agreed.
    if (!(byte(1) & 0x80)) return ParseResult::Error;  // Browsers must mask.
    uint64_t length = byte(1) & 0x7F;
    size_t offset = 2;
    if (length == 126) {
        if (buffer.size() < 4) return ParseResult::NeedMore;
        length = (static_cast<uint64_t>(byte(2)) << 8) | byte(3);
        offset = 4;
    } else if (length == 127) {
        if (buffer.size() < 10) return ParseResult::NeedMore;
        length = 0;
        for (size_t i = 2; i < 10; ++i) length = (length << 8) | byte(i);
        offset = 10;
    }
    const bool control = (opcode & 0x08) != 0;
    if (control && (length > 125 || !fin)) return ParseResult::Error;
    if (length > maxPayload) return ParseResult::Error;
    if (buffer.size() < offset + 4 + length) return ParseResult::NeedMore;

    const uint8_t mask[4] = {byte(offset), byte(offset + 1), byte(offset + 2), byte(offset + 3)};
    offset += 4;
    frame.fin = fin;
    frame.opcode = opcode;
    frame.payload.assign(buffer, offset, static_cast<size_t>(length));
    for (size_t i = 0; i < frame.payload.size(); ++i) frame.payload[i] = static_cast<char>(frame.payload[i] ^ mask[i & 3]);
    buffer.erase(0, offset + static_cast<size_t>(length));
    return ParseResult::Complete;
}

}  // namespace lazo::websocket
