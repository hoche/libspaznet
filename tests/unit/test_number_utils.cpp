#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include <libspaznet/utils/number_utils.hpp>

using spaznet::NumberUtils;

// ---- parse_uint64 ---------------------------------------------------------

TEST(NumberUtilsParseUint64, AcceptsPlainDigits) {
    EXPECT_EQ(NumberUtils::parse_uint64("0"), std::optional<uint64_t>(0));
    EXPECT_EQ(NumberUtils::parse_uint64("5"), std::optional<uint64_t>(5));
    EXPECT_EQ(NumberUtils::parse_uint64("1234567890"),
              std::optional<uint64_t>(1234567890ULL));
    EXPECT_EQ(NumberUtils::parse_uint64("18446744073709551615"),
              std::optional<uint64_t>(std::numeric_limits<uint64_t>::max()));
}

TEST(NumberUtilsParseUint64, RejectsLenientForms) {
    // These are exactly the std::stoull-isms that enable Content-Length
    // confusion / request smuggling.
    EXPECT_FALSE(NumberUtils::parse_uint64("").has_value());       // empty
    EXPECT_FALSE(NumberUtils::parse_uint64(" 5").has_value());     // leading space
    EXPECT_FALSE(NumberUtils::parse_uint64("5 ").has_value());     // trailing space
    EXPECT_FALSE(NumberUtils::parse_uint64("+5").has_value());     // leading plus
    EXPECT_FALSE(NumberUtils::parse_uint64("-1").has_value());     // negative (would wrap)
    EXPECT_FALSE(NumberUtils::parse_uint64("0x10").has_value());   // hex prefix
    EXPECT_FALSE(NumberUtils::parse_uint64("5abc").has_value());   // trailing garbage
    EXPECT_FALSE(NumberUtils::parse_uint64("1_000").has_value());  // separator
    EXPECT_FALSE(NumberUtils::parse_uint64("\t7").has_value());    // leading tab
}

TEST(NumberUtilsParseUint64, RejectsOverflow) {
    // One past UINT64_MAX.
    EXPECT_FALSE(NumberUtils::parse_uint64("18446744073709551616").has_value());
    EXPECT_FALSE(NumberUtils::parse_uint64("99999999999999999999999").has_value());
}

// ---- parse_int ------------------------------------------------------------

TEST(NumberUtilsParseInt, AcceptsSignedDigits) {
    EXPECT_EQ(NumberUtils::parse_int("0"), std::optional<int>(0));
    EXPECT_EQ(NumberUtils::parse_int("200"), std::optional<int>(200));
    EXPECT_EQ(NumberUtils::parse_int("+42"), std::optional<int>(42));
    EXPECT_EQ(NumberUtils::parse_int("-42"), std::optional<int>(-42));
    EXPECT_EQ(NumberUtils::parse_int("2147483647"),
              std::optional<int>(std::numeric_limits<int>::max()));
    EXPECT_EQ(NumberUtils::parse_int("-2147483648"),
              std::optional<int>(std::numeric_limits<int>::min()));
}

TEST(NumberUtilsParseInt, RejectsLenientAndOutOfRange) {
    EXPECT_FALSE(NumberUtils::parse_int("").has_value());
    EXPECT_FALSE(NumberUtils::parse_int("+").has_value());       // sign only
    EXPECT_FALSE(NumberUtils::parse_int("-").has_value());
    EXPECT_FALSE(NumberUtils::parse_int(" 200").has_value());    // leading space
    EXPECT_FALSE(NumberUtils::parse_int("200 ").has_value());    // trailing space
    EXPECT_FALSE(NumberUtils::parse_int("200abc").has_value());  // trailing garbage
    EXPECT_FALSE(NumberUtils::parse_int("2147483648").has_value());   // INT_MAX + 1
    EXPECT_FALSE(NumberUtils::parse_int("-2147483649").has_value());  // INT_MIN - 1
}
