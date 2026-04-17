#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <numeric>
#include <cmath>

// ── Standalone copies of pure-logic functions under test ─────────────────────
// These are extracted from AudioStreamer so tests have no FreeSWITCH dependency.

static inline int16_t clamp16(int32_t v) {
    if (v > 32767)  return  32767;
    if (v < -32768) return -32768;
    return static_cast<int16_t>(v);
}

static void convertChannels(const std::vector<int16_t>& in, int in_ch,
                             int out_ch, std::vector<int16_t>& out) {
    if (in_ch == out_ch) { out = in; return; }

    const size_t frames = in.size() / in_ch;
    out.resize(frames * out_ch);

    if (in_ch == 2 && out_ch == 1) {
        for (size_t i = 0; i < frames; ++i)
            out[i] = clamp16(((int32_t)in[2*i] + (int32_t)in[2*i+1]) / 2);
    } else if (in_ch == 1 && out_ch == 2) {
        for (size_t i = 0; i < frames; ++i) {
            out[2*i]   = in[i];
            out[2*i+1] = in[i];
        }
    } else {
        for (size_t i = 0; i < frames; ++i)
            for (int c = 0; c < out_ch; ++c)
                out[i*out_ch+c] = in[std::min<size_t>(i*in_ch, in.size()-1)];
    }
}

// Mixer: accumulate multiple PlaybackItems and clamp to int16
static std::vector<int16_t> mixItems(
        const std::vector<std::vector<int16_t>>& items,
        size_t samples_per_frame,
        int channels) {
    std::vector<int32_t> accum(samples_per_frame * channels, 0);
    for (auto& item : items) {
        for (size_t s = 0; s < std::min(item.size(), accum.size()); ++s) {
            accum[s] = std::min<int32_t>(32767,
                       std::max<int32_t>(-32768, accum[s] + item[s]));
        }
    }
    std::vector<int16_t> out(accum.size());
    for (size_t i = 0; i < accum.size(); ++i)
        out[i] = static_cast<int16_t>(accum[i]);
    return out;
}

// ── clamp16 ──────────────────────────────────────────────────────────────────

TEST(Clamp16, InRange) {
    EXPECT_EQ(clamp16(0),      0);
    EXPECT_EQ(clamp16(32767),  32767);
    EXPECT_EQ(clamp16(-32768), -32768);
    EXPECT_EQ(clamp16(1000),   1000);
    EXPECT_EQ(clamp16(-1000), -1000);
}

TEST(Clamp16, Overflow) {
    EXPECT_EQ(clamp16(32768),   32767);
    EXPECT_EQ(clamp16(100000),  32767);
    EXPECT_EQ(clamp16(-32769), -32768);
    EXPECT_EQ(clamp16(-100000),-32768);
}

// ── convertChannels ──────────────────────────────────────────────────────────

TEST(ConvertChannels, MonoToMono) {
    std::vector<int16_t> in = {100, 200, 300};
    std::vector<int16_t> out;
    convertChannels(in, 1, 1, out);
    EXPECT_EQ(out, in);
}

TEST(ConvertChannels, StereoToStereo) {
    std::vector<int16_t> in = {1, 2, 3, 4};
    std::vector<int16_t> out;
    convertChannels(in, 2, 2, out);
    EXPECT_EQ(out, in);
}

TEST(ConvertChannels, StereoToMono_Average) {
    // (100 + 200) / 2 = 150
    std::vector<int16_t> in = {100, 200, 300, 400};
    std::vector<int16_t> out;
    convertChannels(in, 2, 1, out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], 150);
    EXPECT_EQ(out[1], 350);
}

TEST(ConvertChannels, StereoToMono_ClampOnOverflow) {
    std::vector<int16_t> in = {32767, 32767};
    std::vector<int16_t> out;
    convertChannels(in, 2, 1, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], 32767); // (32767+32767)/2 = 32767
}

TEST(ConvertChannels, MonoToStereo_Duplicate) {
    std::vector<int16_t> in = {1000, 2000};
    std::vector<int16_t> out;
    convertChannels(in, 1, 2, out);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0], 1000);
    EXPECT_EQ(out[1], 1000);
    EXPECT_EQ(out[2], 2000);
    EXPECT_EQ(out[3], 2000);
}

TEST(ConvertChannels, EmptyInput) {
    std::vector<int16_t> in;
    std::vector<int16_t> out;
    convertChannels(in, 1, 2, out);
    EXPECT_TRUE(out.empty());
}

// ── mixer ────────────────────────────────────────────────────────────────────

TEST(Mixer, SingleItem) {
    std::vector<int16_t> item = {1000, 2000, 3000};
    auto out = mixItems({item}, 3, 1);
    EXPECT_EQ(out, item);
}

TEST(Mixer, TwoItemsSum) {
    std::vector<int16_t> a = {1000, -1000};
    std::vector<int16_t> b = {500,   500};
    auto out = mixItems({a, b}, 2, 1);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], 1500);
    EXPECT_EQ(out[1], -500);
}

TEST(Mixer, ClampOnSumOverflow) {
    std::vector<int16_t> a = {32767};
    std::vector<int16_t> b = {32767};
    auto out = mixItems({a, b}, 1, 1);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], 32767);
}

TEST(Mixer, ClampNegativeOverflow) {
    std::vector<int16_t> a = {-32768};
    std::vector<int16_t> b = {-32768};
    auto out = mixItems({a, b}, 1, 1);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], -32768);
}

TEST(Mixer, Silence) {
    std::vector<int16_t> a = {0, 0, 0};
    std::vector<int16_t> b = {0, 0, 0};
    auto out = mixItems({a, b}, 3, 1);
    for (auto s : out) EXPECT_EQ(s, 0);
}

TEST(Mixer, EmptyQueue) {
    auto out = mixItems({}, 4, 1);
    EXPECT_EQ(out.size(), 4u);
    for (auto s : out) EXPECT_EQ(s, 0);
}

// ── frame sizing ─────────────────────────────────────────────────────────────

TEST(FrameSizing, TwentyMsAt8k) {
    const int rate = 8000;
    const size_t samples_per_frame = rate / 50; // 20ms
    EXPECT_EQ(samples_per_frame, 160u);
    const size_t frame_bytes = samples_per_frame * sizeof(int16_t);
    EXPECT_EQ(frame_bytes, 320u);
}

TEST(FrameSizing, TwentyMsAt16k) {
    const int rate = 16000;
    const size_t samples_per_frame = rate / 50;
    EXPECT_EQ(samples_per_frame, 320u);
}

TEST(FrameSizing, TwentyMsAt48k) {
    const int rate = 48000;
    const size_t samples_per_frame = rate / 50;
    EXPECT_EQ(samples_per_frame, 960u);
}

// ── audioChannels validation ─────────────────────────────────────────────────

TEST(ChannelValidation, ValidRange) {
    auto validate = [](int ch) {
        if (ch <= 0 || ch > 8) return 1;
        return ch;
    };
    EXPECT_EQ(validate(1), 1);
    EXPECT_EQ(validate(2), 2);
    EXPECT_EQ(validate(8), 8);
}

TEST(ChannelValidation, OutOfRange) {
    auto validate = [](int ch) {
        if (ch <= 0 || ch > 8) return 1;
        return ch;
    };
    EXPECT_EQ(validate(0),  1);
    EXPECT_EQ(validate(-1), 1);
    EXPECT_EQ(validate(9),  1);
    EXPECT_EQ(validate(100),1);
}

// ── backpressure threshold ───────────────────────────────────────────────────

TEST(Backpressure, ThresholdIs512KB) {
    constexpr size_t WS_MAX_BUFFERED_BYTES = 512 * 1024;
    EXPECT_EQ(WS_MAX_BUFFERED_BYTES, 524288u);

    size_t buffered = 524287;
    EXPECT_LT(buffered, WS_MAX_BUFFERED_BYTES);  // should pass

    buffered = 524288;
    EXPECT_GE(buffered, WS_MAX_BUFFERED_BYTES);  // should drop
}
