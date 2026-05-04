![VelociLoops](https://raw.githubusercontent.com/kunitoki/VelociLoops/main/backdrop.jpg?cache-control=no-cache)

# VelociLoops

[![Tests](https://github.com/kunitoki/VelociLoops/actions/workflows/tests.yml/badge.svg?cache-control=no-cache)](https://github.com/kunitoki/VelociLoops/actions/workflows/tests.yml)
[![Sanitizers](https://github.com/kunitoki/VelociLoops/actions/workflows/sanitizers.yml/badge.svg?cache-control=no-cache)](https://github.com/kunitoki/VelociLoops/actions/workflows/sanitizers.yml)
[![Fuzzing](https://github.com/kunitoki/VelociLoops/actions/workflows/fuzz.yml/badge.svg?cache-control=no-cache)](https://github.com/kunitoki/VelociLoops/actions/workflows/fuzz.yml)
[![Documentation](https://app.readthedocs.org/projects/velociloops/badge/?version=latest)](https://velociloops.readthedocs.io/en/latest/)
[![Coverage Status](https://coveralls.io/repos/github/kunitoki/VelociLoops/badge.svg?cache-control=no-cache&branch=main)](https://coveralls.io/github/kunitoki/VelociLoops?branch=main)

A C library for reading and writing **REX2** (`.rx2`) audio loop files.

REX2 is Propellerhead's loop-slicing container.  VelociLoops provides a clean,
dependency-free C API that decodes and encodes the proprietary DWOP bitstream,
exposes per-slice float audio, and supports a full read/write round-trip.

---

## Supported Features

### File formats

- **REX2 / RX2 read and write** — Parses and emits IFF/CIFF `.rx2` containers.
- **Legacy ReCycle read support** — Opens AIFF-backed `.rex` and `.rcy` files as PCM loop containers.
- **Disk and memory I/O** — Load from paths or caller-owned memory, and save to paths or memory.

### Audio codec and formats

- **DWOP decode and encode** — Clean-room implementation of the proprietary DWOP bitstream.
- **Mono and stereo audio** — Interleaved internal PCM with mono mirroring when decoding to an optional right channel.
- **16-bit and 24-bit integer paths** — Decode, render, mutate, and re-encode 16-bit and 24-bit sources/authored files.
- **Native sample-rate rendering** — Supports source sample rates from 8 kHz to 192 kHz.

### Slice parsing and rendering

- **Public slice enumeration** — Exposes per-slice PPQ position, sample start, sample length, analysis points, and editing flags.
- **Official-style slice sequence pass** — Applies analysis sensitivity and gate-length behavior when deciding which marker-like SLCE records become renderable slices.
- **Transient-stretch rendering** — Extends slice output when TRSH stretch is active, using the detected loop tail and decay envelope.
- **Realtime slice reads** — Read slice info and audio data perform no heap allocation after opening or authoring a file.
- **Muted, locked, selected, marker, and synthetic flags** — Preserves public slice editing state and exposes derived marker/synthetic state.

### Metadata

- **File metadata** — Reads channel count, sample rate, bit depth, tempo, original tempo, PPQ length, time signature, loop bounds, total frames, processing gain, transient settings, and silence-selected state.
- **Creator metadata** — Reads and writes CREI name, copyright, URL, e-mail, and free-text fields.
- **Legacy tempo and loop metadata** — Recovers tempo, loop points, PPQ positions, and rendered source lengths from supported `.rex` / `.rcy` files.

### Authoring and mutation

- **Create new RX2 files** — Build mono or stereo loops from caller-supplied float slices.
- **SuperFlux auto-slicing** — Build a save-ready loop from full-loop mono or stereo float PCM using onset detection.
- **Set file and creator metadata before writing** — Configure tempo, original tempo, time signature, bit depth, transient settings, and creator fields before adding audio.
- **Append and remove slices** — Add slices by PPQ position and remove existing slices by index.
- **Round-trip by re-encoding** — Decode existing slices to float audio, mutate metadata/slices, and save a fresh RX2 file.

### API and integration

- **Plain C API** — `extern "C"`, opaque handles, explicit error codes, and no exceptions across the ABI.
- **Embeddable implementation** — C++17 standard library only; no runtime third-party dependencies.
- **Robust parser coverage** — Regression tests, sanitizer builds, fuzzing workflow, and coverage workflow are included.

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

```sh
./build/demo/velociloops tests/data/120Stereo.wav out/120Stereo_auto.rx2 120
```

Auto-slices a WAV file with SuperFlux onset detection and writes a REX2 file.

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

int main(void)
{
    /* Open and decode */
    VLError err;
    VLFile file = vl_open("loop.rx2", &err);
    if (!file)
    {
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
    float* L = malloc(n * sizeof(float));
    float* R = malloc(n * sizeof(float));
    int32_t written;
    vl_decode_slice(file, 0, L, R, 0, n, &written);

    /* … use L / R … */

    free(L);
    free(R);
    
    vl_close(file);
    return 0;
}
```

---

## Documentation

Hosted documentation: <https://velociloops.readthedocs.io/en/latest/>

---

## Waveform Comparison

ReCycle output for `120Stereo.rx2`:
![ReCycle](https://raw.githubusercontent.com/kunitoki/VelociLoops/main/sound_recycle.jpg?cache-control=no-cache)

VelociLoops output for `120Stereo.rx2`:
![VelociLoops](https://raw.githubusercontent.com/kunitoki/VelociLoops/main/sound_velociloops.jpg?cache-control=no-cache)

---

## License

See [LICENSE](LICENSE).
