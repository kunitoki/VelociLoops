# Getting Started

VelociLoops is a static C library.  It has no external dependencies beyond the
C++ standard library and CMake 3.20+.

```{toctree}
:maxdepth: 2

api
format
```

---

## Building

```sh
# Configure (static library + demo executable)
cmake -B build .

# Build
cmake --build build

# Run the demo (extract slices from a .rx2 file)
./build/demo/velociloops_demo data/120Stereo.rx2 slices/
```

To open an Xcode project on macOS:

```sh
cmake -G Xcode -B build .
open build/VelociLoops.xcodeproj
```

### Linking against VelociLoops

Add the repository as a CMake subdirectory and link against the static library:

```cmake
add_subdirectory(VelociLoops)
target_link_libraries(my_app PRIVATE velociloops_library)
```

---

## Reading a REX2 file

```c
#include "velociloops.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    VLError err;
    VLFile  file = vl_open("loop.rx2", &err);
    if (!file) {
        fprintf(stderr, "open failed: %s\n", vl_error_string(err));
        return 1;
    }

    /* File-level metadata */
    VLFileInfo info;
    vl_get_info(file, &info);
    printf("channels=%d  rate=%d  tempo=%.3f BPM  slices=%d\n",
           info.channels, info.sample_rate,
           info.tempo / 1000.0, info.slice_count);

    /* Decode each slice to float buffers */
    for (int i = 0; i < info.slice_count; ++i) {
        int32_t n = vl_get_slice_frame_count(file, i);
        float  *L = malloc(n * sizeof(float));
        float  *R = malloc(n * sizeof(float));  /* NULL for mono-only */

        int32_t written;
        VLError e = vl_decode_slice(file, i, L, R, n, &written);
        if (e == VL_OK)
            printf("  slice %d: %d frames\n", i, written);

        /* use L / R here … */

        free(L);
        free(R);
    }

    vl_close(file);
    return 0;
}
```

---

## Creating a REX2 file

```c
#include "velociloops.h"

/* Assume `left` and `right` are float[frame_count] filled with audio data. */
void write_example(const float* left, const float* right, int32_t frame_count) {
    /* 1. Create handle: stereo, 44.1 kHz, 120 BPM */
    VLError err;
    VLFile  file = vl_create_new(2, 44100, 120000, &err);
    if (!file) return;

    /* 2. Optionally set creator metadata before adding slices */
    VLCreatorInfo ci = {0};
    snprintf(ci.name, sizeof(ci.name), "My Plugin");
    snprintf(ci.url,  sizeof(ci.url),  "https://example.com");
    vl_set_creator_info(file, &ci);

    /* 3. Add slices in ascending ppq_pos order */
    /*    ppq_pos=0 places the first slice at the very start of the loop */
    int32_t idx = vl_add_slice(file, 0, left, right, frame_count);
    if (idx < 0) {
        vl_close(file);
        return;
    }

    /* 4. Save */
    vl_save(file, "out.rx2");

    /* 5. Clean up */
    vl_close(file);
}
```

---

## Error handling

Every function that can fail returns a `VLError`.  Functions that return a
handle (`vl_open`, `vl_create_new`) signal failure by returning `NULL` and
optionally writing the error code to an `err` out-parameter.

```c
VLError err;
VLFile  f = vl_open("loop.rx2", &err);
if (!f) {
    /* vl_error_string() never returns NULL */
    fprintf(stderr, "error %d: %s\n", (int)err, vl_error_string(err));
    return;
}

VLFileInfo info;
VLError e = vl_get_info(f, &info);
if (e != VL_OK) {
    fprintf(stderr, "get_info: %s\n", vl_error_string(e));
}

vl_close(f);
```

### VLError quick reference

| Code | Value | Meaning |
|------|-------|---------|
| `VL_OK` | 0 | Success |
| `VL_ERROR_INVALID_HANDLE` | -1 | NULL or closed VLFile |
| `VL_ERROR_INVALID_ARG` | -2 | NULL pointer or out-of-range number |
| `VL_ERROR_FILE_NOT_FOUND` | -3 | Path missing or unreadable |
| `VL_ERROR_FILE_CORRUPT` | -4 | Malformed IFF/DWOP structure |
| `VL_ERROR_OUT_OF_MEMORY` | -5 | Heap allocation failed |
| `VL_ERROR_INVALID_SLICE` | -6 | Slice index out of range |
| `VL_ERROR_INVALID_SAMPLE_RATE` | -7 | Unsupported sample rate |
| `VL_ERROR_BUFFER_TOO_SMALL` | -8 | Buffer < `vl_get_slice_frame_count()` |
| `VL_ERROR_NO_CREATOR_INFO` | -9 | CREI chunk absent |
| `VL_ERROR_NOT_IMPLEMENTED` | -10 | Feature not yet functional |
| `VL_ERROR_ALREADY_HAS_DATA` | -11 | Metadata set after slices added |

---

## Loading from memory

For embedded contexts or network streams where the file is already in memory:

```c
const uint8_t *buf  = /* … */;
size_t         size = /* … */;

VLError err;
VLFile  file = vl_open_from_memory(buf, size, &err);
/* buf may be freed after this call */
```

---

## Serialising to memory

Use the two-call pattern to write a REX2 file into a heap buffer:

```c
size_t sz = 0;
vl_save_to_memory(file, NULL, &sz);   /* query required size */

uint8_t *buf = malloc(sz);
vl_save_to_memory(file, buf, &sz);    /* write */

/* transmit or store buf[0..sz-1] */
free(buf);
```
