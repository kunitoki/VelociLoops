#include "velociloops.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>
#include <string>

/* -----------------------------------------------------------------------
   Minimal WAV writer
   ----------------------------------------------------------------------- */

static bool writeWav(const char* path, const float* left, const float* right,
                     int32_t frames, int32_t sampleRate) {
    const int channels  = right ? 2 : 1;
    const int byteDepth = 2;
    const int dataBytes = frames * channels * byteDepth;

    FILE* f = fopen(path, "wb");
    if (!f) return false;

    auto w16 = [&](uint16_t v){ fwrite(&v, 2, 1, f); };
    auto w32 = [&](uint32_t v){ fwrite(&v, 4, 1, f); };

    fwrite("RIFF", 1, 4, f);
    w32((uint32_t)(36 + dataBytes));
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    w32(16);
    w16(1);                          /* PCM */
    w16((uint16_t)channels);
    w32((uint32_t)sampleRate);
    w32((uint32_t)(sampleRate * channels * byteDepth));
    w16((uint16_t)(channels * byteDepth));
    w16(16);
    fwrite("data", 1, 4, f);
    w32((uint32_t)dataBytes);

    for (int32_t i = 0; i < frames; ++i) {
        auto toS16 = [](float s) -> int16_t {
            const float c = s < -1.f ? -1.f : (s > 1.f ? 1.f : s);
            return (int16_t)(c * 32767.f);
        };
        int16_t l = toS16(left[i]);
        fwrite(&l, 2, 1, f);
        if (right) {
            int16_t r = toS16(right[i]);
            fwrite(&r, 2, 1, f);
        }
    }

    fclose(f);
    return true;
}

/* -----------------------------------------------------------------------
   Main
   ----------------------------------------------------------------------- */

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.rx2> [output_dir]\n", argv[0]);
        return 1;
    }

    const char* inputPath = argv[1];
    const char* outDir    = argc >= 3 ? argv[2] : ".";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        fprintf(stderr, "Failed to create output directory '%s': %s\n",
                outDir, ec.message().c_str());
        return 1;
    }

    /* --- open --- */
    VLError err = VL_OK;
    VLFile  file = vl_open(inputPath, &err);
    if (!file) {
        fprintf(stderr, "Failed to open '%s': %s\n", inputPath, vl_error_string(err));
        return 1;
    }

    /* --- file metadata --- */
    VLFileInfo info;
    vl_get_info(file, &info);

    printf("=== %s ===\n", inputPath);
    printf("  channels     : %d\n",   info.channels);
    printf("  sample rate  : %d Hz\n", info.sample_rate);
    printf("  bit depth    : %d\n",   info.bit_depth);
    printf("  total frames : %d\n",   info.total_frames);
    printf("  loop start   : %d\n",   info.loop_start);
    printf("  loop end     : %d\n",   info.loop_end);
    printf("  tempo        : %.3f BPM\n", info.tempo / 1000.0);
    printf("  orig. tempo  : %.3f BPM\n", info.original_tempo / 1000.0);
    printf("  time sig     : %d/%d\n", info.time_sig_num, info.time_sig_den);
    printf("  ppq length   : %d\n",   info.ppq_length);
    printf("  slices       : %d\n",   info.slice_count);
    printf("  gain         : %d\n",   info.processing_gain);
    printf("  transient    : %s  attack=%d decay=%d stretch=%d\n\n",
           info.transient_enabled ? "on" : "off",
           info.transient_attack,
           info.transient_decay,
           info.transient_stretch);

    VLCreatorInfo creator = {};
    if (vl_get_creator_info(file, &creator) == VL_OK) {
        if (creator.name[0])      printf("  creator name : %s\n", creator.name);
        if (creator.copyright[0]) printf("  copyright    : %s\n", creator.copyright);
        if (creator.url[0])       printf("  url          : %s\n", creator.url);
        if (creator.email[0])     printf("  email        : %s\n", creator.email);
        if (creator.free_text[0]) printf("  free text    : %s\n", creator.free_text);
        printf("\n");
    }

    /* --- slices --- */
    printf("  %-6s  %-10s  %-10s  %-10s  %-10s\n",
           "slice", "ppq_pos", "smp_start", "smp_len", "out_frames");
    printf("  %-6s  %-10s  %-10s  %-10s  %-10s\n",
           "------", "----------", "----------", "----------", "----------");

    for (int32_t i = 0; i < info.slice_count; ++i) {
        VLSliceInfo si;
        vl_get_slice_info(file, i, &si);
        const int32_t frames = vl_get_slice_frame_count(file, i);
        printf("  %-6d  %-10d  %-10d  %-10d  %-10d\n",
               i, si.ppq_pos, si.sample_start, si.sample_length, frames);
    }
    printf("\n");

    /* --- extract slices to WAV --- */
    const bool stereo = info.channels >= 2;
    int exported = 0, failed = 0;

    for (int32_t i = 0; i < info.slice_count; ++i) {
        const int32_t frames = vl_get_slice_frame_count(file, i);
        if (frames <= 0) { ++failed; continue; }

        std::vector<float> lbuf((size_t)frames);
        std::vector<float> rbuf((size_t)frames);

        float* rptr = stereo ? rbuf.data() : nullptr;
        int32_t written = 0;
        err = vl_decode_slice(file, i, lbuf.data(), rptr, frames, &written);
        if (err != VL_OK) {
            fprintf(stderr, "  slice %d: decode error: %s\n", i, vl_error_string(err));
            ++failed;
            continue;
        }

        char path[512];
        std::snprintf(path, sizeof(path), "%s/slice_%03d.wav", outDir, i);

        if (!writeWav(path, lbuf.data(), rptr, written, info.sample_rate)) {
            fprintf(stderr, "  slice %d: failed to write '%s'\n", i, path);
            ++failed;
            continue;
        }

        printf("  wrote %s  (%d frames)\n", path, written);
        ++exported;
    }

    printf("\n  %d/%d slices exported", exported, info.slice_count);
    if (failed) printf(", %d failed", failed);
    printf("\n");

    if (!failed) {
        const std::filesystem::path roundtripPath =
            std::filesystem::path(outDir) / "roundtrip.rx2";
        VLError roundtripErr = vl_save(file, roundtripPath.string().c_str());
        if (roundtripErr != VL_OK) {
            fprintf(stderr, "Failed to write roundtrip RX2 '%s': %s\n",
                    roundtripPath.string().c_str(), vl_error_string(roundtripErr));
            failed = 1;
        } else {
            printf("  wrote %s\n", roundtripPath.string().c_str());

            VLError reopenErr = VL_OK;
            VLFile reopened = vl_open(roundtripPath.string().c_str(), &reopenErr);
            if (!reopened) {
                fprintf(stderr, "Failed to reopen roundtrip RX2: %s\n", vl_error_string(reopenErr));
                failed = 1;
            } else {
                VLFileInfo rtInfo;
                vl_get_info(reopened, &rtInfo);
                printf("  roundtrip reopen: %d/%d slices, %d/%d frames, %.3f BPM, %d channel%s\n",
                       rtInfo.slice_count,
                       info.slice_count,
                       rtInfo.total_frames,
                       info.total_frames,
                       rtInfo.tempo / 1000.0,
                       rtInfo.channels,
                       rtInfo.channels == 1 ? "" : "s");
                if (rtInfo.slice_count != info.slice_count ||
                    rtInfo.total_frames != info.total_frames) {
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
