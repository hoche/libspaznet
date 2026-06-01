#include <libspaznet/codec/huffman.hpp>

#include <array>
#include <cstring>

namespace spaznet::codec {


namespace {

// RFC 7541 Appendix B: per-symbol (code, bit length). Index 0..255 are
// the byte values; 256 is the EOS code (forbidden in real input).
struct HuffSym {
    uint32_t code;
    uint8_t bits;
};

constexpr std::array<HuffSym, 257> kTable = {{
    {0x1ff8u, 13},   {0x7fffd8u, 23}, {0xfffffe2u, 28}, {0xfffffe3u, 28},
    {0xfffffe4u, 28}, {0xfffffe5u, 28}, {0xfffffe6u, 28}, {0xfffffe7u, 28},
    {0xfffffe8u, 28}, {0xffffeau, 24}, {0x3ffffffcu, 30}, {0xfffffe9u, 28},
    {0xfffffeau, 28}, {0x3ffffffdu, 30}, {0xfffffebu, 28}, {0xfffffecu, 28},
    {0xfffffedu, 28}, {0xfffffeeu, 28}, {0xfffffefu, 28}, {0xffffff0u, 28},
    {0xffffff1u, 28}, {0xffffff2u, 28}, {0x3ffffffeu, 30}, {0xffffff3u, 28},
    {0xffffff4u, 28}, {0xffffff5u, 28}, {0xffffff6u, 28}, {0xffffff7u, 28},
    {0xffffff8u, 28}, {0xffffff9u, 28}, {0xffffffau, 28}, {0xffffffbu, 28},
    {0x14u, 6},  {0x3f8u, 10}, {0x3f9u, 10}, {0xffau, 12},
    {0x1ff9u, 13}, {0x15u, 6},  {0xf8u, 8},   {0x7fau, 11},
    {0x3fau, 10}, {0x3fbu, 10}, {0xf9u, 8},   {0x7fbu, 11},
    {0xfau, 8},   {0x16u, 6},   {0x17u, 6},   {0x18u, 6},
    {0x0u, 5},    {0x1u, 5},    {0x2u, 5},    {0x19u, 6},
    {0x1au, 6},   {0x1bu, 6},   {0x1cu, 6},   {0x1du, 6},
    {0x1eu, 6},   {0x1fu, 6},   {0x5cu, 7},   {0xfbu, 8},
    {0x7ffcu, 15}, {0x20u, 6},  {0xffbu, 12}, {0x3fcu, 10},
    {0x1ffau, 13}, {0x21u, 6},  {0x5du, 7},   {0x5eu, 7},
    {0x5fu, 7},    {0x60u, 7},  {0x61u, 7},   {0x62u, 7},
    {0x63u, 7},    {0x64u, 7},  {0x65u, 7},   {0x66u, 7},
    {0x67u, 7},    {0x68u, 7},  {0x69u, 7},   {0x6au, 7},
    {0x6bu, 7},    {0x6cu, 7},  {0x6du, 7},   {0x6eu, 7},
    {0x6fu, 7},    {0x70u, 7},  {0x71u, 7},   {0x72u, 7},
    {0xfcu, 8},    {0x73u, 7},  {0xfdu, 8},   {0x1ffbu, 13},
    {0x7fff0u, 19}, {0x1ffcu, 13}, {0x3ffcu, 14}, {0x22u, 6},
    {0x7ffdu, 15}, {0x3u, 5},   {0x23u, 6},   {0x4u, 5},
    {0x24u, 6},    {0x5u, 5},   {0x25u, 6},   {0x26u, 6},
    {0x27u, 6},    {0x6u, 5},   {0x74u, 7},   {0x75u, 7},
    {0x28u, 6},    {0x29u, 6},  {0x2au, 6},   {0x7u, 5},
    {0x2bu, 6},    {0x76u, 7},  {0x2cu, 6},   {0x8u, 5},
    {0x9u, 5},     {0x2du, 6},  {0x77u, 7},   {0x78u, 7},
    {0x79u, 7},    {0x7au, 7},  {0x7bu, 7},   {0x7ffeu, 15},
    {0x7fcu, 11},  {0x3ffdu, 14}, {0x1ffdu, 13}, {0xffffffcu, 28},
    {0xfffe6u, 20}, {0x3fffd2u, 22}, {0xfffe7u, 20}, {0xfffe8u, 20},
    {0x3fffd3u, 22}, {0x3fffd4u, 22}, {0x3fffd5u, 22}, {0x7fffd9u, 23},
    {0x3fffd6u, 22}, {0x7fffdau, 23}, {0x7fffdbu, 23}, {0x7fffdcu, 23},
    {0x7fffddu, 23}, {0x7fffdeu, 23}, {0xffffebu, 24}, {0x7fffdfu, 23},
    {0xffffecu, 24}, {0xffffedu, 24}, {0x3fffd7u, 22}, {0x7fffe0u, 23},
    {0xffffeeu, 24}, {0x7fffe1u, 23}, {0x7fffe2u, 23}, {0x7fffe3u, 23},
    {0x7fffe4u, 23}, {0x1fffdcu, 21}, {0x3fffd8u, 22}, {0x7fffe5u, 23},
    {0x3fffd9u, 22}, {0x7fffe6u, 23}, {0x7fffe7u, 23}, {0xffffefu, 24},
    {0x3fffdau, 22}, {0x1fffddu, 21}, {0xfffe9u, 20}, {0x3fffdbu, 22},
    {0x3fffdcu, 22}, {0x7fffe8u, 23}, {0x7fffe9u, 23}, {0x1fffdeu, 21},
    {0x7fffeau, 23}, {0x3fffddu, 22}, {0x3fffdeu, 22}, {0xfffff0u, 24},
    {0x1fffdfu, 21}, {0x3fffdfu, 22}, {0x7fffebu, 23}, {0x7fffecu, 23},
    {0x1fffe0u, 21}, {0x1fffe1u, 21}, {0x3fffe0u, 22}, {0x1fffe2u, 21},
    {0x7fffedu, 23}, {0x3fffe1u, 22}, {0x7fffeeu, 23}, {0x7fffefu, 23},
    {0xfffeau, 20}, {0x3fffe2u, 22}, {0x3fffe3u, 22}, {0x3fffe4u, 22},
    {0x7ffff0u, 23}, {0x3fffe5u, 22}, {0x3fffe6u, 22}, {0x7ffff1u, 23},
    {0x3ffffe0u, 26}, {0x3ffffe1u, 26}, {0xfffebu, 20}, {0x7fff1u, 19},
    {0x3fffe7u, 22}, {0x7ffff2u, 23}, {0x3fffe8u, 22}, {0x1ffffecu, 25},
    {0x3ffffe2u, 26}, {0x3ffffe3u, 26}, {0x3ffffe4u, 26}, {0x7ffffdeu, 27},
    {0x7ffffdfu, 27}, {0x3ffffe5u, 26}, {0xfffff1u, 24}, {0x1ffffedu, 25},
    {0x7fff2u, 19}, {0x1fffe3u, 21}, {0x3ffffe6u, 26}, {0x7ffffe0u, 27},
    {0x7ffffe1u, 27}, {0x3ffffe7u, 26}, {0x7ffffe2u, 27}, {0xfffff2u, 24},
    {0x1fffe4u, 21}, {0x1fffe5u, 21}, {0x3ffffe8u, 26}, {0x3ffffe9u, 26},
    {0xffffffdu, 28}, {0x7ffffe3u, 27}, {0x7ffffe4u, 27}, {0x7ffffe5u, 27},
    {0xfffecu, 20}, {0xfffff3u, 24}, {0xfffedu, 20}, {0x1fffe6u, 21},
    {0x3fffe9u, 22}, {0x1fffe7u, 21}, {0x1fffe8u, 21}, {0x7ffff3u, 23},
    {0x3fffeau, 22}, {0x3fffebu, 22}, {0x1ffffeeu, 25}, {0x1ffffefu, 25},
    {0xfffff4u, 24}, {0xfffff5u, 24}, {0x3ffffeau, 26}, {0x7ffff4u, 23},
    {0x3ffffebu, 26}, {0x7ffffe6u, 27}, {0x3ffffecu, 26}, {0x3ffffedu, 26},
    {0x7ffffe7u, 27}, {0x7ffffe8u, 27}, {0x7ffffe9u, 27}, {0x7ffffeau, 27},
    {0x7ffffebu, 27}, {0xffffffeu, 28}, {0x7ffffecu, 27}, {0x7ffffedu, 27},
    {0x7ffffeeu, 27}, {0x7ffffefu, 27}, {0x7fffff0u, 27}, {0x3ffffeeu, 26},
    {0x3fffffffu, 30}, // EOS
}};

} // namespace

auto huffman_encoded_size(std::string_view input) -> std::size_t {
    std::size_t bits = 0;
    for (unsigned char ch : input) {
        bits += kTable[ch].bits;
    }
    return (bits + 7) / 8;
}

auto huffman_encode(std::string_view input, std::vector<uint8_t>& out) -> void {
    uint64_t buf = 0;
    int held = 0;
    const std::size_t base = out.size();
    out.reserve(base + huffman_encoded_size(input));
    for (unsigned char ch : input) {
        const auto& sym = kTable[ch];
        buf = (buf << sym.bits) | sym.code;
        held += sym.bits;
        while (held >= 8) {
            held -= 8;
            out.push_back(static_cast<uint8_t>((buf >> held) & 0xFFU));
        }
    }
    if (held > 0) {
        // EOS-pattern padding: top bits of the EOS code (all 1s).
        const int pad = 8 - held;
        const uint32_t pad_bits = (1U << pad) - 1U;
        out.push_back(static_cast<uint8_t>(((buf << pad) | pad_bits) & 0xFFU));
    }
}

// Decoder uses a precomputed prefix-table indexed by 8-bit chunks. Each
// step consumes 8 input bits and walks one or more table entries until
// a leaf is reached. Build it lazily on first use.
namespace {

struct DecNode {
    int16_t children[2]{-1, -1}; // child node indices in `nodes`
    int16_t symbol{-1};          // leaf symbol (or -1 for internal node)
};

class HuffTree {
  public:
    HuffTree() {
        nodes_.reserve(513);
        nodes_.emplace_back(); // root
        for (std::size_t s = 0; s <= 256; ++s) {
            insert(s, kTable[s].code, kTable[s].bits);
        }
    }
    auto root() const -> int16_t {
        return 0;
    }
    auto step(int16_t node, int bit) const -> int16_t {
        return nodes_[static_cast<std::size_t>(node)].children[bit];
    }
    auto symbol(int16_t node) const -> int16_t {
        return nodes_[static_cast<std::size_t>(node)].symbol;
    }

  private:
    auto insert(std::size_t sym, uint32_t code, std::size_t bits) -> void {
        int16_t cur = 0;
        for (std::size_t i = 0; i < bits; ++i) {
            int b = static_cast<int>((code >> (bits - 1 - i)) & 1U);
            int16_t next = nodes_[static_cast<std::size_t>(cur)].children[b];
            if (next < 0) {
                nodes_.emplace_back();
                next = static_cast<int16_t>(nodes_.size() - 1);
                nodes_[static_cast<std::size_t>(cur)].children[b] = next;
            }
            cur = next;
        }
        nodes_[static_cast<std::size_t>(cur)].symbol = static_cast<int16_t>(sym);
    }
    std::vector<DecNode> nodes_;
};

auto tree() -> const HuffTree& {
    static const HuffTree t;
    return t;
}

} // namespace

auto huffman_decode(std::span<const uint8_t> input, std::string& out) -> bool {
    const HuffTree& t = tree();
    int16_t node = t.root();
    std::size_t bit_count = 0;
    int16_t pad_start_node = t.root();
    std::size_t pad_start_bit = 0;
    bool any_emit_since_start = true;

    for (std::size_t i = 0; i < input.size(); ++i) {
        uint8_t b = input[i];
        for (int j = 7; j >= 0; --j) {
            int bit = (b >> j) & 1;
            int16_t next = t.step(node, bit);
            if (next < 0) {
                return false;
            }
            node = next;
            ++bit_count;
            const int16_t sym = t.symbol(node);
            if (sym >= 0) {
                if (sym == 256) {
                    return false; // EOS in stream is forbidden
                }
                out.push_back(static_cast<char>(static_cast<uint8_t>(sym)));
                node = t.root();
                pad_start_node = node;
                pad_start_bit = bit_count;
                any_emit_since_start = true;
            } else {
                any_emit_since_start = false;
            }
        }
    }
    // Final padding (RFC 7541 §5.2): if we ended mid-symbol, the trailing
    // bits must be a prefix of the EOS code (all 1s) AND no more than 7
    // bits long.
    if (node != t.root()) {
        const std::size_t pad_bits = bit_count - pad_start_bit;
        if (pad_bits > 7) {
            return false;
        }
        // Walk again from pad_start_node along the path we took to here,
        // verifying every bit is 1.
        int16_t check = pad_start_node;
        for (std::size_t i = 0; i < pad_bits; ++i) {
            int16_t next = t.step(check, 1);
            if (next < 0) {
                return false;
            }
            check = next;
        }
        if (check != node) {
            return false;
        }
    }
    (void)any_emit_since_start;
    return true;
}


} // namespace spaznet::codec
