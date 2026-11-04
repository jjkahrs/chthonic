// Frame codec: u16 payload length + u16 opcode + payload, little-endian.
#pragma once
#include "messages.h"

#include <span>

namespace chthonic::proto {

enum class DecodeStatus { ok, need_more, too_large, unknown_opcode, malformed };

struct Header {
    uint16_t length = 0;
    Opcode opcode{};
};

struct Decoded {
    DecodeStatus status = DecodeStatus::need_more;
    Message msg;
    size_t consumed = 0; // bytes of buf used by this frame (0 unless ok)
};

Opcode opcode_of(const Message& m);
const char* opcode_name(Opcode op);

// Appends one complete frame to out.
void encode(std::vector<uint8_t>& out, const Message& m);
std::vector<uint8_t> encode(const Message& m);

// Reads the 4-byte header; bytes must hold at least kHeaderSize.
Header parse_header(std::span<const uint8_t> bytes);
// Decodes a payload whose length was already checked against the frame limit.
DecodeStatus decode_payload(Opcode op, std::span<const uint8_t> payload, Message& out);
// Decodes the first frame in buf. too_large is reported from the header alone, before the payload arrives.
Decoded decode_frame(std::span<const uint8_t> buf, size_t maxPayload);

} // namespace chthonic::proto
