// RFC 7541 Appendix C.4 Huffman example vectors.

#include <gtest/gtest.h>

#include <libspaznet/http3/huffman.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

using namespace spaznet::http3;

namespace {

auto hex(std::string s) -> std::vector<uint8_t> {
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](unsigned char c) { return std::isspace(c) != 0; }),
            s.end());
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    auto nyb = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return 10 + (c - 'A');
    };
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((nyb(s[i]) << 4) | nyb(s[i + 1])));
    }
    return out;
}

} // namespace

TEST(HpackHuffman, Encode_WwwExampleCom) {
    // RFC 7541 §C.4.1: "www.example.com" -> f1 e3 c2 e5 f2 3a 6b a0 ab 90 f4 ff
    std::vector<uint8_t> out;
    huffman_encode("www.example.com", out);
    EXPECT_EQ(out, hex("f1e3 c2e5 f23a 6ba0 ab90 f4ff"));
}

TEST(HpackHuffman, Encode_NoCache) {
    std::vector<uint8_t> out;
    huffman_encode("no-cache", out);
    EXPECT_EQ(out, hex("a8eb 1064 9cbf"));
}

TEST(HpackHuffman, Encode_CustomKey) {
    std::vector<uint8_t> out;
    huffman_encode("custom-key", out);
    EXPECT_EQ(out, hex("25a8 49e9 5ba9 7d7f"));
}

TEST(HpackHuffman, Encode_CustomValue) {
    std::vector<uint8_t> out;
    huffman_encode("custom-value", out);
    EXPECT_EQ(out, hex("25a8 49e9 5bb8 e8b4 bf"));
}

TEST(HpackHuffman, Decode_RoundTrip) {
    for (std::string input :
         {std::string("hello"),
          std::string("www.example.com"),
          std::string("no-cache"),
          std::string("custom-key"),
          std::string("custom-value"),
          std::string(""),
          std::string("\x00\x01\x02\x03\xfe\xff", 6),
          std::string(255, 'a')}) {
        std::vector<uint8_t> enc;
        huffman_encode(input, enc);
        std::string dec;
        ASSERT_TRUE(huffman_decode({enc.data(), enc.size()}, dec)) << "input=" << input;
        EXPECT_EQ(dec, input);
    }
}

TEST(HpackHuffman, EncodedSizeMatchesActual) {
    for (std::string input :
         {std::string("hi"), std::string("www.example.com"), std::string(255, 'z')}) {
        std::vector<uint8_t> enc;
        huffman_encode(input, enc);
        EXPECT_EQ(huffman_encoded_size(input), enc.size());
    }
}
