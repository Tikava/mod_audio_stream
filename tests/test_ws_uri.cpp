#include <gtest/gtest.h>
#include <cstring>
#include <cctype>
#include <string>

// Standalone copy of validate_ws_uri for unit testing without FreeSWITCH
#define MAX_WS_URI 4096

static int validate_ws_uri_test(const char* url, char* wsUri) {
    const char* hostStart = nullptr;

    if (strncmp(url, "ws://", 5) == 0) {
        hostStart = url + 5;
    } else if (strncmp(url, "wss://", 6) == 0) {
        hostStart = url + 6;
    } else {
        return 0;
    }

    const char* hostEnd = hostStart;
    while (*hostEnd && *hostEnd != ':' && *hostEnd != '/') {
        if (!std::isalnum(static_cast<unsigned char>(*hostEnd)) &&
            *hostEnd != '-' && *hostEnd != '.') {
            return 0;
        }
        ++hostEnd;
    }
    if (hostStart == hostEnd) return 0;

    if (*hostEnd == ':') {
        const char* portStart = hostEnd + 1;
        while (*portStart && *portStart != '/') {
            if (!std::isdigit(static_cast<unsigned char>(*portStart))) return 0;
            ++portStart;
        }
    }

    strncpy(wsUri, url, MAX_WS_URI);
    wsUri[MAX_WS_URI - 1] = '\0';
    return 1;
}

// ── valid URIs ────────────────────────────────────────────────────────────────

TEST(ValidateWsUri, PlainWs) {
    char out[MAX_WS_URI];
    EXPECT_EQ(validate_ws_uri_test("ws://localhost", out), 1);
    EXPECT_STREQ(out, "ws://localhost");
}

TEST(ValidateWsUri, PlainWsWithPort) {
    char out[MAX_WS_URI];
    EXPECT_EQ(validate_ws_uri_test("ws://localhost:8765", out), 1);
}

TEST(ValidateWsUri, PlainWsWithPath) {
    char out[MAX_WS_URI];
    EXPECT_EQ(validate_ws_uri_test("ws://localhost:8765/stream", out), 1);
}

TEST(ValidateWsUri, SecureWss) {
    char out[MAX_WS_URI];
    EXPECT_EQ(validate_ws_uri_test("wss://asr.example.com/v1/stream", out), 1);
}

TEST(ValidateWsUri, WssWithPort) {
    char out[MAX_WS_URI];
    EXPECT_EQ(validate_ws_uri_test("wss://asr.example.com:443/stream", out), 1);
}

TEST(ValidateWsUri, IpAddress) {
    char out[MAX_WS_URI];
    EXPECT_EQ(validate_ws_uri_test("ws://192.168.1.1:9000/audio", out), 1);
}

TEST(ValidateWsUri, SubdomainHyphen) {
    char out[MAX_WS_URI];
    EXPECT_EQ(validate_ws_uri_test("ws://my-server.example.com", out), 1);
}

// ── invalid URIs ─────────────────────────────────────────────────────────────

TEST(ValidateWsUri, EmptyString) {
    char out[MAX_WS_URI] = {};
    EXPECT_EQ(validate_ws_uri_test("", out), 0);
}

TEST(ValidateWsUri, HttpScheme) {
    char out[MAX_WS_URI];
    EXPECT_EQ(validate_ws_uri_test("http://example.com", out), 0);
}

TEST(ValidateWsUri, NoScheme) {
    char out[MAX_WS_URI];
    EXPECT_EQ(validate_ws_uri_test("example.com", out), 0);
}

TEST(ValidateWsUri, EmptyHost) {
    char out[MAX_WS_URI];
    EXPECT_EQ(validate_ws_uri_test("ws://", out), 0);
    EXPECT_EQ(validate_ws_uri_test("wss://", out), 0);
}

TEST(ValidateWsUri, InvalidCharInHost) {
    char out[MAX_WS_URI];
    EXPECT_EQ(validate_ws_uri_test("ws://exa mple.com", out), 0);
    EXPECT_EQ(validate_ws_uri_test("ws://example!.com", out), 0);
}

TEST(ValidateWsUri, NonNumericPort) {
    char out[MAX_WS_URI];
    EXPECT_EQ(validate_ws_uri_test("ws://example.com:abc", out), 0);
}

TEST(ValidateWsUri, OutputCopied) {
    char out[MAX_WS_URI];
    const char* uri = "wss://stream.example.com:8443/v2/audio";
    EXPECT_EQ(validate_ws_uri_test(uri, out), 1);
    EXPECT_STREQ(out, uri);
}

// ── UTF-8 validation (standalone) ────────────────────────────────────────────

static bool is_valid_utf8_test(const char* str) {
    while (*str) {
        if ((*str & 0x80) == 0x00) {
            str++;
        } else if ((*str & 0xE0) == 0xC0) {
            if ((str[1] & 0xC0) != 0x80) return false;
            str += 2;
        } else if ((*str & 0xF0) == 0xE0) {
            if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80) return false;
            str += 3;
        } else if ((*str & 0xF8) == 0xF0) {
            if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80 || (str[3] & 0xC0) != 0x80) return false;
            str += 4;
        } else {
            return false;
        }
    }
    return true;
}

TEST(Utf8Validation, AsciiValid) {
    EXPECT_TRUE(is_valid_utf8_test("Hello, World!"));
    EXPECT_TRUE(is_valid_utf8_test(""));
    EXPECT_TRUE(is_valid_utf8_test("{}"));
}

TEST(Utf8Validation, TwoByteValid) {
    // "ñ" = 0xC3 0xB1
    const char s[] = {'\xC3', '\xB1', '\0'};
    EXPECT_TRUE(is_valid_utf8_test(s));
}

TEST(Utf8Validation, ThreeByteValid) {
    // "€" = 0xE2 0x82 0xAC
    const char s[] = {'\xE2', '\x82', '\xAC', '\0'};
    EXPECT_TRUE(is_valid_utf8_test(s));
}

TEST(Utf8Validation, InvalidSequence) {
    // 0xFF is never valid in UTF-8
    const char s[] = {'\xFF', '\0'};
    EXPECT_FALSE(is_valid_utf8_test(s));
}

TEST(Utf8Validation, TruncatedSequence) {
    // starts 2-byte but only 1 byte
    const char s[] = {'\xC3', '\0'};
    EXPECT_FALSE(is_valid_utf8_test(s));
}

TEST(Utf8Validation, JsonMetadata) {
    EXPECT_TRUE(is_valid_utf8_test(R"({"language":"en-US","session":"abc123"})"));
}
