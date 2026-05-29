#include <gtest/gtest.h>

#include <libspaznet/http3/h3frame.hpp>

#include <vector>

using namespace spaznet::http3;

namespace {

auto round_trip(const H3Frame& f) -> testing::AssertionResult {
    std::vector<uint8_t> wire;
    encode_h3_frame(wire, f);
    std::size_t off = 0;
    H3Frame parsed;
    if (!parse_h3_frame({wire.data(), wire.size()}, off, parsed)) {
        return testing::AssertionFailure() << "parse failed";
    }
    if (off != wire.size()) {
        return testing::AssertionFailure()
               << "leftover bytes: off=" << off << " wire=" << wire.size();
    }
    std::vector<uint8_t> re;
    encode_h3_frame(re, parsed);
    if (re != wire) {
        return testing::AssertionFailure() << "re-encode differs";
    }
    return testing::AssertionSuccess();
}

} // namespace

TEST(H3Frame, Data) {
    H3Data f;
    f.data = {0x10, 0x20, 0x30, 0x40};
    EXPECT_TRUE(round_trip(f));
}

TEST(H3Frame, Headers) {
    H3Headers f;
    f.encoded_field_section = {0x00, 0x00, 0xC0 | 17, 0xC0 | 25};
    EXPECT_TRUE(round_trip(f));
}

TEST(H3Frame, Settings) {
    H3Settings f;
    f.entries.emplace_back(0x06, 1024); // SETTINGS_MAX_FIELD_SECTION_SIZE
    f.entries.emplace_back(0x01, 0);    // QPACK_MAX_TABLE_CAPACITY
    f.entries.emplace_back(0x07, 0);    // QPACK_BLOCKED_STREAMS
    EXPECT_TRUE(round_trip(f));
}

TEST(H3Frame, GoAwayAndMaxPushAndCancelPush) {
    EXPECT_TRUE(round_trip(H3GoAway{4}));
    EXPECT_TRUE(round_trip(H3MaxPushId{100}));
    EXPECT_TRUE(round_trip(H3CancelPush{2}));
}

TEST(H3Frame, ReservedRoundTrip) {
    // Frame type 0x21 (one of the GREASE patterns) carries arbitrary bytes.
    H3Reserved f;
    f.type = 0x21;
    f.data = {0xAB, 0xCD};
    EXPECT_TRUE(round_trip(f));
}

TEST(H3Frame, MultiParse) {
    std::vector<uint8_t> wire;
    encode_h3_frame(wire, H3Settings{});
    encode_h3_frame(wire, H3Headers{{0x00, 0x00, 0xC0 | 17}});
    encode_h3_frame(wire, H3Data{{1, 2, 3}});

    std::size_t off = 0;
    H3Frame f;
    ASSERT_TRUE(parse_h3_frame({wire.data(), wire.size()}, off, f));
    EXPECT_TRUE(std::holds_alternative<H3Settings>(f));
    ASSERT_TRUE(parse_h3_frame({wire.data(), wire.size()}, off, f));
    EXPECT_TRUE(std::holds_alternative<H3Headers>(f));
    ASSERT_TRUE(parse_h3_frame({wire.data(), wire.size()}, off, f));
    EXPECT_TRUE(std::holds_alternative<H3Data>(f));
    EXPECT_EQ(off, wire.size());
}
