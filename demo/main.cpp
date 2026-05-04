#include "velociloops.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

/* -----------------------------------------------------------------------
   Minimal WAV reader/writer
   ----------------------------------------------------------------------- */

struct WavData
{
    int32_t channels = 0;
    int32_t sampleRate = 0;
    std::vector<float> left;
    std::vector<float> right;
};

static uint16_t readLE16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t readLE32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t readSignedLE(const uint8_t* p, uint16_t bits)
{
    if (bits == 8)
        return (int32_t)p[0] - 128;
    if (bits == 16)
        return (int16_t)readLE16(p);
    if (bits == 24)
    {
        int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
        if (v & 0x800000)
            v |= ~0x00ffffff;
        return v;
    }
    if (bits == 32)
        return (int32_t)readLE32(p);
    return 0;
}

static bool readWav(const char* path, WavData& out, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "failed to open file";
        return false;
    }

    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
    {
        error = "not a RIFF/WAVE file";
        return false;
    }

    uint16_t format = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t blockAlign = 0;
    uint16_t bits = 0;
    const uint8_t* data = nullptr;
    uint32_t dataBytes = 0;
    bool sawFmt = false;

    size_t off = 12;
    while (off + 8 <= bytes.size())
    {
        const uint8_t* chunk = bytes.data() + off;
        const uint32_t size = readLE32(chunk + 4);
        off += 8;
        if (off + size > bytes.size())
        {
            error = "truncated chunk";
            return false;
        }

        if (std::memcmp(chunk, "fmt ", 4) == 0)
        {
            if (size < 16)
            {
                error = "short fmt chunk";
                return false;
            }

            format = readLE16(bytes.data() + off);
            channels = readLE16(bytes.data() + off + 2);
            sampleRate = readLE32(bytes.data() + off + 4);
            blockAlign = readLE16(bytes.data() + off + 12);
            bits = readLE16(bytes.data() + off + 14);

            if (format == 0xfffe && size >= 40)
            {
                const uint16_t validBits = readLE16(bytes.data() + off + 18);
                const uint16_t subFormat = readLE16(bytes.data() + off + 24);
                format = subFormat;
                if (validBits != 0)
                    bits = validBits;
            }
            sawFmt = true;
        }
        else if (std::memcmp(chunk, "data", 4) == 0)
        {
            data = bytes.data() + off;
            dataBytes = size;
        }

        off += size + (size & 1u);
    }

    if (!sawFmt || !data)
    {
        error = "missing fmt or data chunk";
        return false;
    }
    if (channels != 1 && channels != 2)
    {
        error = "only mono and stereo WAV files are supported";
        return false;
    }
    if (sampleRate < 8000 || sampleRate > 192000)
    {
        error = "unsupported sample rate";
        return false;
    }
    if (format != 1 && format != 3)
    {
        error = "only PCM and IEEE float WAV files are supported";
        return false;
    }
    if (format == 1 && bits != 8 && bits != 16 && bits != 24 && bits != 32)
    {
        error = "unsupported PCM bit depth";
        return false;
    }
    if (format == 3 && bits != 32)
    {
        error = "only 32-bit float WAV files are supported";
        return false;
    }

    const uint16_t sampleBytes = (uint16_t)((bits + 7) / 8);
    const uint16_t expectedAlign = (uint16_t)(channels * sampleBytes);
    if (blockAlign < expectedAlign || blockAlign == 0)
    {
        error = "invalid block alignment";
        return false;
    }

    const uint32_t frames = dataBytes / blockAlign;
    if (frames == 0)
    {
        error = "empty WAV data";
        return false;
    }

    out.channels = channels;
    out.sampleRate = (int32_t)sampleRate;
    out.left.assign(frames, 0.0f);
    out.right.assign(channels == 2 ? frames : 0, 0.0f);

    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        const uint8_t* src = data + (size_t)frame * blockAlign;
        for (uint16_t ch = 0; ch < channels; ++ch)
        {
            const uint8_t* samplePtr = src + (size_t)ch * sampleBytes;
            float sample = 0.0f;
            if (format == 3)
            {
                float value = 0.0f;
                std::memcpy(&value, samplePtr, sizeof(value));
                sample = std::isfinite(value) ? value : 0.0f;
            }
            else
            {
                const int32_t value = readSignedLE(samplePtr, bits);
                const float scale = bits == 8 ? 128.0f : (float)(1u << (bits - 1));
                sample = (float)value / scale;
            }

            sample = std::clamp(sample, -1.0f, 1.0f);
            if (ch == 0)
                out.left[frame] = sample;
            else
                out.right[frame] = sample;
        }
    }

    return true;
}

static bool writeWav(const char* path, const float* left, const float* right, int32_t frames, int32_t sampleRate)
{
    const int channels = right ? 2 : 1;
    const int byteDepth = 2;
    const int dataBytes = frames * channels * byteDepth;

    FILE* f = fopen(path, "wb");
    if (!f)
        return false;

    auto w16 = [&](uint16_t v)
    {
        fwrite(&v, 2, 1, f);
    };

    auto w32 = [&](uint32_t v)
    {
        fwrite(&v, 4, 1, f);
    };

    fwrite("RIFF", 1, 4, f);
    w32((uint32_t)(36 + dataBytes));
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    w32(16);
    w16(1); /* PCM */
    w16((uint16_t)channels);
    w32((uint32_t)sampleRate);
    w32((uint32_t)(sampleRate * channels * byteDepth));
    w16((uint16_t)(channels * byteDepth));
    w16(16);
    fwrite("data", 1, 4, f);
    w32((uint32_t)dataBytes);

    for (int32_t i = 0; i < frames; ++i)
    {
        auto toS16 = [](float s) -> int16_t
        {
            const float c = s < -1.f ? -1.f : (s > 1.f ? 1.f : s);
            return (int16_t)(c * 32767.f);
        };

        int16_t l = toS16(left[i]);
        fwrite(&l, 2, 1, f);
        if (right)
        {
            int16_t r = toS16(right[i]);
            fwrite(&r, 2, 1, f);
        }
    }

    fclose(f);
    return true;
}

static std::string lowerExtension(const char* path)
{
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c)
                   {
                       return (char)std::tolower(c);
                   });
    return ext;
}

static int convertWavToRx2(const char* inputPath, const char* outputPath, int32_t tempo)
{
    WavData wav;
    std::string error;
    if (!readWav(inputPath, wav, error))
    {
        fprintf(stderr, "Failed to read WAV '%s': %s\n", inputPath, error.c_str());
        return 1;
    }

    VLSuperFluxOptions options = {};
    vl_superflux_default_options(&options);

    VLError err = VL_OK;
    VLFile file = vl_create_from_superflux(wav.channels,
                                           wav.sampleRate,
                                           tempo,
                                           wav.left.data(),
                                           wav.channels == 2 ? wav.right.data() : nullptr,
                                           (int32_t)wav.left.size(),
                                           &options,
                                           &err);
    if (!file)
    {
        fprintf(stderr, "Failed to slice WAV '%s': %s\n", inputPath, vl_error_string(err));
        return 1;
    }

    VLFileInfo info = {};
    vl_get_info(file, &info);

    err = vl_save(file, outputPath);
    if (err != VL_OK)
    {
        fprintf(stderr, "Failed to write RX2 '%s': %s\n", outputPath, vl_error_string(err));
        vl_close(file);
        return 1;
    }

    printf("=== %s ===\n", inputPath);
    printf("  wrote        : %s\n", outputPath);
    printf("  channels     : %d\n", info.channels);
    printf("  sample rate  : %d Hz\n", info.sample_rate);
    printf("  total frames : %d\n", info.total_frames);
    printf("  tempo        : %.3f BPM\n", info.tempo / 1000.0);
    printf("  ppq length   : %d\n", info.ppq_length);
    printf("  slices       : %d\n", info.slice_count);

    for (int32_t i = 0; i < info.slice_count; ++i)
    {
        VLSliceInfo si = {};
        vl_get_slice_info(file, i, &si);
        printf("  slice %-4d  start=%-8d  frames=%-8d  ppq=%d\n",
               i, si.sample_start, si.sample_length, si.ppq_pos);
    }

    vl_close(file);
    return 0;
}

/* -----------------------------------------------------------------------
   Main
   ----------------------------------------------------------------------- */

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s <file.rx2> [output_dir]\n", argv[0]);
        fprintf(stderr, "  %s <file.wav> [output.rx2|output_dir] [tempo_bpm]\n", argv[0]);
        return 1;
    }

    const char* inputPath = argv[1];
    if (lowerExtension(inputPath) == ".wav")
    {
        double bpm = 120.0;
        if (argc >= 4)
        {
            char* end = nullptr;
            bpm = std::strtod(argv[3], &end);
            if (!end || *end != '\0' || bpm <= 0.0)
            {
                fprintf(stderr, "Invalid tempo '%s'. Use BPM, e.g. 120 or 87.5.\n", argv[3]);
                return 1;
            }
        }

        std::filesystem::path outputPath;
        if (argc >= 3)
        {
            outputPath = argv[2];
            std::error_code dirEc;
            if (std::filesystem::is_directory(outputPath, dirEc))
            {
                std::filesystem::path filename = std::filesystem::path(inputPath).stem();
                filename.replace_extension(".rx2");
                outputPath /= filename;
            }
        }
        else
        {
            outputPath = std::filesystem::path(inputPath).replace_extension(".rx2");
        }

        if (outputPath.empty())
        {
            fprintf(stderr, "Invalid output path.\n");
            return 1;
        }

        if (outputPath.extension().empty())
            outputPath.replace_extension(".rx2");

        if (!outputPath.parent_path().empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(outputPath.parent_path(), ec);
            if (ec)
            {
                fprintf(stderr, "Failed to create output directory '%s': %s\n",
                        outputPath.parent_path().string().c_str(), ec.message().c_str());
                return 1;
            }
        }

        const int32_t tempo = (int32_t)std::lround(bpm * 1000.0);
        return convertWavToRx2(inputPath, outputPath.string().c_str(), tempo);
    }

    const char* outDir = argc >= 3 ? argv[2] : ".";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec)
    {
        fprintf(stderr, "Failed to create output directory '%s': %s\n", outDir, ec.message().c_str());
        return 1;
    }

    /* --- open --- */
    VLError err = VL_OK;
    VLFile file = vl_open(inputPath, &err);
    if (!file)
    {
        fprintf(stderr, "Failed to open '%s': %s\n", inputPath, vl_error_string(err));
        return 1;
    }

    /* --- file metadata --- */
    VLFileInfo info;
    vl_get_info(file, &info);

    printf("=== %s ===\n", inputPath);
    printf("  channels     : %d\n", info.channels);
    printf("  sample rate  : %d Hz\n", info.sample_rate);
    printf("  bit depth    : %d\n", info.bit_depth);
    printf("  total frames : %d\n", info.total_frames);
    printf("  loop start   : %d\n", info.loop_start);
    printf("  loop end     : %d\n", info.loop_end);
    printf("  tempo        : %.3f BPM\n", info.tempo / 1000.0);
    printf("  orig. tempo  : %.3f BPM\n", info.original_tempo / 1000.0);
    printf("  time sig     : %d/%d\n", info.time_sig_num, info.time_sig_den);
    printf("  ppq length   : %d\n", info.ppq_length);
    printf("  slices       : %d\n", info.slice_count);
    printf("  gain         : %d\n", info.processing_gain);
    printf("  transient    : %s  attack=%d decay=%d stretch=%d\n\n", info.transient_enabled ? "on" : "off", info.transient_attack, info.transient_decay,
           info.transient_stretch);

    VLCreatorInfo creator = {};
    if (vl_get_creator_info(file, &creator) == VL_OK)
    {
        if (creator.name[0])
            printf("  creator name : %s\n", creator.name);
        if (creator.copyright[0])
            printf("  copyright    : %s\n", creator.copyright);
        if (creator.url[0])
            printf("  url          : %s\n", creator.url);
        if (creator.email[0])
            printf("  email        : %s\n", creator.email);
        if (creator.free_text[0])
            printf("  free text    : %s\n", creator.free_text);
        printf("\n");
    }

    /* --- slices --- */
    printf("  %-6s  %-10s  %-10s  %-10s  %-10s\n", "slice", "ppq_pos", "smp_start", "smp_len", "out_frames");
    printf("  %-6s  %-10s  %-10s  %-10s  %-10s\n", "------", "----------", "----------", "----------", "----------");

    for (int32_t i = 0; i < info.slice_count; ++i)
    {
        VLSliceInfo si;
        vl_get_slice_info(file, i, &si);
        const int32_t frames = vl_get_slice_frame_count(file, i);
        printf("  %-6d  %-10d  %-10d  %-10d  %-10d\n", i, si.ppq_pos, si.sample_start, si.sample_length, frames);
    }
    printf("\n");

    /* --- extract slices to WAV --- */
    const bool stereo = info.channels >= 2;
    int exported = 0, failed = 0;

    for (int32_t i = 0; i < info.slice_count; ++i)
    {
        const int32_t frames = vl_get_slice_frame_count(file, i);
        if (frames <= 0)
        {
            ++failed;
            continue;
        }

        std::vector<float> lbuf((size_t)frames);
        std::vector<float> rbuf((size_t)frames);

        float* rptr = stereo ? rbuf.data() : nullptr;
        int32_t written = 0;
        err = vl_decode_slice(file, i, lbuf.data(), rptr, 0, frames, &written);
        if (err != VL_OK)
        {
            fprintf(stderr, "  slice %d: decode error: %s\n", i, vl_error_string(err));
            ++failed;
            continue;
        }

        char path[512];
        std::snprintf(path, sizeof(path), "%s/slice_%03d.wav", outDir, i);

        if (!writeWav(path, lbuf.data(), rptr, written, info.sample_rate))
        {
            fprintf(stderr, "  slice %d: failed to write '%s'\n", i, path);
            ++failed;
            continue;
        }

        printf("  wrote %s  (%d frames)\n", path, written);
        ++exported;
    }

    printf("\n  %d/%d slices exported", exported, info.slice_count);
    if (failed)
        printf(", %d failed", failed);
    printf("\n");

    if (!failed)
    {
        const std::filesystem::path roundtripPath = std::filesystem::path(outDir) / "roundtrip.rx2";
        VLError roundtripErr = vl_save(file, roundtripPath.string().c_str());
        if (roundtripErr != VL_OK)
        {
            fprintf(stderr, "Failed to write roundtrip RX2 '%s': %s\n", roundtripPath.string().c_str(), vl_error_string(roundtripErr));
            failed = 1;
        }
        else
        {
            printf("  wrote %s\n", roundtripPath.string().c_str());

            VLError reopenErr = VL_OK;
            VLFile reopened = vl_open(roundtripPath.string().c_str(), &reopenErr);
            if (!reopened)
            {
                fprintf(stderr, "Failed to reopen roundtrip RX2: %s\n", vl_error_string(reopenErr));
                failed = 1;
            }
            else
            {
                VLFileInfo rtInfo;
                vl_get_info(reopened, &rtInfo);
                printf("  roundtrip reopen: %d/%d slices, %d/%d frames, %.3f BPM, %d channel%s\n", rtInfo.slice_count, info.slice_count, rtInfo.total_frames,
                       info.total_frames, rtInfo.tempo / 1000.0, rtInfo.channels, rtInfo.channels == 1 ? "" : "s");
                if (rtInfo.slice_count != info.slice_count || rtInfo.total_frames != info.total_frames)
                {
                    fprintf(stderr, "Roundtrip metadata mismatch for '%s'\n", inputPath);
                    failed = 1;
                }
                vl_close(reopened);
            }
        }
    }

    vl_close(file);
    return failed ? 1 : 0;
}
