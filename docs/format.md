# REX2 File Format

This document describes the `.rx2` / REX2 format as implemented by
VelociLoops. It is based on the production parser, DWOP decoder, and DWOP
encoder in `src/velociloops.cpp`.

The important practical point is that REX2 stores one compressed waveform for
the whole loop. Slice chunks describe frame ranges in the decoded waveform; they
do not contain per-slice audio.

---

## Container Overview

REX2 is an IFF/CIFF-style container. The root chunk is a `CAT ` container whose
type tag is `REX2`. Chunk IDs are four ASCII bytes and chunk sizes are
big-endian.

Typical layout:

```text
CAT  REX2
|-- HEAD          file header / magic
|-- CREI          creator metadata (optional)
|-- GLOB          global loop settings
|-- RECY          ReCycle metadata
|-- CAT  DEVL     device settings
|   |-- TRSH      transient shaper
|   |-- EQ        equalizer settings
|   `-- COMP      compressor settings
|-- CAT  SLCL     slice list
|   `-- SLCE * N  slice boundaries in decoded sample frames
|-- SINF          sample format and loop region
`-- SDAT          DWOP-compressed audio payload
```

VelociLoops' reader recursively scans nested `CAT ` chunks, so the parser does
not depend on the exact ordering shown above. VelociLoops' writer emits the
ordering shown above.

Each non-container chunk uses standard IFF framing:

```text
Offset  Size  Field
0       4     Chunk ID, for example "GLOB"
4       4     Payload size in bytes, big-endian, excluding this 8-byte header
8       N     Payload
```

Container chunks add a 4-byte type tag at the beginning of their payload:

```text
Offset  Size  Field
0       4     "CAT " or "FORM"
4       4     Payload size, including the type tag
8       4     Type tag, for example "REX2" or "SLCL"
12      ...   Nested chunks
```

Chunks are padded to even byte offsets. The pad byte is not included in the
stored payload size.

All multi-byte integer fields below are big-endian unless stated otherwise.

---

## Chunk Reference

### `HEAD` - File Header

VelociLoops writes a fixed 29-byte `HEAD` payload:

```text
49 0c f1 8d bc 02 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00
```

The official SDK parses this as:

| Offset | Size | Description |
|--------|------|-------------|
| 0 | 4 | Magic value. Official SDK validates `0x490cf18d`. |
| 4 | variable | File format version record. VelociLoops writes the same fixed bytes as the test fixtures. |
| later | 16 | UUID. Official SDK rejects non-zero UUID bytes. |
| end | variable | Obsolete filename string. VelociLoops writes an empty string. |

VelociLoops validates the magic and accepts the fixture-compatible `bc 01`,
`bc 02`, and `bc 03` format version records. Newer unsupported header versions
are rejected as corrupt.

### `CREI` - Creator Info

Optional creator metadata. The current implementation reads and writes five
cross-platform strings, each stored as a 32-bit byte count followed by that many
bytes.

| Field | Type | Description |
|-------|------|-------------|
| name | `uint32 length` + bytes | Creator or artist name. |
| copyright | `uint32 length` + bytes | Copyright notice. |
| url | `uint32 length` + bytes | Creator website. |
| email | `uint32 length` + bytes | Contact email. |
| free_text | `uint32 length` + bytes | Free-form note. |

VelociLoops copies each string into a 256-byte C buffer, so the public API
exposes at most 255 bytes plus a trailing NUL per field. This is not a fixed
`5 * 256` byte payload on disk.

If this chunk is absent, `vl_get_creator_info()` returns
`VL_ERROR_NO_CREATOR_INFO`.

### `GLOB` - Global Loop Settings

`GLOB` is 22 bytes. In the official SDK this is the
`CREX2GeneralInfoChunk`. VelociLoops uses the fields that affect public
metadata and rendering.

| Offset | Size | Type | SDK field | Description |
|--------|------|------|-----------|-------------|
| 0 | 4 | `uint32` | slice count | Number of `SLCE` chunks expected. |
| 4 | 2 | `uint16` | bars | Bar count / layout field. VelociLoops writes `1`. |
| 6 | 1 | `uint8` | beats | Beat count / layout field. VelociLoops writes `0`. |
| 7 | 1 | `uint8` | time sig numerator | Public `time_sig_num`. |
| 8 | 1 | `uint8` | time sig denominator | Public `time_sig_den`. |
| 9 | 1 | `uint8` | sensitivity | Analysis sensitivity. VelociLoops writes `0x4e`. |
| 10 | 2 | `uint16` | gate sensitivity | Gate sensitivity. VelociLoops writes `0`. |
| 12 | 2 | `uint16` | processing gain | Slice render gain. |
| 14 | 2 | `uint16` | pitch | Pitch field. VelociLoops writes `1`. |
| 16 | 4 | `uint32` | preview tempo | Tempo in BPM * 1000. |
| 20 | 1 | bool byte | transmit-as-slices | VelociLoops writes `1`. |
| 21 | 1 | bool byte | silence selected | Public `silence_selected`. |

The render gain used by `vl_decode_slice()` is:

```text
linear_gain = processing_gain * 0.000833333354
```

In practice, `1200` is approximately unity in the official SDK model. New files
created by VelociLoops clamp and write `1000` by default.

### `RECY` - ReCycle Metadata

The official SDK parses this as a ReCycle version, preview-play flag,
horizontal ruler unit, exported size, and exported-slice count. VelociLoops only
uses it to recover `original_tempo` from bytes 8..11 when the value is positive.

VelociLoops writes a 15-byte payload:

| Offset | Size | Description |
|--------|------|-------------|
| 0 | 8 | Version / flags bytes written as `bc 02 00 00 00 01 00 00`. |
| 8 | 4 | Original tempo in BPM * 1000. |
| 12 | 2 | Reserved, written as `0`. |
| 14 | 1 | Reserved/unit byte, written as `8`. |

When reading older or odd files, VelociLoops can also derive the original tempo
from the loop frame count, sample rate, and PPQ length if the chunk value is not
useful.

### `TRSH` - Transient Shaper

`TRSH` lives inside `CAT DEVL` and is 7 bytes.

| Offset | Size | Type | Description |
|--------|------|------|-------------|
| 0 | 1 | bool byte | Enabled flag. |
| 1 | 2 | `uint16` | Attack, valid range `0..1023`. |
| 3 | 2 | `uint16` | Decay, valid range `0..1023`. |
| 5 | 2 | `uint16` | Stretch, valid range `0..100`. |

VelociLoops uses `stretch` to extend rendered slice length. The rendered tail is
not extra compressed audio; it is synthesized during `vl_decode_slice()` by
looping a segment around the slice end with a decaying envelope.

### `EQ  ` and `COMP`

These chunks also live inside `CAT DEVL`. VelociLoops writes default payloads
for compatibility but does not currently expose their values through the public
API.

`EQ  ` is 17 bytes in files written by VelociLoops:

```text
00 00 0f 00 64 00 00 03 e8 09 c4 00 00 03 e8 4e 20
```

`COMP` is 9 bytes in files written by VelociLoops:

```text
00 00 4d 00 27 00 42 00 38
```

### `SLCE` - Slice Entry

`SLCE` chunks live inside `CAT SLCL`. Each payload is 11 bytes in normal REX2
files and files written by VelociLoops.

| Offset | Size | Type | Description |
|--------|------|------|-------------|
| 0 | 4 | `uint32` | Slice start frame in the decoded waveform. |
| 4 | 4 | `uint32` | Slice length in PCM frames at native sample rate. |
| 8 | 2 | `uint16` | Analyze points. VelociLoops writes `0x7fff`. |
| 10 | 1 | flags | Bit flags described below. |

Flags:

| Bit | Meaning |
|-----|---------|
| 0 | Muted slice. |
| 1 | Locked slice. |
| 2 | Selected slice. |
| 3..7 | Reserved; official SDK rejects these. |

Official files can contain marker-like slices with `sample_length <= 1`.
VelociLoops filters those out after parsing.

The public `VLSliceInfo::ppq_pos` is not stored directly in `SLCE`. It is
derived after all slices are sorted:

```text
denom = (loop_end > loop_start) ? (loop_end - loop_start) : total_frames
relative_start = max(sample_start - loop_start, 0)
ppq_pos = round(relative_start * ppq_length / denom)
```

### `SINF` - Sample Info

`SINF` is the key audio-format descriptor. It is 18 bytes.

| Offset | Size | Type | Description |
|--------|------|------|-------------|
| 0 | 1 | `uint8` | Channel count. Official and VelociLoops support `1` or `2`. |
| 1 | 1 | format code | Bit-depth / sample-format code. |
| 2 | 4 | `uint32` | Native sample rate in Hz. |
| 6 | 4 | `uint32` | Total decoded PCM frame count. |
| 10 | 4 | `uint32` | Loop start frame, inclusive. |
| 14 | 4 | `uint32` | Loop end frame, exclusive. |

Format codes:

| Code | Official meaning | VelociLoops behavior |
|------|------------------|----------------------|
| `1` | 8-bit | Recognized as metadata. |
| `3` | 16-bit integer | Primary supported path. |
| `5` | 24-bit integer | Recognized as metadata; not emitted by VelociLoops. |
| `7` | 32-bit float | Recognized as metadata. |

VelociLoops stores decoded audio internally as interleaved signed 16-bit PCM and
always writes new files as 16-bit (`SINF` code `3`). The official SDK has both
16-bit and 24-bit DWOP decode destinations; VelociLoops' current public
round-trip path is intentionally the 16-bit path.

The loop region determines how slice positions map between sample frames and
PPQ time. If `loop_end <= loop_start`, VelociLoops treats the whole decoded
waveform as the loop.

### `SDAT` - DWOP Compressed Audio

`SDAT` contains the compressed waveform for the entire file. The DWOP bitstream
starts at byte 0 of the `SDAT` payload, immediately after the 8-byte IFF chunk
header. There is no extra DWOP prologue in the verified REX2 files.

The reader also accepts a chunk named `DWOP` as a payload alias, but VelociLoops
writes `SDAT`.

The decoder stops after the `SINF.total_frames` count has been produced. Any
final padding bits in the last 32-bit word are ignored.

---

## Time and PPQ

REX2 positions slices musically in PPQ ticks. VelociLoops uses:

| Constant | Value | Meaning |
|----------|-------|---------|
| `kREXPPQ` | `15360` | Ticks per 4/4 bar. |
| quarter note | `3840` | `15360 / 4`. |
| sixteenth note | `960` | `3840 / 4`. |

For files written by VelociLoops, `ppq_length` defaults to four bars:

```text
ppq_length = 15360 * 4 = 61440
```

Frame-to-PPQ conversion uses the active loop region:

```text
frames_in_loop = loop_end - loop_start
ppq_pos = round((sample_start - loop_start) * ppq_length / frames_in_loop)
```

When creating a new file and adding a slice by PPQ, VelociLoops maps PPQ back to
sample frames with the same loop-region ratio when loop points are known. If no
loop region exists yet, it falls back to tempo and sample rate:

```text
sample_start = round(ppq * sample_rate * 60000 /
                     (tempo_bpm_x1000 * 15360))
```

---

## Audio Format

### Storage Model

The physical storage model is:

```text
SINF: channel count, sample format code, sample rate, frame count, loop points
SDAT: one DWOP bitstream for all frames
SLCE: decoded-frame ranges into that full waveform
```

For stereo, PCM frames are interleaved after decode:

```text
frame 0: left[0], right[0]
frame 1: left[1], right[1]
...
```

Slice decoding does not re-run DWOP per slice. `vl_open()` decodes all `SDAT`
frames once into the internal PCM buffer. `vl_decode_slice()` then copies and
renders a slice range from that PCM buffer, applying processing gain and the
transient-stretch tail.

### Slice Rendering

`SLCE.sample_start` and `SLCE.sample_length` describe the source segment in the
decoded PCM buffer. `vl_decode_slice()` renders from that segment with these
implementation details:

- If the first stored `SLCE` starts after a valid `SINF.loop_start`, VelociLoops
  synthesizes a leading slice whose metadata spans `loop_start` to that first
  stored slice. This matches the official SDK's reported slice list for files
  such as `120Stereo.rx2`.
- The sample read position starts two frames after the stored `SLCE` start,
  matching observed `REXRenderSlice` behavior.
- Samples are converted to float by dividing signed 16-bit PCM by `32768.0`.
- `GLOB.processing_gain` is applied as described above.
- If transient stretch is enabled, the rendered length can exceed
  `sample_length`. The extra frames are produced by alternating forward and
  backward playback through a detected loop region near the slice end.
- The stretch envelope decays linearly to silence over the tail.

This rendering step is separate from the DWOP audio format. The `SDAT` payload
contains only the original decoded waveform, not the stretched render tails.

---

## DWOP Codec

DWOP is Propellerhead's proprietary adaptive predictive entropy codec used for
the `SDAT` payload. The implementation in VelociLoops is a clean-room port of
the official SDK behavior.

### Bitstream

DWOP data is consumed as big-endian 32-bit words, MSB first.

The official SDK underflow path reads bytes from the `SDAT` payload and converts
each 4-byte group into a host word equivalent to:

```cpp
word = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
```

`readBit()` behavior:

```text
old_bits_left = bits_left
bits_left -= 1
if old_bits_left - 1 < 0:
    current_word = next 32-bit word
    bits_left = 31
bit = MSB(current_word)
current_word <<= 1
```

Multi-bit reads take the top `n` bits from `current_word`, shift left by `n`,
and pull from the next word only if the read crosses a word boundary.

### Per-Channel State

Each coded channel has:

| State | Count | Initial value | Purpose |
|-------|-------|---------------|---------|
| `deltas` | 5 signed ints | `0` | Predictor history / integrated residual state. |
| `averages` | 5 unsigned ints | `2560` | Running magnitude estimates used to choose predictor order. |
| `j` | 1 unsigned int | `2` | Adaptive Rice/Golomb scale. |
| `rbits` | 1 signed int | `0` | Number of remainder bits read after the prefix. |

The decoder multiplies stored deltas by two at the start of a decode call and
right-shifts by one when writing 16-bit output. This is why the implementation
uses names such as `sample2x` and `signed2x`.

### Decoding One Coded Channel Sample

For each coded channel sample:

1. Select the predictor index `idx` whose `averages[idx]` is smallest.
2. Compute the base prefix step:

   ```text
   base_step = ((min_average * 3) + 36) >> 7
   ```

3. Read zero bits until a one bit is encountered. For each zero:

   ```text
   prefix_sum += step
   zeros_window -= 1
   if zeros_window == 0:
       step <<= 2
       zeros_window = 7
   ```

4. Adjust `j` and `rbits` to the final `step`:

   ```text
   if step < j:
       for jt = j >> 1; step < jt; jt >>= 1:
           j = jt
           rbits -= 1
   else:
       while step >= j:
           j <<= 1
           rbits += 1
   ```

5. Read `rbits` remainder bits. If `rbits == 0`, the remainder is zero.
6. If the remainder is in the extended range, read one extra bit:

   ```text
   threshold = j - step
   if remainder >= threshold:
       extra = read 1 bit
       remainder = remainder * 2 - threshold + extra
   ```

7. Combine prefix and remainder:

   ```text
   code = prefix_sum + remainder
   residual = -(code & 1) ^ code
   ```

   This maps even codes to non-negative residuals and odd codes to negative
   residuals in the doubled sample domain.

8. Apply the predictor selected by `idx`.
9. Output `predicted_sample2x >> 1` for 16-bit decode.
10. Update all five averages:

    ```text
    average[i] = average[i] + (delta[i] ^ (delta[i] >> 31)) - (average[i] >> 5)
    ```

    This is one's-complement magnitude, not `abs(delta)`. For example, `-1`
    contributes `0`, and `-2` contributes `1`.

### Predictor Cases

The five predictors are cumulative integration orders. In pseudocode:

```cpp
case 0:
    t0 = residual - d0;
    t1 = t0 - d1;
    t2 = t1 - d2;
    d4 = t2 - d3; d3 = t2; d2 = t1; d1 = t0; d0 = residual;
    sample = residual;

case 1:
    t1 = residual - d1;
    t2 = t1 - d2;
    sample = d0 + residual;
    d4 = t2 - d3; d3 = t2; d2 = t1; d1 = residual; d0 = sample;

case 2:
    nd1 = d1 + residual;
    sample = d0 + nd1;
    t = residual - d2;
    d4 = t - d3; d3 = t; d2 = residual; d1 = nd1; d0 = sample;

case 3:
    nd2 = d2 + residual;
    nd1 = d1 + nd2;
    sample = d0 + nd1;
    d4 = residual - d3; d3 = residual; d2 = nd2; d1 = nd1; d0 = sample;

case 4:
    nd3 = d3 + residual;
    nd2 = d2 + nd3;
    nd1 = d1 + nd2;
    sample = d0 + nd1;
    d4 = residual; d3 = nd3; d2 = nd2; d1 = nd1; d0 = sample;
```

### Mono Layout

Mono is one coded channel. The decoder writes one signed 16-bit PCM sample per
frame:

```text
out[frame] = clamp(sample2x >> 1)
```

### Stereo Layout

Stereo uses two coded channel states in one shared bitstream. The first coded
channel is the left sample. The second coded channel is a side/difference value:

```text
coded0 = left2x
coded1 = right2x - left2x
```

The decoder consumes `coded0`, then `coded1`, for every frame:

```text
left  = coded0 >> 1
right = (coded0 + coded1) >> 1
```

The two coded channels maintain independent `deltas`, `averages`, `j`, and
`rbits`, but they share the same `current_word` and `bits_left` bit-reader
state.

### Decoder Chunking

VelociLoops decodes in blocks up to `0x100000` frames, matching the official
SDK's block-oriented path. The DWOP predictor state is preserved across blocks.
It is not reset at slice boundaries.

---

## DWOP Encoding

VelociLoops includes an encoder that writes bitstreams accepted by the
VelociLoops decoder. It mirrors the decoder state machine:

- Input floats are first clamped to `[-1.0, 1.0]` and converted to signed
  16-bit PCM.
- Mono encodes one coded channel: `sample2x = pcm * 2`.
- Stereo encodes two coded channels per frame: `left2x`, then
  `right2x - left2x`.
- The predictor index is selected by the same minimum-average rule.
- The encoder computes the residual that will reconstruct the desired sample
  through the selected predictor.
- Residuals are mapped to code values with:

  ```text
  code = residual >= 0 ? residual : -residual - 1
  ```

- The encoder searches for a valid zero-prefix length and remainder encoding
  using the same `step`, `j`, `rbits`, and extended-remainder rules as the
  decoder.
- Bits are written MSB first into big-endian 32-bit words.
- The last word is zero-padded if the final code does not end on a 32-bit
  boundary.

The writer then emits the complete REX2 container with `SDAT` holding the DWOP
payload.

### Writer Subset

Files written by VelociLoops are deliberately conservative:

- Root container is `CAT REX2`.
- `HEAD` is the fixed 29-byte compatibility payload.
- `CREI` is written only when at least one creator field is non-empty.
- `GLOB`, `RECY`, `DEVL/TRSH/EQ/COMP`, `SLCL/SLCE`, `SINF`, and `SDAT` are
  written.
- `SINF` is always emitted as 16-bit integer (`format code 3`).
- `processing_gain`, transient fields, sample rate, tempo, time signature, loop
  points, and slice starts/lengths come from `VLFileInfo` and added slices after
  normalization.

An unmodified file opened from disk and saved through `vl_save_to_memory()` is
returned byte-for-byte from the original buffer unless the handle was dirtied.

---

## Validation Notes

For `120Mono.rx2`, the verified first loaded DWOP words are:

```text
0x82082082 0x08208208 0x20820820 0x82082082
```

Those words begin at the first byte of the `SDAT` payload. The official trace
for `120Mono.rx2` decodes `117760` frames, and decoded frame `322`
(`loopStart`) begins:

```text
-231 -421 -410 -209 205 564 709 585 161 -349
```

These details are useful regression anchors when changing the bit reader,
prefix-step logic, average update, or predictor code.
