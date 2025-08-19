#include "velociloops.h"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    VLError err = VL_OK;
    VLFile f = vl_open_from_memory(data, size, &err);
    if (!f)
        return 0;

    VLFileInfo info = {};
    vl_get_info(f, &info);

    // Cap per-slice frame count so the fuzzer does not OOM on pathological sizes.
    static constexpr int32_t kMaxFuzzFrames = 1 << 20; // 1M samples

    for (int i = 0; i < info.slice_count; ++i)
    {
        const int32_t frames = vl_get_slice_frame_count(f, i);
        if (frames <= 0 || frames > kMaxFuzzFrames)
            continue;

        std::vector<float> left(static_cast<size_t>(frames));
        std::vector<float> right(static_cast<size_t>(frames));
        int32_t actual = 0;
        vl_decode_slice(f, i, left.data(), right.data(), 0, frames, &actual);
    }

    vl_close(f);
    return 0;
}
