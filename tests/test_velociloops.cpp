#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
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
#include <set>
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
    ~ScopedVLFile()
    {
        vl_close(handle);
    }

    ScopedVLFile(const ScopedVLFile&) = delete;
    ScopedVLFile& operator=(const ScopedVLFile&) = delete;

    ScopedVLFile(ScopedVLFile&& other) noexcept : handle(other.handle)
    {
        other.handle = nullptr;
    }

    ScopedVLFile& operator=(ScopedVLFile&& other) noexcept
    {
        if (this != &other)
        {
            vl_close(handle);
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    explicit operator bool() const
    {
        return handle != nullptr;
    }
};

struct DecodedSlice
{
    VLSliceInfo info = {};
    std::vector<float> left;
    std::vector<float> right;
};

struct DecodedFile
{
    VLFileInfo info = {};
    std::vector<DecodedSlice> slices;
};

struct WavInfo
{
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitDepth = 0;
    uint32_t dataBytes = 0;
};

struct ChunkRange
{
    size_t payload = 0;
    uint32_t size = 0;
    bool found = false;
};

std::vector<fs::path> dataFiles()
{
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(kDataDir))
    {
        if (entry.is_regular_file() && entry.path().extension() != ".png")
        {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<uint8_t> readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

uint16_t le16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void putBe32(std::vector<uint8_t>& bytes, size_t off, uint32_t value)
{
    REQUIRE(off + 4 <= bytes.size());
    bytes[off + 0] = (uint8_t)(value >> 24);
    bytes[off + 1] = (uint8_t)(value >> 16);
    bytes[off + 2] = (uint8_t)(value >> 8);
    bytes[off + 3] = (uint8_t)value;
}

ChunkRange findChunkInRange(const std::vector<uint8_t>& bytes, const char id[4], size_t start, size_t end)
{
    size_t off = start;
    while (off + 8 <= end && off + 8 <= bytes.size())
    {
        const size_t header = off;
        const uint32_t size = be32(bytes.data() + off + 4);
        off += 8;
        if (off + size > bytes.size())
        {
            break;
        }

        if (std::memcmp(bytes.data() + header, id, 4) == 0)
        {
            return {off, size, true};
        }

        if (std::memcmp(bytes.data() + header, "CAT ", 4) == 0 && size >= 4)
        {
            ChunkRange nested = findChunkInRange(bytes, id, off + 4, off + size);
            if (nested.found)
            {
                return nested;
            }
        }

        off += size + (size & 1u);
    }

    return {};
}

ChunkRange findChunk(const std::vector<uint8_t>& bytes, const char id[4])
{
    REQUIRE(bytes.size() >= 12);
    REQUIRE(std::memcmp(bytes.data(), "CAT ", 4) == 0);
    return findChunkInRange(bytes, id, 12, bytes.size());
}

bool readWavInfo(const fs::path& path, WavInfo& out)
{
    const auto bytes = readFile(path);
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
    {
        return false;
    }

    bool sawFmt = false;
    bool sawData = false;
    size_t off = 12;
    while (off + 8 <= bytes.size())
    {
        const uint8_t* chunk = bytes.data() + off;
        const uint32_t size = le32(chunk + 4);
        off += 8;
        if (off + size > bytes.size())
        {
            return false;
        }

        if (std::memcmp(chunk, "fmt ", 4) == 0)
        {
            if (size < 16)
            {
                return false;
            }
            const uint16_t format = le16(bytes.data() + off);
            out.channels = le16(bytes.data() + off + 2);
            out.sampleRate = le32(bytes.data() + off + 4);
            out.bitDepth = le16(bytes.data() + off + 14);
            sawFmt = format == 1;
        }
        else if (std::memcmp(chunk, "data", 4) == 0)
        {
            out.dataBytes = size;
            sawData = true;
        }

        off += size + (size & 1u);
    }

    return sawFmt && sawData;
}

std::set<std::string> expectedOpenableContainers()
{
    return {
        "100HasCreatorInfo.rx2", "100WeirdSampleRate.rx2",    "120AllMuted.rx2",          "120FourBeats.rx2",
        "120Gated.rx2",          "120GatedMuted.rx2",         "120Mono copy.rx2",         "120Mono24Bits.rx2",
        "120Mono.rx2",           "120SevenEights.rx2",        "120Stereo copy.rx2",       "120Stereo.rx2",
        "120ThreeBeats.rx2",     "120TransmitAsOneSlice.rx2", "240FiveHundredSlices.rx2", "450OneHundredBars.rx2",
    };
}

std::set<std::string> expectedRenderableContainers()
{
    auto names = expectedOpenableContainers();
    names.erase("100HasCreatorInfo.rx2");
    names.erase("100WeirdSampleRate.rx2");
    return names;
}

bool isContainerExtension(const fs::path& path)
{
    const std::string ext = path.extension().string();
    return ext == ".rx2" || ext == ".rex" || ext == ".rcy";
}

void checkInfoSane(const VLFileInfo& info)
{
    CHECK((info.channels == 1 || info.channels == 2));
    CHECK(info.sample_rate >= 8000);
    CHECK(info.sample_rate <= 192000);
    CHECK(info.slice_count >= 0);
    CHECK(info.tempo > 0);
    CHECK(info.original_tempo > 0);
    CHECK(info.ppq_length > 0);
    CHECK(info.time_sig_num > 0);
    CHECK(info.time_sig_den > 0);
    CHECK(info.total_frames > 0);
    CHECK(info.loop_start >= 0);
    CHECK(info.loop_end >= 0);
    CHECK(info.loop_end <= info.total_frames);
}

void checkDecodedBuffer(const std::vector<float>& buffer)
{
    CHECK(!buffer.empty());
    CHECK(std::all_of(buffer.begin(), buffer.end(),
                      [](float sample)
                      {
                          return std::isfinite(sample) && sample >= -16.0f && sample <= 16.0f;
                      }));
}

DecodedFile decodeWholeFile(VLFile file)
{
    DecodedFile decoded;
    REQUIRE(vl_get_info(file, &decoded.info) == VL_OK);
    checkInfoSane(decoded.info);
    REQUIRE(decoded.info.slice_count > 0);

    decoded.slices.reserve((size_t)decoded.info.slice_count);
    int32_t previousStart = -1;
    int32_t previousPpq = -1;
    double absoluteEnergy = 0.0;

    for (int32_t index = 0; index < decoded.info.slice_count; ++index)
    {
        DecodedSlice slice;
        CAPTURE(index);
        REQUIRE(vl_get_slice_info(file, index, &slice.info) == VL_OK);
        CHECK(slice.info.sample_start >= 0);
        CHECK(slice.info.sample_length > 1);
        CHECK(slice.info.sample_start >= previousStart);
        CHECK(slice.info.ppq_pos >= previousPpq);
        previousStart = slice.info.sample_start;
        previousPpq = slice.info.ppq_pos;

        const int32_t frames = vl_get_slice_frame_count(file, index);
        REQUIRE(frames > 0);

        slice.left.assign((size_t)frames, -99.0f);
        if (decoded.info.channels == 2)
        {
            slice.right.assign((size_t)frames, -99.0f);
        }

        int32_t written = 0;
        REQUIRE(vl_decode_slice(file, index, slice.left.data(), slice.right.empty() ? nullptr : slice.right.data(), frames, &written) == VL_OK);
        CHECK(written == frames);
        checkDecodedBuffer(slice.left);
        if (!slice.right.empty())
        {
            checkDecodedBuffer(slice.right);
        }

        absoluteEnergy += std::accumulate(slice.left.begin(), slice.left.end(), 0.0,
                                          [](double total, float sample)
                                          {
                                              return total + std::fabs(sample);
                                          });

        decoded.slices.push_back(std::move(slice));
    }

    CHECK(absoluteEnergy >= 0.0);
    return decoded;
}

void roundtripByReencoding(const DecodedFile& decoded)
{
    VLError err = VL_OK;
    ScopedVLFile out(vl_create_new(decoded.info.channels, decoded.info.sample_rate, decoded.info.tempo, &err));
    REQUIRE(out);
    CHECK(err == VL_OK);

    VLFileInfo info = decoded.info;
    info.transient_enabled = 0;
    info.transient_stretch = 0;
    info.processing_gain = 1000;
    REQUIRE(vl_set_info(out.handle, &info) == VL_OK);

    for (size_t i = 0; i < decoded.slices.size(); ++i)
    {
        const DecodedSlice& slice = decoded.slices[i];
        CAPTURE(i);
        const int32_t added =
            vl_add_slice(out.handle, slice.info.ppq_pos, slice.left.data(), slice.right.empty() ? nullptr : slice.right.data(), (int32_t)slice.left.size());
        REQUIRE(added == (int32_t)i);
    }

    size_t required = 0;
    REQUIRE(vl_save_to_memory(out.handle, nullptr, &required) == VL_OK);
    REQUIRE(required > 64);

    std::vector<uint8_t> encoded(required - 1);
    size_t shortSize = encoded.size();
    CHECK(vl_save_to_memory(out.handle, encoded.data(), &shortSize) == VL_ERROR_BUFFER_TOO_SMALL);
    CHECK(shortSize == required);

    encoded.assign(required, 0);
    size_t encodedSize = encoded.size();
    REQUIRE(vl_save_to_memory(out.handle, encoded.data(), &encodedSize) == VL_OK);
    CHECK(encodedSize == required);

    ScopedVLFile reopened(vl_open_from_memory(encoded.data(), encoded.size(), &err));
    REQUIRE(reopened);
    CHECK(err == VL_OK);

    VLFileInfo reopenedInfo = {};
    REQUIRE(vl_get_info(reopened.handle, &reopenedInfo) == VL_OK);
    CHECK(reopenedInfo.channels == decoded.info.channels);
    CHECK(reopenedInfo.sample_rate == decoded.info.sample_rate);
    CHECK(reopenedInfo.slice_count == decoded.info.slice_count);
    CHECK(reopenedInfo.total_frames > 0);

    for (int32_t index = 0; index < reopenedInfo.slice_count; ++index)
    {
        CAPTURE(index);
        const int32_t frames = vl_get_slice_frame_count(reopened.handle, index);
        REQUIRE(frames > 0);

        std::vector<float> left((size_t)frames);
        std::vector<float> right(reopenedInfo.channels == 2 ? (size_t)frames : 0u);
        int32_t written = 0;
        REQUIRE(vl_decode_slice(reopened.handle, index, left.data(), right.empty() ? nullptr : right.data(), frames, &written) == VL_OK);
        CHECK(written == frames);
        checkDecodedBuffer(left);
        if (!right.empty())
        {
            checkDecodedBuffer(right);
        }
    }
}

std::vector<uint8_t> buildSmallEncodedFile(int32_t channels = 1, bool withCreator = false, VLFileInfo* savedInfo = nullptr)
{
    VLError err = VL_OK;
    ScopedVLFile file(vl_create_new(channels, 44100, 120000, &err));
    REQUIRE(file);
    REQUIRE(err == VL_OK);

    VLFileInfo info = {};
    REQUIRE(vl_get_info(file.handle, &info) == VL_OK);
    info.ppq_length = 15360;
    info.transient_enabled = 0;
    info.transient_stretch = 0;
    REQUIRE(vl_set_info(file.handle, &info) == VL_OK);

    if (withCreator)
    {
        VLCreatorInfo creator = {};
        std::strcpy(creator.name, "metadata");
        std::strcpy(creator.email, "metadata@example.invalid");
        REQUIRE(vl_set_creator_info(file.handle, &creator) == VL_OK);
    }

    std::vector<float> left(32);
    std::vector<float> right(channels == 2 ? 32u : 0u);
    for (size_t i = 0; i < left.size(); ++i)
    {
        left[i] = std::sin((float)i * 0.2f) * 0.25f;
        if (!right.empty())
        {
            right[i] = std::cos((float)i * 0.13f) * 0.15f;
        }
    }

    REQUIRE(vl_add_slice(file.handle, 0, left.data(), right.empty() ? nullptr : right.data(), (int32_t)left.size()) == 0);

    size_t size = 0;
    REQUIRE(vl_save_to_memory(file.handle, nullptr, &size) == VL_OK);
    std::vector<uint8_t> bytes(size);
    REQUIRE(vl_save_to_memory(file.handle, bytes.data(), &size) == VL_OK);
    bytes.resize(size);

    if (savedInfo)
    {
        REQUIRE(vl_get_info(file.handle, savedInfo) == VL_OK);
    }

    return bytes;
}

} // namespace

TEST_CASE("utility functions expose stable strings")
{
    CHECK(std::string(vl_version_string()).find("velociloops ") == 0);
    CHECK(std::string(vl_error_string(VL_OK)) == "OK");
    CHECK(std::string(vl_error_string(VL_ERROR_INVALID_HANDLE)) == "invalid handle");
    CHECK(std::string(vl_error_string(VL_ERROR_INVALID_ARG)) == "invalid argument");
    CHECK(std::string(vl_error_string(VL_ERROR_FILE_NOT_FOUND)) == "file not found");
    CHECK(std::string(vl_error_string(VL_ERROR_FILE_CORRUPT)) == "file corrupt or unsupported format");
    CHECK(std::string(vl_error_string(VL_ERROR_OUT_OF_MEMORY)) == "out of memory");
    CHECK(std::string(vl_error_string(VL_ERROR_INVALID_SLICE)) == "invalid slice index");
    CHECK(std::string(vl_error_string(VL_ERROR_INVALID_SAMPLE_RATE)) == "invalid sample rate");
    CHECK(std::string(vl_error_string(VL_ERROR_BUFFER_TOO_SMALL)) == "buffer too small");
    CHECK(std::string(vl_error_string(VL_ERROR_NO_CREATOR_INFO)) == "no creator info available");
    CHECK(std::string(vl_error_string(VL_ERROR_NOT_IMPLEMENTED)) == "not implemented");
    CHECK(std::string(vl_error_string(VL_ERROR_ALREADY_HAS_DATA)) == "already has data");
    CHECK(std::string(vl_error_string((VLError)-12345)) == "unknown error");
}

TEST_CASE("invalid API arguments return errors")
{
    VLError err = VL_OK;
    CHECK(vl_open(nullptr, &err) == nullptr);
    CHECK(err == VL_ERROR_INVALID_ARG);

    const fs::path missing = kDataDir / "does-not-exist.rx2";
    CHECK(vl_open(missing.string().c_str(), &err) == nullptr);
    CHECK(err == VL_ERROR_FILE_NOT_FOUND);

    CHECK(vl_open_from_memory(nullptr, 12, &err) == nullptr);
    CHECK(err == VL_ERROR_INVALID_ARG);
    const uint8_t tiny[] = {'C', 'A', 'T', ' '};
    CHECK(vl_open_from_memory(tiny, sizeof(tiny), &err) == nullptr);
    CHECK(err == VL_ERROR_FILE_CORRUPT);

    CHECK(vl_get_info(nullptr, nullptr) == VL_ERROR_INVALID_HANDLE);
    CHECK(vl_get_creator_info(nullptr, nullptr) == VL_ERROR_INVALID_HANDLE);
    CHECK(vl_get_slice_info(nullptr, 0, nullptr) == VL_ERROR_INVALID_HANDLE);
    CHECK(vl_set_output_sample_rate(nullptr, 44100) == VL_ERROR_INVALID_HANDLE);
    CHECK(vl_get_slice_frame_count(nullptr, 0) == VL_ERROR_INVALID_HANDLE);
    CHECK(vl_decode_slice(nullptr, 0, nullptr, nullptr, 0, nullptr) == VL_ERROR_INVALID_HANDLE);
    CHECK(vl_set_info(nullptr, nullptr) == VL_ERROR_INVALID_HANDLE);
    CHECK(vl_set_creator_info(nullptr, nullptr) == VL_ERROR_INVALID_HANDLE);
    CHECK(vl_add_slice(nullptr, 0, nullptr, nullptr, 0) == VL_ERROR_INVALID_HANDLE);
    CHECK(vl_remove_slice(nullptr, 0) == VL_ERROR_INVALID_HANDLE);
    CHECK(vl_save(nullptr, "out.rx2") == VL_ERROR_INVALID_HANDLE);
    CHECK(vl_save_to_memory(nullptr, nullptr, nullptr) == VL_ERROR_INVALID_HANDLE);
}

TEST_CASE("malformed and patched containers cover parser edge cases")
{
    SUBCASE("SINF bit depth codes are reported")
    {
        for (const auto& codeAndDepth : {std::pair<uint8_t, int32_t>{1, 8}, std::pair<uint8_t, int32_t>{7, 32}, std::pair<uint8_t, int32_t>{0xff, 16}})
        {
            CAPTURE((int)codeAndDepth.first);
            std::vector<uint8_t> bytes = buildSmallEncodedFile();
            const ChunkRange sinf = findChunk(bytes, "SINF");
            REQUIRE(sinf.found);
            REQUIRE(sinf.size >= 18);
            bytes[sinf.payload + 1] = codeAndDepth.first;

            VLError err = VL_OK;
            ScopedVLFile file(vl_open_from_memory(bytes.data(), bytes.size(), &err));
            REQUIRE(file);
            REQUIRE(err == VL_OK);

            VLFileInfo info = {};
            REQUIRE(vl_get_info(file.handle, &info) == VL_OK);
            CHECK(info.bit_depth == codeAndDepth.second);
        }
    }

    SUBCASE("truncated CREI strings do not make an otherwise valid file unreadable")
    {
        std::vector<uint8_t> bytes = buildSmallEncodedFile(1, true);
        const ChunkRange crei = findChunk(bytes, "CREI");
        REQUIRE(crei.found);
        REQUIRE(crei.size >= 4);
        putBe32(bytes, crei.payload, crei.size + 100u);

        VLError err = VL_OK;
        ScopedVLFile file(vl_open_from_memory(bytes.data(), bytes.size(), &err));
        REQUIRE(file);
        REQUIRE(err == VL_OK);

        VLCreatorInfo creator = {};
        CHECK(vl_get_creator_info(file.handle, &creator) == VL_ERROR_NO_CREATOR_INFO);
    }

    SUBCASE("out of range SLCE sample starts decode as silence")
    {
        VLFileInfo savedInfo = {};
        std::vector<uint8_t> bytes = buildSmallEncodedFile(1, false, &savedInfo);
        const ChunkRange slce = findChunk(bytes, "SLCE");
        REQUIRE(slce.found);
        REQUIRE(slce.size >= 11);
        putBe32(bytes, slce.payload, (uint32_t)savedInfo.total_frames + 16u);
        putBe32(bytes, slce.payload + 4, 8u);

        VLError err = VL_OK;
        ScopedVLFile file(vl_open_from_memory(bytes.data(), bytes.size(), &err));
        REQUIRE(file);
        REQUIRE(err == VL_OK);

        const int32_t frames = vl_get_slice_frame_count(file.handle, 0);
        REQUIRE(frames == 1);
        float left = 99.0f;
        float right = 99.0f;
        int32_t written = 0;
        REQUIRE(vl_decode_slice(file.handle, 0, &left, &right, 1, &written) == VL_OK);
        CHECK(written == 1);
        CHECK(left == 0.0f);
        CHECK(right == 0.0f);
    }
}

TEST_CASE("WAV fixtures are valid PCM references")
{
    int wavCount = 0;
    for (const fs::path& path : dataFiles())
    {
        if (path.extension() != ".wav")
        {
            continue;
        }
        CAPTURE(path.filename().string());
        WavInfo info;
        REQUIRE(readWavInfo(path, info));
        CHECK((info.channels == 1 || info.channels == 2));
        CHECK(info.sampleRate >= 8000);
        CHECK(info.sampleRate <= 192000);
        CHECK((info.bitDepth == 16 || info.bitDepth == 24));
        CHECK(info.dataBytes > 0);
        ++wavCount;
    }
    CHECK(wavCount == 17);
}

TEST_CASE("mono slice rendering matches decoded DWOP trace after render offset")
{
    VLError err = VL_OK;
    ScopedVLFile file(vl_open((kDataDir / "120Mono.rx2").string().c_str(), &err));
    REQUIRE(file);
    REQUIRE(err == VL_OK);

    VLFileInfo info = {};
    REQUIRE(vl_get_info(file.handle, &info) == VL_OK);

    const int32_t frames = vl_get_slice_frame_count(file.handle, 0);
    REQUIRE(frames >= 10);

    std::vector<float> left((size_t)frames);
    int32_t written = 0;
    REQUIRE(vl_decode_slice(file.handle, 0, left.data(), nullptr, frames, &written) == VL_OK);
    CHECK(written == frames);

    const int32_t expected[] = {-410, -209, 205, 564, 709, 585, 161, -349, -637, -561};
    const float gain = (float)info.processing_gain * 0.000833333354f;
    REQUIRE(gain > 0.0f);
    for (size_t i = 0; i < std::size(expected); ++i)
    {
        CAPTURE(i);
        CHECK((int32_t)std::lround(left[i] * 32768.0f / gain) == expected[i]);
    }
}

TEST_CASE("stereo files synthesize the leading loop slice reported by the official SDK")
{
    VLError err = VL_OK;
    ScopedVLFile file(vl_open((kDataDir / "120Stereo.rx2").string().c_str(), &err));
    REQUIRE(file);
    REQUIRE(err == VL_OK);

    VLFileInfo info = {};
    REQUIRE(vl_get_info(file.handle, &info) == VL_OK);
    CHECK(info.slice_count == 10);

    VLSliceInfo first = {};
    REQUIRE(vl_get_slice_info(file.handle, 0, &first) == VL_OK);
    CHECK(first.ppq_pos == 0);
    CHECK(first.sample_start == info.loop_start);
    CHECK(first.sample_length == 10890);
    CHECK(vl_get_slice_frame_count(file.handle, 0) == 16069);

    const int32_t frames = vl_get_slice_frame_count(file.handle, 0);
    std::vector<float> left((size_t)frames);
    std::vector<float> right((size_t)frames);
    int32_t written = 0;
    REQUIRE(vl_decode_slice(file.handle, 0, left.data(), right.data(), frames, &written) == VL_OK);
    CHECK(written == frames);
    CHECK(left[0] == doctest::Approx(-0.010498f).epsilon(0.001f));
    CHECK(right[0] == doctest::Approx(-0.009613f).epsilon(0.001f));
}

TEST_CASE("all test data files are classified by the public opener")
{
    const auto files = dataFiles();
    const auto expected = expectedOpenableContainers();
    CHECK(files.size() == 40);

    int decodableCount = 0;
    int rejectedCount = 0;
    for (const fs::path& path : files)
    {
        const std::string name = path.filename().string();
        CAPTURE(name);

        VLError err = VL_OK;
        ScopedVLFile file(vl_open(path.string().c_str(), &err));
        const bool shouldDecode = expected.count(name) != 0;
        if (shouldDecode)
        {
            REQUIRE(file);
            CHECK(err == VL_OK);
            ++decodableCount;
        }
        else
        {
            REQUIRE_FALSE(file);
            CHECK((err == VL_ERROR_FILE_CORRUPT || err == VL_ERROR_INVALID_ARG));
            ++rejectedCount;
        }

        if (isContainerExtension(path))
        {
            const auto bytes = readFile(path);
            VLError memErr = VL_OK;
            ScopedVLFile fromMemory(vl_open_from_memory(bytes.data(), bytes.size(), &memErr));
            CHECK((fromMemory.handle != nullptr) == shouldDecode);
            CHECK(memErr == (shouldDecode ? VL_OK : VL_ERROR_FILE_CORRUPT));
        }
    }

    CHECK(decodableCount == (int)expected.size());
    CHECK(rejectedCount == (int)(files.size() - expected.size()));
}

TEST_CASE("every decodable fixture renders every slice and roundtrips through memory")
{
    for (const std::string& name : expectedOpenableContainers())
    {
        CAPTURE(name);
        const fs::path path = kDataDir / name;

        VLError err = VL_OK;
        ScopedVLFile file(vl_open(path.string().c_str(), &err));
        REQUIRE(file);
        CHECK(err == VL_OK);

        VLFileInfo info = {};
        REQUIRE(vl_get_info(file.handle, &info) == VL_OK);
        CHECK(vl_get_info(file.handle, nullptr) == VL_ERROR_INVALID_ARG);
        CHECK(vl_get_creator_info(file.handle, nullptr) == VL_ERROR_INVALID_ARG);
        CHECK(vl_get_slice_info(file.handle, 0, nullptr) == VL_ERROR_INVALID_ARG);
        CHECK(vl_get_slice_info(file.handle, info.slice_count, nullptr) == VL_ERROR_INVALID_ARG);
        VLSliceInfo ignoredSlice = {};
        CHECK(vl_get_slice_info(file.handle, -1, &ignoredSlice) == VL_ERROR_INVALID_SLICE);
        CHECK(vl_get_slice_info(file.handle, info.slice_count, &ignoredSlice) == VL_ERROR_INVALID_SLICE);
        CHECK(vl_get_slice_frame_count(file.handle, -1) == VL_ERROR_INVALID_SLICE);
        CHECK(vl_get_slice_frame_count(file.handle, info.slice_count) == VL_ERROR_INVALID_SLICE);

        CHECK(vl_set_output_sample_rate(file.handle, 1) == VL_ERROR_INVALID_SAMPLE_RATE);
        CHECK(vl_set_output_sample_rate(file.handle, info.sample_rate) == VL_OK);
        const int32_t otherRate = info.sample_rate == 44100 ? 48000 : 44100;
        CHECK(vl_set_output_sample_rate(file.handle, otherRate) == VL_ERROR_NOT_IMPLEMENTED);

        VLCreatorInfo creator = {};
        const VLError creatorResult = vl_get_creator_info(file.handle, &creator);
        if (creatorResult == VL_OK)
        {
            CHECK((creator.name[0] || creator.copyright[0] || creator.url[0] || creator.email[0] || creator.free_text[0]));
        }
        else
        {
            CHECK(creatorResult == VL_ERROR_NO_CREATOR_INFO);
        }

        const int32_t firstFrames = vl_get_slice_frame_count(file.handle, 0);
        if (expectedRenderableContainers().count(name) != 0)
        {
            REQUIRE(firstFrames > 0);
            std::vector<float> tooSmall((size_t)firstFrames);
            CHECK(vl_decode_slice(file.handle, 0, nullptr, nullptr, firstFrames, nullptr) == VL_ERROR_INVALID_ARG);
            CHECK(vl_decode_slice(file.handle, 0, tooSmall.data(), nullptr, firstFrames - 1, nullptr) == VL_ERROR_BUFFER_TOO_SMALL);
            CHECK(vl_decode_slice(file.handle, -1, tooSmall.data(), nullptr, firstFrames, nullptr) == VL_ERROR_INVALID_SLICE);
            CHECK(vl_decode_slice(file.handle, info.slice_count, tooSmall.data(), nullptr, firstFrames, nullptr) == VL_ERROR_INVALID_SLICE);
        }
        else
        {
            CHECK(info.slice_count == 0);
            CHECK(firstFrames == VL_ERROR_INVALID_SLICE);
        }

        DecodedFile decoded;
        if (expectedRenderableContainers().count(name) != 0)
        {
            decoded = decodeWholeFile(file.handle);
        }
        else
        {
            checkInfoSane(info);
        }

        size_t originalSize = 0;
        REQUIRE(vl_save_to_memory(file.handle, nullptr, &originalSize) == VL_OK);
        CHECK(originalSize == readFile(path).size());

        std::vector<uint8_t> shortOriginal(originalSize > 0 ? originalSize - 1 : 0);
        size_t shortOriginalSize = shortOriginal.size();
        CHECK(vl_save_to_memory(file.handle, shortOriginal.data(), &shortOriginalSize) == VL_ERROR_BUFFER_TOO_SMALL);
        CHECK(shortOriginalSize == originalSize);

        std::vector<uint8_t> originalCopy(originalSize);
        size_t copySize = originalCopy.size();
        REQUIRE(vl_save_to_memory(file.handle, originalCopy.data(), &copySize) == VL_OK);
        CHECK(originalCopy == readFile(path));
        CHECK(vl_save_to_memory(file.handle, originalCopy.data(), nullptr) == VL_ERROR_INVALID_ARG);

        if (expectedRenderableContainers().count(name) != 0)
        {
            roundtripByReencoding(decoded);
        }
    }
}

TEST_CASE("fresh mono and stereo files can be assembled, mutated, saved, and reopened")
{
    CHECK(vl_create_new(0, 44100, 120000, nullptr) == nullptr);
    CHECK(vl_create_new(1, 7999, 120000, nullptr) == nullptr);
    CHECK(vl_create_new(1, 192001, 120000, nullptr) == nullptr);
    CHECK(vl_create_new(1, 44100, 0, nullptr) == nullptr);

    SUBCASE("empty files and invalid metadata fail before encoding")
    {
        VLError err = VL_OK;
        ScopedVLFile file(vl_create_new(1, 44100, 120000, &err));
        REQUIRE(file);

        size_t size = 0;
        CHECK(vl_save_to_memory(file.handle, nullptr, &size) == VL_ERROR_INVALID_ARG);

        VLFileInfo info = {};
        REQUIRE(vl_get_info(file.handle, &info) == VL_OK);
        info.sample_rate = 7999;
        CHECK(vl_set_info(file.handle, &info) == VL_ERROR_INVALID_SAMPLE_RATE);
        info.sample_rate = 44100;
        info.tempo = 0;
        CHECK(vl_set_info(file.handle, &info) == VL_ERROR_INVALID_ARG);
        CHECK(vl_set_info(file.handle, nullptr) == VL_ERROR_INVALID_ARG);

        VLCreatorInfo creator = {};
        CHECK(vl_set_creator_info(file.handle, nullptr) == VL_ERROR_INVALID_ARG);
        CHECK(vl_set_creator_info(file.handle, &creator) == VL_OK);
    }

    SUBCASE("metadata normalization repairs unset timing and invalid loop bounds")
    {
        for (const bool invalidLoopStart : {false, true})
        {
            CAPTURE(invalidLoopStart);
            VLError err = VL_OK;
            ScopedVLFile file(vl_create_new(1, 44100, 120000, &err));
            REQUIRE(file);

            VLFileInfo info = {};
            REQUIRE(vl_get_info(file.handle, &info) == VL_OK);
            info.original_tempo = 0;
            info.ppq_length = 0;
            info.time_sig_num = 0;
            info.time_sig_den = 0;
            info.processing_gain = 0;
            info.transient_enabled = 0;
            info.transient_decay = 0;
            info.transient_stretch = 250;
            info.silence_selected = 1;
            if (invalidLoopStart)
            {
                info.loop_start = 1000;
                info.loop_end = 0;
            }
            else
            {
                info.loop_start = 0;
                info.loop_end = 2000;
            }
            REQUIRE(vl_set_info(file.handle, &info) == VL_OK);

            std::vector<float> samples(64);
            for (size_t i = 0; i < samples.size(); ++i)
            {
                samples[i] = (i % 2 == 0) ? 1.25f : -1.25f;
            }
            REQUIRE(vl_add_slice(file.handle, 0, samples.data(), nullptr, (int32_t)samples.size()) == 0);

            size_t size = 0;
            REQUIRE(vl_save_to_memory(file.handle, nullptr, &size) == VL_OK);
            std::vector<uint8_t> bytes(size);
            REQUIRE(vl_save_to_memory(file.handle, bytes.data(), &size) == VL_OK);

            ScopedVLFile reopened(vl_open_from_memory(bytes.data(), bytes.size(), &err));
            REQUIRE(reopened);
            VLFileInfo reopenedInfo = {};
            REQUIRE(vl_get_info(reopened.handle, &reopenedInfo) == VL_OK);
            CHECK(reopenedInfo.loop_start == 0);
            CHECK(reopenedInfo.loop_end == reopenedInfo.total_frames);
            CHECK(reopenedInfo.ppq_length > 0);
            CHECK(reopenedInfo.time_sig_num == 4);
            CHECK(reopenedInfo.time_sig_den == 4);
            CHECK(reopenedInfo.processing_gain == 1000);
            CHECK(reopenedInfo.transient_enabled == 0);
            CHECK(reopenedInfo.transient_decay == 1023);
            CHECK(reopenedInfo.transient_stretch == 100);
            CHECK(reopenedInfo.silence_selected == 1);
        }
    }

    SUBCASE("mono file with creator metadata")
    {
        VLError err = VL_OK;
        ScopedVLFile file(vl_create_new(1, 44100, 120000, &err));
        REQUIRE(file);
        CHECK(err == VL_OK);

        VLFileInfo info = {};
        REQUIRE(vl_get_info(file.handle, &info) == VL_OK);
        info.ppq_length = 15360 * 2;
        info.transient_enabled = 0;
        info.transient_stretch = 0;
        REQUIRE(vl_set_info(file.handle, &info) == VL_OK);

        VLCreatorInfo creator = {};
        std::strcpy(creator.name, "VelociLoops Tests");
        std::strcpy(creator.copyright, "Public test fixture");
        std::strcpy(creator.url, "https://example.invalid/velociloops");
        std::strcpy(creator.email, "tests@example.invalid");
        std::strcpy(creator.free_text, "roundtrip metadata");
        REQUIRE(vl_set_creator_info(file.handle, &creator) == VL_OK);

        std::vector<float> first(128);
        std::vector<float> second(96);
        for (size_t i = 0; i < first.size(); ++i)
        {
            first[i] = (float)i / (float)first.size() * 0.5f - 0.25f;
        }
        for (size_t i = 0; i < second.size(); ++i)
        {
            second[i] = std::sin((float)i * 0.1f) * 0.35f;
        }

        CHECK(vl_add_slice(file.handle, -1, first.data(), nullptr, (int32_t)first.size()) == VL_ERROR_INVALID_ARG);
        CHECK(vl_add_slice(file.handle, 0, nullptr, nullptr, (int32_t)first.size()) == VL_ERROR_INVALID_ARG);
        CHECK(vl_add_slice(file.handle, 0, first.data(), nullptr, 0) == VL_ERROR_INVALID_ARG);
        REQUIRE(vl_add_slice(file.handle, 0, first.data(), nullptr, (int32_t)first.size()) == 0);
        REQUIRE(vl_add_slice(file.handle, 15360, second.data(), nullptr, (int32_t)second.size()) == 1);

        CHECK(vl_set_info(file.handle, &info) == VL_ERROR_ALREADY_HAS_DATA);
        CHECK(vl_set_creator_info(file.handle, &creator) == VL_ERROR_ALREADY_HAS_DATA);
        CHECK(vl_remove_slice(file.handle, -1) == VL_ERROR_INVALID_SLICE);
        CHECK(vl_remove_slice(file.handle, 2) == VL_ERROR_INVALID_SLICE);
        REQUIRE(vl_remove_slice(file.handle, 1) == VL_OK);
        REQUIRE(vl_add_slice(file.handle, 15360, second.data(), nullptr, (int32_t)second.size()) == 1);

        CHECK(vl_save(file.handle, nullptr) == VL_ERROR_INVALID_ARG);

        size_t size = 0;
        REQUIRE(vl_save_to_memory(file.handle, nullptr, &size) == VL_OK);
        REQUIRE(size > 64);
        std::vector<uint8_t> bytes(size);
        REQUIRE(vl_save_to_memory(file.handle, bytes.data(), &size) == VL_OK);

        const fs::path savedPath = fs::temp_directory_path() / "velociloops_test_save.rx2";
        fs::remove(savedPath);
        REQUIRE(vl_save(file.handle, savedPath.string().c_str()) == VL_OK);
        CHECK(fs::exists(savedPath));
        CHECK(vl_save(file.handle, fs::temp_directory_path().string().c_str()) == VL_ERROR_INVALID_ARG);
        fs::remove(savedPath);

        ScopedVLFile reopened(vl_open_from_memory(bytes.data(), bytes.size(), &err));
        REQUIRE(reopened);
        VLFileInfo reopenedInfo = {};
        REQUIRE(vl_get_info(reopened.handle, &reopenedInfo) == VL_OK);
        CHECK(reopenedInfo.channels == 1);
        CHECK(reopenedInfo.sample_rate == 44100);
        CHECK(reopenedInfo.slice_count == 2);

        VLCreatorInfo reopenedCreator = {};
        REQUIRE(vl_get_creator_info(reopened.handle, &reopenedCreator) == VL_OK);
        CHECK(std::string(reopenedCreator.name) == "VelociLoops Tests");
        CHECK(std::string(reopenedCreator.email) == "tests@example.invalid");

        const DecodedFile decoded = decodeWholeFile(reopened.handle);
        CHECK(decoded.slices.size() == 2);
    }

    SUBCASE("stereo file keeps independent channels")
    {
        VLError err = VL_OK;
        ScopedVLFile file(vl_create_new(2, 48000, 90000, &err));
        REQUIRE(file);
        CHECK(err == VL_OK);

        VLFileInfo info = {};
        REQUIRE(vl_get_info(file.handle, &info) == VL_OK);
        info.ppq_length = 15360;
        info.transient_enabled = 0;
        info.transient_stretch = 0;
        REQUIRE(vl_set_info(file.handle, &info) == VL_OK);

        std::vector<float> left(160);
        std::vector<float> right(160);
        for (size_t i = 0; i < left.size(); ++i)
        {
            left[i] = std::sin((float)i * 0.07f) * 0.4f;
            right[i] = std::cos((float)i * 0.11f) * 0.2f;
        }

        CHECK(vl_add_slice(file.handle, 0, left.data(), nullptr, (int32_t)left.size()) == VL_ERROR_INVALID_ARG);
        REQUIRE(vl_add_slice(file.handle, 0, left.data(), right.data(), (int32_t)left.size()) == 0);

        size_t size = 0;
        REQUIRE(vl_save_to_memory(file.handle, nullptr, &size) == VL_OK);
        std::vector<uint8_t> bytes(size);
        REQUIRE(vl_save_to_memory(file.handle, bytes.data(), &size) == VL_OK);

        ScopedVLFile reopened(vl_open_from_memory(bytes.data(), bytes.size(), &err));
        REQUIRE(reopened);
        VLFileInfo reopenedInfo = {};
        REQUIRE(vl_get_info(reopened.handle, &reopenedInfo) == VL_OK);
        CHECK(reopenedInfo.channels == 2);
        CHECK(reopenedInfo.sample_rate == 48000);
        CHECK(reopenedInfo.slice_count == 1);

        const int32_t frames = vl_get_slice_frame_count(reopened.handle, 0);
        REQUIRE(frames > 0);
        std::vector<float> decodedLeft((size_t)frames);
        std::vector<float> decodedRight((size_t)frames);
        int32_t written = 0;
        REQUIRE(vl_decode_slice(reopened.handle, 0, decodedLeft.data(), decodedRight.data(), frames, &written) == VL_OK);
        CHECK(written == frames);
        checkDecodedBuffer(decodedLeft);
        checkDecodedBuffer(decodedRight);
        CHECK(std::inner_product(decodedLeft.begin(), decodedLeft.end(), decodedRight.begin(), 0.0, std::plus<double>(),
                                 [](float l, float r)
                                 {
                                     return std::fabs(l - r);
                                 }) > 0.01);
    }
}
