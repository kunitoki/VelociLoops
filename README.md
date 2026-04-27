![VelociLoops](https://raw.githubusercontent.com/kunitoki/VelociLoops/main/backdrop.jpg?x=2)

# VelociLoops

[![Tests](https://github.com/kunitoki/VelociLoops/actions/workflows/tests.yml/badge.svg?x=2)](https://github.com/kunitoki/VelociLoops/actions/workflows/tests.yml)
[![Coverage Status](https://coveralls.io/repos/github/kunitoki/VelociLoops/badge.svg?x=2&branch=main)](https://coveralls.io/github/kunitoki/VelociLoops?branch=main)

A C library for reading and writing **REX2** (`.rx2`) audio loop files.

REX2 is Propellerhead's loop-slicing container.  VelociLoops provides a clean,
dependency-free C API that decodes and encodes the proprietary DWOP bitstream,
exposes per-slice float audio, and supports a full read/write round-trip.

---

## Features

- **DWOP decode / encode** — Lossless mono and stereo, 16-bit and 24-bit
- **Float slice extraction** — Normalized [-1.0, 1.0] output with transient-stretch rendering
- **Full round-trip** — Read an existing file, mutate slices, save back
- **Creator metadata** — Read and write CREI name / copyright / URL fields
- **Plain C API** — `extern "C"`, no exceptions, no RTTI; embeds cleanly in C and C++ hosts
- **No external dependencies** — C++17 standard library only

---

## Building

Requires **CMake 3.20+** and a C++17 compiler.

```sh
cmake -B build .
cmake --build build
```

This produces the static library `libvelociloops_static`, shared library `libvelociloops_shared` and the `velociloops` demo
executable.

### Run the demo

```sh
./build/demo/velociloops tests/data/120Stereo.rx2 out/
```

Extracts every slice as a WAV file and performs a save/reload round-trip check.

### Embed in your project

```cmake
add_subdirectory(VelociLoops)
target_link_libraries(my_target PRIVATE velociloops_static)
```

---

## Quick Start

```c
#include "velociloops.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    /* Open and decode */
    VLError err;
    VLFile  file = vl_open("loop.rx2", &err);
    if (!file) {
        fprintf(stderr, "open: %s\n", vl_error_string(err));
        return 1;
    }

    /* File metadata */
    VLFileInfo info;
    vl_get_info(file, &info);
    printf("%d ch  %d Hz  %.1f BPM  %d slices\n",
           info.channels, info.sample_rate,
           info.tempo / 1000.0, info.slice_count);

    /* Decode slice 0 */
    int32_t n = vl_get_slice_frame_count(file, 0);
    float  *L = malloc(n * sizeof(float));
    float  *R = malloc(n * sizeof(float));
    int32_t written;
    vl_decode_slice(file, 0, L, R, n, &written);

    /* … use L / R … */

    free(L); free(R);
    vl_close(file);
    return 0;
}
```

---

## Documentation

Hosted documentation: <https://velociloops.readthedocs.io/en/latest/>

---

## Project Status

The mono decode path is **frame-accurate** against the official Propellerhead REX
SDK for the included test files.  The stereo encode/decode round-trip is
functional.  Resampling (`vl_set_output_sample_rate`) is parsed but not yet
implemented.

---

## Waveform Comparison

ReCycle output for `120Stereo.rx2`:
![ReCycle](https://raw.githubusercontent.com/kunitoki/VelociLoops/main/sound_recycle.jpg?x=2)

VelociLoops output for `120Stereo.rx2`:
![VelociLoops](https://raw.githubusercontent.com/kunitoki/VelociLoops/main/sound_velociloops.jpg?x=2)

---

## License

See [LICENSE](LICENSE).
