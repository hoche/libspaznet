#include <libspaznet/quic/varint.hpp>

#include <stdexcept>

namespace spaznet {
namespace quic {

namespace {

constexpr uint64_t k1ByteMax = (1ULL << 6) - 1;
constexpr uint64_t k2ByteMax = (1ULL << 14) - 1;
constexpr uint64_t k4ByteMax = (1ULL << 30) - 1;

constexpr uint8_t kPrefix2 = 0x40;
constexpr uint8_t kPrefix4 = 0x80;
constexpr uint8_t kPrefix8 = 0xC0;

} // namespace

auto VarInt::encoded_size(uint64_t value) -> std::size_t {
    if (value <= k1ByteMax) {
        return 1;
    }
    if (value <= k2ByteMax) {
        return 2;
    }
    if (value <= k4ByteMax) {
        return 4;
    }
    if (value <= kMax) {
        return 8;
    }
    throw std::out_of_range("VarInt value exceeds 2^62-1");
}

auto VarInt::write(uint8_t* out, uint64_t value) -> std::size_t {
    const std::size_t size = encoded_size(value);
    switch (size) {
        case 1:
            out[0] = static_cast<uint8_t>(value);
            return 1;
        case 2:
            out[0] = static_cast<uint8_t>(kPrefix2 | ((value >> 8) & 0x3F));
            out[1] = static_cast<uint8_t>(value & 0xFF);
            return 2;
        case 4:
            out[0] = static_cast<uint8_t>(kPrefix4 | ((value >> 24) & 0x3F));
            out[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
            out[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
            out[3] = static_cast<uint8_t>(value & 0xFF);
            return 4;
        case 8:
        default:
            out[0] = static_cast<uint8_t>(kPrefix8 | ((value >> 56) & 0x3F));
            out[1] = static_cast<uint8_t>((value >> 48) & 0xFF);
            out[2] = static_cast<uint8_t>((value >> 40) & 0xFF);
            out[3] = static_cast<uint8_t>((value >> 32) & 0xFF);
            out[4] = static_cast<uint8_t>((value >> 24) & 0xFF);
            out[5] = static_cast<uint8_t>((value >> 16) & 0xFF);
            out[6] = static_cast<uint8_t>((value >> 8) & 0xFF);
            out[7] = static_cast<uint8_t>(value & 0xFF);
            return 8;
    }
}

auto VarInt::encode(uint64_t value) -> std::vector<uint8_t> {
    std::vector<uint8_t> out(encoded_size(value));
    write(out.data(), value);
    return out;
}

auto VarInt::append(std::vector<uint8_t>& out, uint64_t value) -> void {
    const std::size_t size = encoded_size(value);
    const std::size_t base = out.size();
    out.resize(base + size);
    write(out.data() + base, value);
}

auto VarInt::decode(const uint8_t* buf, std::size_t buf_len, std::size_t& offset, uint64_t& value)
    -> bool {
    if (offset >= buf_len) {
        return false;
    }
    const uint8_t first = buf[offset];
    const std::size_t size = 1U << (first >> 6);
    if (offset + size > buf_len) {
        return false;
    }
    uint64_t result = first & 0x3F;
    for (std::size_t i = 1; i < size; ++i) {
        result = (result << 8) | buf[offset + i];
    }
    offset += size;
    value = result;
    return true;
}

auto VarInt::decode(const std::vector<uint8_t>& buf, std::size_t& offset, uint64_t& value) -> bool {
    return decode(buf.data(), buf.size(), offset, value);
}

} // namespace quic
} // namespace spaznet
