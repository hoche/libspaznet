#include <gtest/gtest.h>

#include <libspaznet/http3/qpack.hpp>

#include <string>
#include <vector>

using namespace spaznet::http3;

TEST(QpackStatic, TableHasExpectedSize) {
    EXPECT_EQ(qpack_static_table().size(), 99U);
    EXPECT_STREQ(qpack_static_table()[0].name, ":authority");
    EXPECT_STREQ(qpack_static_table()[17].name, ":method");
    EXPECT_STREQ(qpack_static_table()[17].value, "GET");
    EXPECT_STREQ(qpack_static_table()[25].name, ":status");
    EXPECT_STREQ(qpack_static_table()[25].value, "200");
}

TEST(QpackEncode, IndexedFieldLine_StaticOnly) {
    HeaderList in = {{":method", "GET"}, {":scheme", "https"}, {":path", "/"},
                     {":status", "200"}};
    std::vector<uint8_t> wire;
    qpack_encode(in, wire);
    // Two-byte prefix (0x00 0x00) plus four 1100xxxx (0xC0 | idx) bytes.
    ASSERT_EQ(wire.size(), 2U + 4U);
    EXPECT_EQ(wire[0], 0x00);
    EXPECT_EQ(wire[1], 0x00);
    EXPECT_EQ(wire[2], 0xC0 | 17); // :method GET
    EXPECT_EQ(wire[3], 0xC0 | 23); // :scheme https
    EXPECT_EQ(wire[4], 0xC0 | 1);  // :path /
    EXPECT_EQ(wire[5], 0xC0 | 25); // :status 200
}

TEST(QpackRoundTrip, MixedFields) {
    HeaderList in = {{":method", "GET"},
                     {":scheme", "https"},
                     {":authority", "localhost"},
                     {":path", "/index.html"},
                     {"user-agent", "curl/8.5.0"},
                     {"accept", "*/*"},
                     {"x-custom-header", "frobnicate"}};
    std::vector<uint8_t> wire;
    qpack_encode(in, wire);
    HeaderList out;
    ASSERT_TRUE(qpack_decode({wire.data(), wire.size()}, out));
    EXPECT_EQ(out, in);
}

TEST(QpackRoundTrip, EmptyValueAllowed) {
    HeaderList in = {{":path", ""}};
    std::vector<uint8_t> wire;
    qpack_encode(in, wire);
    HeaderList out;
    ASSERT_TRUE(qpack_decode({wire.data(), wire.size()}, out));
    EXPECT_EQ(out, in);
}

TEST(QpackRoundTrip, ResponseHeaders) {
    HeaderList in = {{":status", "200"},
                     {"content-type", "text/plain"},
                     {"content-length", "5"},
                     {"server", "libspaznet"}};
    std::vector<uint8_t> wire;
    qpack_encode(in, wire);
    HeaderList out;
    ASSERT_TRUE(qpack_decode({wire.data(), wire.size()}, out));
    EXPECT_EQ(out, in);
}

TEST(QpackDecode, RejectsNonZeroInsertCount) {
    // 0x01 (insert count = 1) + 0x00 (delta base = 0). Must fail since
    // we don't support dynamic table.
    std::vector<uint8_t> wire = {0x01, 0x00};
    HeaderList out;
    EXPECT_FALSE(qpack_decode({wire.data(), wire.size()}, out));
}
