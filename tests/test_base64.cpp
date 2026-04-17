#include <gtest/gtest.h>
#include "../base64.h"
#include <string>
#include <vector>

// ── base64 encode / decode round-trips ──────────────────────────────────────

TEST(Base64, EmptyRoundTrip) {
    std::string src = "";
    EXPECT_EQ(base64_decode(base64_encode(src)), src);
}

TEST(Base64, ShortStringRoundTrip) {
    std::string src = "Hello, World!";
    EXPECT_EQ(base64_decode(base64_encode(src)), src);
}

TEST(Base64, BinaryRoundTrip) {
    // 256-byte pattern covering all byte values
    std::string src;
    for (int i = 0; i < 256; ++i) src += static_cast<char>(i);
    EXPECT_EQ(base64_decode(base64_encode(src)), src);
}

TEST(Base64, KnownVector) {
    // RFC 4648 test vectors
    EXPECT_EQ(base64_encode(""),       "");
    EXPECT_EQ(base64_encode("f"),      "Zg==");
    EXPECT_EQ(base64_encode("fo"),     "Zm8=");
    EXPECT_EQ(base64_encode("foo"),    "Zm9v");
    EXPECT_EQ(base64_encode("foobar"), "Zm9vYmFy");
}

TEST(Base64, DecodeKnownVector) {
    EXPECT_EQ(base64_decode(""),         "");
    EXPECT_EQ(base64_decode("Zg=="),     "f");
    EXPECT_EQ(base64_decode("Zm8="),     "fo");
    EXPECT_EQ(base64_decode("Zm9v"),     "foo");
    EXPECT_EQ(base64_decode("Zm9vYmFy"),"foobar");
}

TEST(Base64, LargeBinaryRoundTrip) {
    // 64 KB of pseudo-random bytes
    std::string src;
    src.reserve(65536);
    uint32_t state = 0xDEADBEEFu;
    for (int i = 0; i < 65536; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 5;
        src += static_cast<char>(state & 0xFF);
    }
    EXPECT_EQ(base64_decode(base64_encode(src)), src);
}
