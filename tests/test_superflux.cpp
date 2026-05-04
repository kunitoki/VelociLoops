#include "velociloops.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <string>
#include <vector>

namespace
{

namespace fs = std::filesystem;

const fs::path kDataDir = VELOCILOOPS_TEST_DATA_DIR;

struct ScopedVLFile
{
    VLFile handle = nullptr;

    explicit ScopedVLFile(VLFile h = nullptr) : handle(h) {}
    ~ScopedVLFile() { vl_close(handle); }

    ScopedVLFile(const ScopedVLFile&) = delete;
    ScopedVLFile& operator=(const ScopedVLFile&) = delete;

    explicit operator bool() const { return handle != nullptr; }
};

uint16_t le16(const uint8_t* p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

uint32_t le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct WavAudio
{
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint32_t frameCount = 0;
    std::vector<float> left;
    std::vector<float> right; // empty for mono
    bool valid = false;
};

WavAudio loadWavFloat(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};

    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};

    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return {};

    uint16_t audioFormat = 0, channels = 0, bitDepth = 0;
    uint32_t sampleRate = 0;
    size_t dataOffset = 0;
    uint32_t dataSize = 0;

    size_t off = 12;
    while (off + 8 <= bytes.size())
    {
        const uint8_t* chunk = bytes.data() + off;
        const uint32_t size = le32(chunk + 4);
        off += 8;
        if (off + size > bytes.size())
            return {};

        if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16)
        {
            audioFormat = le16(bytes.data() + off);
            channels = le16(bytes.data() + off + 2);
            sampleRate = le32(bytes.data() + off + 4);
            bitDepth = le16(bytes.data() + off + 14);
        }
        else if (std::memcmp(chunk, "data", 4) == 0)
        {
            dataOffset = off;
            dataSize = size;
        }

        off += size + (size & 1u);
    }

    if (audioFormat != 1 || channels < 1 || channels > 2 || dataSize == 0)
        return {};
    if (bitDepth != 16 && bitDepth != 24)
        return {};

    const uint32_t bytesPerFrame = channels * (bitDepth / 8);
    const uint32_t frameCount = dataSize / bytesPerFrame;

    WavAudio out;
    out.channels = channels;
    out.sampleRate = sampleRate;
    out.frameCount = frameCount;
    out.left.resize(frameCount);
    if (channels == 2)
        out.right.resize(frameCount);

    if (bitDepth == 16)
    {
        for (uint32_t i = 0; i < frameCount; ++i)
        {
            const size_t base = dataOffset + (size_t)i * bytesPerFrame;
            out.left[i] = (float)(int16_t)le16(bytes.data() + base) * (1.0f / 32768.0f);
            if (channels == 2)
                out.right[i] = (float)(int16_t)le16(bytes.data() + base + 2) * (1.0f / 32768.0f);
        }
    }
    else
    {
        for (uint32_t i = 0; i < frameCount; ++i)
        {
            const size_t base = dataOffset + (size_t)i * bytesPerFrame;
            const uint8_t* p = bytes.data() + base;
            const int32_t s = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)(int8_t)p[2] << 16);
            out.left[i] = (float)s * (1.0f / 8388608.0f);
            if (channels == 2)
            {
                const uint8_t* q = p + 3;
                const int32_t s2 = (int32_t)q[0] | ((int32_t)q[1] << 8) | ((int32_t)(int8_t)q[2] << 16);
                out.right[i] = (float)s2 * (1.0f / 8388608.0f);
            }
        }
    }

    out.valid = true;
    return out;
}

std::vector<fs::path> wavFixtures()
{
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(kDataDir))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".wav")
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

TEST_CASE("superflux fixture suite")
{
    const auto fixtures = wavFixtures();
    CHECK(!fixtures.empty());

    for (const auto& path : fixtures)
    {
        CAPTURE(path.filename().string());

        const WavAudio wav = loadWavFloat(path);
        REQUIRE(wav.valid);
        REQUIRE(wav.frameCount > 0);

        VLError err = VL_OK;
        ScopedVLFile file(vl_create_from_superflux(
            (int32_t)wav.channels,
            (int32_t)wav.sampleRate,
            120000,
            wav.left.data(),
            wav.right.empty() ? nullptr : wav.right.data(),
            (int32_t)wav.frameCount,
            nullptr,
            &err));

        REQUIRE(file);
        CHECK(err == VL_OK);

        VLFileInfo info = {};
        REQUIRE(vl_get_info(file.handle, &info) == VL_OK);
        CHECK(info.channels == (int32_t)wav.channels);
        CHECK(info.sample_rate == (int32_t)wav.sampleRate);
        CHECK(info.total_frames == (int32_t)wav.frameCount);
        CHECK(info.slice_count >= 1);
        CHECK(info.loop_start == 0);
        CHECK(info.loop_end == (int32_t)wav.frameCount);

        int32_t nextExpectedStart = info.loop_start;
        int32_t prevPpq = -1;
        for (int32_t i = 0; i < info.slice_count; ++i)
        {
            CAPTURE(i);
            VLSliceInfo slice = {};
            REQUIRE(vl_get_slice_info(file.handle, i, &slice) == VL_OK);

            CHECK(slice.sample_start == nextExpectedStart);
            CHECK(slice.sample_length > 0);
            CHECK(slice.ppq_pos >= prevPpq);

            nextExpectedStart = slice.sample_start + slice.sample_length;
            prevPpq = slice.ppq_pos;

            const int32_t frames = vl_get_slice_frame_count(file.handle, i);
            REQUIRE(frames > 0);

            std::vector<float> left((size_t)frames);
            std::vector<float> right(info.channels == 2 ? (size_t)frames : 0u);
            int32_t written = 0;
            REQUIRE(vl_decode_slice(file.handle, i,
                                    left.data(),
                                    right.empty() ? nullptr : right.data(),
                                    0, frames, &written) == VL_OK);
            CHECK(written == frames);
            CHECK(std::all_of(left.begin(), left.end(), [](float s) { return std::isfinite(s); }));
            CHECK(std::all_of(right.begin(), right.end(), [](float s) { return std::isfinite(s); }));
        }

        CHECK(nextExpectedStart == info.loop_end);
    }
}

TEST_CASE("superflux output can be saved and reopened with consistent metadata")
{
    const WavAudio wav = loadWavFloat(kDataDir / "120Stereo.wav");
    REQUIRE(wav.valid);
    CHECK(wav.channels == 2);

    VLError err = VL_OK;
    ScopedVLFile file(vl_create_from_superflux(
        2, (int32_t)wav.sampleRate, 120000,
        wav.left.data(), wav.right.data(), (int32_t)wav.frameCount,
        nullptr, &err));
    REQUIRE(file);

    VLFileInfo info = {};
    REQUIRE(vl_get_info(file.handle, &info) == VL_OK);

    size_t size = 0;
    REQUIRE(vl_save_to_memory(file.handle, nullptr, &size) == VL_OK);
    REQUIRE(size > 64);

    std::vector<uint8_t> bytes(size);
    REQUIRE(vl_save_to_memory(file.handle, bytes.data(), &size) == VL_OK);

    ScopedVLFile reopened(vl_open_from_memory(bytes.data(), bytes.size(), &err));
    REQUIRE(reopened);

    VLFileInfo reopenedInfo = {};
    REQUIRE(vl_get_info(reopened.handle, &reopenedInfo) == VL_OK);
    CHECK(reopenedInfo.channels == info.channels);
    CHECK(reopenedInfo.sample_rate == info.sample_rate);
    CHECK(reopenedInfo.slice_count == info.slice_count);
    CHECK(reopenedInfo.total_frames == info.total_frames);
    CHECK(reopenedInfo.loop_start == info.loop_start);
    CHECK(reopenedInfo.loop_end == info.loop_end);

    for (int32_t i = 0; i < reopenedInfo.slice_count; ++i)
    {
        CAPTURE(i);
        const int32_t frames = vl_get_slice_frame_count(reopened.handle, i);
        REQUIRE(frames > 0);

        std::vector<float> left((size_t)frames);
        std::vector<float> right((size_t)frames);
        int32_t written = 0;
        REQUIRE(vl_decode_slice(reopened.handle, i, left.data(), right.data(), 0, frames, &written) == VL_OK);
        CHECK(written == frames);
        CHECK(std::all_of(left.begin(), left.end(), [](float s) { return std::isfinite(s); }));
        CHECK(std::all_of(right.begin(), right.end(), [](float s) { return std::isfinite(s); }));
    }
}

TEST_CASE("superflux lower threshold detects more onsets")
{
    const WavAudio wav = loadWavFloat(kDataDir / "120Mono.wav");
    REQUIRE(wav.valid);

    auto countSlices = [&](float threshold) -> int32_t
    {
        VLSuperFluxOptions opts = {};
        vl_superflux_default_options(&opts);
        opts.threshold = threshold;

        VLError err = VL_OK;
        ScopedVLFile file(vl_create_from_superflux(
            1, (int32_t)wav.sampleRate, 120000,
            wav.left.data(), nullptr, (int32_t)wav.frameCount,
            &opts, &err));
        if (!file)
            return 0;
        VLFileInfo info = {};
        vl_get_info(file.handle, &info);
        return info.slice_count;
    };

    const int32_t countLowThreshold = countSlices(0.5f);
    const int32_t countHighThreshold = countSlices(5.0f);

    CHECK(countLowThreshold >= countHighThreshold);
    CHECK(countLowThreshold >= 1);
    CHECK(countHighThreshold >= 1);
}

TEST_CASE("superflux very high threshold produces 1 slice")
{
    const WavAudio wav = loadWavFloat(kDataDir / "120FourBeats.wav");
    REQUIRE(wav.valid);

    VLSuperFluxOptions opts = {};
    vl_superflux_default_options(&opts);
    opts.threshold = 1000.0f;

    VLError err = VL_OK;
    ScopedVLFile file(vl_create_from_superflux(
        (int32_t)wav.channels,
        (int32_t)wav.sampleRate,
        120000,
        wav.left.data(),
        wav.right.empty() ? nullptr : wav.right.data(),
        (int32_t)wav.frameCount,
        &opts, &err));
    REQUIRE(file);

    VLFileInfo info = {};
    REQUIRE(vl_get_info(file.handle, &info) == VL_OK);
    CHECK(info.slice_count == 1);
    CHECK(info.total_frames == (int32_t)wav.frameCount);
}

TEST_CASE("superflux larger combine_ms produces fewer or equal slices")
{
    const WavAudio wav = loadWavFloat(kDataDir / "240FiveHundredSlices.wav");
    REQUIRE(wav.valid);

    auto countSlices = [&](float combineMs) -> int32_t
    {
        VLSuperFluxOptions opts = {};
        vl_superflux_default_options(&opts);
        opts.combine_ms = combineMs;

        VLError err = VL_OK;
        ScopedVLFile file(vl_create_from_superflux(
            (int32_t)wav.channels,
            (int32_t)wav.sampleRate,
            240000,
            wav.left.data(),
            wav.right.empty() ? nullptr : wav.right.data(),
            (int32_t)wav.frameCount,
            &opts, &err));
        if (!file)
            return 0;
        VLFileInfo info = {};
        vl_get_info(file.handle, &info);
        return info.slice_count;
    };

    const int32_t countTight = countSlices(10.0f);
    const int32_t countLoose = countSlices(200.0f);

    CHECK(countTight >= countLoose);
    CHECK(countLoose >= 1);
}

TEST_CASE("superflux min_slice_frames enforces minimum distance between slice starts")
{
    const WavAudio wav = loadWavFloat(kDataDir / "120FourBeats.wav");
    REQUIRE(wav.valid);

    const int32_t minFrames = (int32_t)wav.sampleRate / 10; // 100 ms

    VLSuperFluxOptions opts = {};
    vl_superflux_default_options(&opts);
    opts.threshold = 0.5f;
    opts.min_slice_frames = minFrames;

    VLError err = VL_OK;
    ScopedVLFile file(vl_create_from_superflux(
        (int32_t)wav.channels,
        (int32_t)wav.sampleRate,
        120000,
        wav.left.data(),
        wav.right.empty() ? nullptr : wav.right.data(),
        (int32_t)wav.frameCount,
        &opts, &err));
    REQUIRE(file);

    VLFileInfo info = {};
    REQUIRE(vl_get_info(file.handle, &info) == VL_OK);

    for (int32_t i = 0; i + 1 < info.slice_count; ++i)
    {
        CAPTURE(i);
        VLSliceInfo curr = {};
        VLSliceInfo next = {};
        REQUIRE(vl_get_slice_info(file.handle, i, &curr) == VL_OK);
        REQUIRE(vl_get_slice_info(file.handle, i + 1, &next) == VL_OK);
        CHECK(next.sample_start - curr.sample_start >= minFrames);
    }
}

TEST_CASE("superflux handles 24-bit mono WAV correctly")
{
    const WavAudio wav = loadWavFloat(kDataDir / "120Mono24Bits.wav");
    REQUIRE(wav.valid);
    CHECK(wav.channels == 1);

    VLError err = VL_OK;
    ScopedVLFile file(vl_create_from_superflux(
        1, (int32_t)wav.sampleRate, 120000,
        wav.left.data(), nullptr, (int32_t)wav.frameCount,
        nullptr, &err));

    REQUIRE(file);
    CHECK(err == VL_OK);

    VLFileInfo info = {};
    REQUIRE(vl_get_info(file.handle, &info) == VL_OK);
    CHECK(info.channels == 1);
    CHECK(info.sample_rate == (int32_t)wav.sampleRate);
    CHECK(info.total_frames == (int32_t)wav.frameCount);
    CHECK(info.slice_count >= 1);
    CHECK(info.loop_end == (int32_t)wav.frameCount);
}

TEST_CASE("superflux stereo WAV produces stereo output with decodable channels")
{
    const WavAudio wav = loadWavFloat(kDataDir / "120Stereo.wav");
    REQUIRE(wav.valid);
    CHECK(wav.channels == 2);

    VLError err = VL_OK;
    ScopedVLFile file(vl_create_from_superflux(
        2, (int32_t)wav.sampleRate, 120000,
        wav.left.data(), wav.right.data(), (int32_t)wav.frameCount,
        nullptr, &err));
    REQUIRE(file);

    VLFileInfo info = {};
    REQUIRE(vl_get_info(file.handle, &info) == VL_OK);
    CHECK(info.channels == 2);
    CHECK(info.slice_count >= 1);

    const int32_t frames = vl_get_slice_frame_count(file.handle, 0);
    REQUIRE(frames > 0);

    std::vector<float> left((size_t)frames);
    std::vector<float> right((size_t)frames);
    int32_t written = 0;
    REQUIRE(vl_decode_slice(file.handle, 0, left.data(), right.data(), 0, frames, &written) == VL_OK);
    CHECK(written == frames);
    CHECK(std::all_of(left.begin(), left.end(), [](float s) { return std::isfinite(s); }));
    CHECK(std::all_of(right.begin(), right.end(), [](float s) { return std::isfinite(s); }));

    const double leftRightDiff = std::inner_product(
        left.begin(), left.end(), right.begin(), 0.0,
        std::plus<double>(), [](float l, float r) { return std::fabs((double)l - (double)r); });
    CHECK(leftRightDiff > 0.0);
}

