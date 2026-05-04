# API Reference

This document is the complete reference for the VelociLoops C API defined in
`include/velociloops.h`.


## Types

### `VLFile`

```c
typedef struct VLFile_s* VLFile;
```

Opaque handle representing one open REX2 file.  Obtained from `vl_open()`,
`vl_open_from_memory()`, or `vl_create_new()`.  Must be released with
`vl_close()` when no longer needed.  All functions that accept a `VLFile`
treat `NULL` as `VL_ERROR_INVALID_HANDLE`.


### `VLError`

```c
typedef enum { VL_OK = 0 } VLError;
```

Return type for all fallible API calls.  Zero (`VL_OK`) is success; every
negative value is a distinct failure reason.

| Enumerator | Value | Description |
|------------|-------|-------------|
| `VL_OK` | 0 | Success. |
| `VL_ERROR_INVALID_HANDLE` | -1 | NULL or already-closed `VLFile`. |
| `VL_ERROR_INVALID_ARG` | -2 | NULL required pointer or out-of-range number. |
| `VL_ERROR_FILE_NOT_FOUND` | -3 | Path does not exist or cannot be opened. |
| `VL_ERROR_FILE_CORRUPT` | -4 | IFF/DWOP structure malformed or mandatory chunks missing. |
| `VL_ERROR_OUT_OF_MEMORY` | -5 | Heap allocation failed. |
| `VL_ERROR_INVALID_SLICE` | -6 | Slice index negative or >= `slice_count`. |
| `VL_ERROR_INVALID_SAMPLE_RATE` | -7 | Sample rate zero, negative, or unsupported. |
| `VL_ERROR_BUFFER_TOO_SMALL` | -8 | `capacity` < `vl_get_slice_frame_count()`. |
| `VL_ERROR_NO_CREATOR_INFO` | -9 | File has no CREI chunk. |
| `VL_ERROR_NOT_IMPLEMENTED` | -10 | Feature parsed but not yet functional (e.g. resampling). |
| `VL_ERROR_ALREADY_HAS_DATA` | -11 | Metadata set attempted after slices were added. |
| `VL_ERROR_FILE_TOO_NEW` | -12 | File was written by a newer unsupported ReCycle/REX version. |
| `VL_ERROR_ZERO_LOOP_LENGTH` | -13 | File declares no usable loop length. |
| `VL_ERROR_INVALID_SIZE` | -14 | File or chunk size fields are invalid or inconsistent with the payload. |
| `VL_ERROR_INVALID_TEMPO` | -15 | Tempo field is zero or outside the supported ReCycle range. |


### `VLFileInfo`

```c
typedef struct { /* fields omitted */ } VLFileInfo;
```

File-level metadata.  Populated by `vl_get_info()`; supplied to `vl_set_info()`
when assembling a new file.

| Field | Type | Description |
|-------|------|-------------|
| `channels` | `int32_t` | 1 = mono, 2 = stereo. |
| `sample_rate` | `int32_t` | Native sample rate in Hz. |
| `slice_count` | `int32_t` | Number of slices. |
| `tempo` | `int32_t` | Playback tempo in BPM × 1000 (120 BPM → 120000). |
| `original_tempo` | `int32_t` | Original recording tempo in BPM × 1000 (RECY chunk). |
| `ppq_length` | `int32_t` | Loop length in PPQ ticks (kREXPPQ = 15360 ticks/bar). |
| `time_sig_num` | `int32_t` | Time-signature numerator. |
| `time_sig_den` | `int32_t` | Time-signature denominator. |
| `bit_depth` | `int32_t` | Source/authored bit depth: 16 or 24. |
| `total_frames` | `int32_t` | Total PCM frames in the decoded DWOP payload. |
| `loop_start` | `int32_t` | First PCM frame of the loop region (inclusive). |
| `loop_end` | `int32_t` | Last PCM frame of the loop region (exclusive). Zero = whole file. |
| `processing_gain` | `int32_t` | Raw GLOB gain (level = gain × 0.000833333354f; 1200 ≈ unity). |
| `transient_enabled` | `int32_t` | Non-zero if the transient shaper is active. |
| `transient_attack` | `int32_t` | Transient shaper attack (raw TRSH value). |
| `transient_decay` | `int32_t` | Transient shaper decay (raw TRSH value). |
| `transient_stretch` | `int32_t` | Stretch tail length (raw TRSH value). Non-zero extends `vl_get_slice_frame_count()`. |
| `silence_selected` | `int32_t` | Non-zero when the GLOB "silence selected" flag is set. |


### `VLSliceInfo`

```c
typedef struct { /* fields omitted */ } VLSliceInfo;
```

Per-slice metadata.  Populated by `vl_get_slice_info()`.

| Field | Type | Description |
|-------|------|-------------|
| `ppq_pos` | `int32_t` | Slice position in PPQ ticks from loop start. |
| `sample_length` | `int32_t` | Raw PCM frame count (no stretch tail). |
| `sample_start` | `int32_t` | Frame offset into the decoded PCM buffer. |
| `analysis_points` | `int32_t` | Raw slice-analysis value from the file. |
| `flags` | `int32_t` | Bitmask of `VL_SLICE_FLAG_*` values. |

Slice flags:

| Flag | Meaning |
|------|---------|
| `VL_SLICE_FLAG_MUTED` | Muted slice state. |
| `VL_SLICE_FLAG_LOCKED` | Locked slice state. |
| `VL_SLICE_FLAG_SELECTED` | Selected slice state. |
| `VL_SLICE_FLAG_MARKER` | Marker-like raw record that was not promoted to a normal renderable boundary. |
| `VL_SLICE_FLAG_SYNTHETIC` | Synthetic leading slice inserted by VelociLoops. |


### `VLCreatorInfo`

```c
typedef struct { /* fields omitted */ } VLCreatorInfo;
```

Optional creator and tag metadata from the REX2 CREI chunk.

| Field | Size | Description |
|-------|------|-------------|
| `name` | 256 bytes | Creator or artist name (NUL-terminated UTF-8). |
| `copyright` | 256 bytes | Copyright notice. |
| `url` | 256 bytes | Creator website URL. |
| `email` | 256 bytes | Contact e-mail address. |
| `free_text` | 256 bytes | Arbitrary free-form text. |

Absent fields are returned as empty strings (`""`).


### `VLSuperFluxOptions`

```c
typedef struct { /* fields omitted */ } VLSuperFluxOptions;
```

Tunable parameters for `vl_create_from_superflux()`. Initialize with
`vl_superflux_default_options()` and then override individual fields as needed.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `frame_size` | `int32_t` | 2048 | FFT/window size. Must be a power of two. |
| `fps` | `int32_t` | 200 | Onset detection frames per second. |
| `filter_bands` | `int32_t` | 24 | Log-frequency filter bands per octave. |
| `max_bins` | `int32_t` | 3 | Frequency bins for SuperFlux maximum filtering. |
| `diff_frames` | `int32_t` | 0 | Previous-frame distance; `<= 0` derives it from `ratio`. |
| `min_slice_frames` | `int32_t` | 0 | Minimum samples between slice starts; `<= 0` uses 10 ms. |
| `filter_equal` | `int32_t` | 0 | Non-zero normalizes triangular filter areas. |
| `online` | `int32_t` | 0 | Non-zero uses causal framing and peak picking. |
| `threshold` | `float` | 1.1 | Peak-picking threshold above the local average. |
| `combine_ms` | `float` | 30 | Suppress detections within this many milliseconds. |
| `pre_avg` | `float` | 0.15 | Seconds before peak for moving average. |
| `pre_max` | `float` | 0.01 | Seconds before peak for moving maximum. |
| `post_avg` | `float` | 0 | Seconds after peak for moving average. |
| `post_max` | `float` | 0.05 | Seconds after peak for moving maximum. |
| `delay_ms` | `float` | 0 | Detection timestamp offset in milliseconds. |
| `ratio` | `float` | 0.5 | Window ratio used to derive `diff_frames`. |
| `fmin` | `float` | 30 | Filterbank minimum frequency in Hz. |
| `fmax` | `float` | 17000 | Filterbank maximum frequency in Hz. |
| `log_mul` | `float` | 1 | Magnitude multiplier before `log10`. |
| `log_add` | `float` | 1 | Positive value added before `log10`. |


## Functions

### Open / Close


#### `vl_open`

```c
VLFile vl_open(const char* path, VLError* err);
```

Load and decode a REX2 file from disk.

Parses the IFF/CIFF container, validates mandatory chunks, and decompresses
the DWOP bitstream into an internal PCM buffer.  Complexity is O(n) in the
total frame count.

**Parameters**

| Name | Description |
|------|-------------|
| `path` | NUL-terminated path to the `.rx2` file. |
| `err` | Out-parameter; receives `VL_OK` on success or a negative error code. May be NULL. |

**Returns** A valid `VLFile` on success, or `NULL` on failure.

**Errors** `VL_ERROR_FILE_NOT_FOUND`, `VL_ERROR_FILE_CORRUPT`, `VL_ERROR_OUT_OF_MEMORY`.


#### `vl_open_from_memory`

```c
VLFile vl_open_from_memory(const void* data, size_t size, VLError* err);
```

Load and decode a REX2 file from a caller-owned memory buffer.

Identical to `vl_open()` except it reads from `data`.  The library copies
whatever internal state it needs; the caller may free `data` immediately after
this call returns.

**Parameters**

| Name | Description |
|------|-------------|
| `data` | Pointer to the raw `.rx2` bytes. |
| `size` | Byte length of `data`. |
| `err` | Out-parameter for the status code. May be NULL. |

**Returns** A valid `VLFile` on success, or `NULL` on failure.


#### `vl_create_new`

```c
VLFile vl_create_new(int32_t channels, int32_t sample_rate,
                      int32_t tempo, VLError* err);
```

Create a new, empty file handle for assembling a REX2 loop.

Metadata (via `vl_set_info()`, `vl_set_creator_info()`) must be set before the
first `vl_add_slice()` call; attempts to change metadata afterwards return
`VL_ERROR_ALREADY_HAS_DATA`.

**Parameters**

| Name | Description |
|------|-------------|
| `channels` | 1 (mono) or 2 (stereo). |
| `sample_rate` | Output sample rate in Hz. |
| `tempo` | Playback tempo in BPM × 1000. |
| `err` | Out-parameter for the status code. May be NULL. |

**Returns** A valid `VLFile` on success, or `NULL` on failure.


#### `vl_superflux_default_options`

```c
void vl_superflux_default_options(VLSuperFluxOptions* out);
```

Fill a caller-allocated `VLSuperFluxOptions` struct with the defaults used by
`vl_create_from_superflux()`. Passing `NULL` is allowed and does nothing.


#### `vl_create_from_superflux`

```c
VLFile vl_create_from_superflux(int32_t channels,
                                int32_t sample_rate,
                                int32_t tempo,
                                const float* left,
                                const float* right,
                                int32_t frames,
                                const VLSuperFluxOptions* options,
                                VLError* err);
```

Create a new authoring handle from a complete mono or stereo loop. Stereo input
is downmixed for SuperFlux onset detection, while the original left/right
buffers are copied into the resulting slices. The returned handle can be saved
with `vl_save()` or `vl_save_to_memory()`.

**Parameters**

| Name | Description |
|------|-------------|
| `channels` | 1 (mono) or 2 (stereo). |
| `sample_rate` | Input/output sample rate in Hz. |
| `tempo` | Playback tempo in BPM × 1000. |
| `left` | Left or mono buffer containing `frames` float samples. |
| `right` | Right buffer for stereo. Must be non-NULL when `channels == 2`. |
| `frames` | Number of PCM frames in each input buffer. |
| `options` | SuperFlux options, or NULL for defaults. |
| `err` | Out-parameter for the status code. May be NULL. |

**Returns** A valid `VLFile` on success, or `NULL` on failure.


#### `vl_close`

```c
void vl_close(VLFile file);
```

Release all resources associated with a `VLFile` handle.

Safe to call with `NULL` (no-op).  The handle is invalid after this call.


### Read: Metadata


#### `vl_get_info`

```c
VLError vl_get_info(VLFile file, VLFileInfo* out);
```

Retrieve file-level metadata.

**Parameters**

| Name | Description |
|------|-------------|
| `file` | Open `VLFile` handle. |
| `out` | Caller-allocated `VLFileInfo`; filled on `VL_OK`. |

**Returns** `VL_OK`, `VL_ERROR_INVALID_HANDLE`, or `VL_ERROR_INVALID_ARG`.


#### `vl_get_creator_info`

```c
VLError vl_get_creator_info(VLFile file, VLCreatorInfo* out);
```

Retrieve creator/tag metadata.

**Returns** `VL_OK` on success.  `VL_ERROR_NO_CREATOR_INFO` if the file has no
CREI chunk.


### Read: Slice Enumeration


#### `vl_get_slice_info`

```c
VLError vl_get_slice_info(VLFile file, int32_t index, VLSliceInfo* out);
```

Retrieve per-slice metadata by zero-based index.

After the file has been opened or authored, this function performs no heap
allocation and writes only to the caller-supplied `out` struct.

**Returns** `VL_OK` on success.  `VL_ERROR_INVALID_SLICE` if `index` is out of
range.


#### `vl_set_slice_info`

```c
VLError vl_set_slice_info(VLFile file, int32_t index,
                          int32_t flags, int32_t analysis_points);
```

Update mutable slice editing state. `VL_SLICE_FLAG_MUTED`,
`VL_SLICE_FLAG_LOCKED`, and `VL_SLICE_FLAG_SELECTED` are serialized to `SLCE`;
marker and synthetic bits are derived by the reader. Pass a negative
`analysis_points` value to keep the current value.


### Read: Sample Extraction

The realtime-safe read methods are:
- `vl_get_slice_info`
- `vl_get_slice_frame_count`
- `vl_decode_slice`

After a `VLFile` has been opened or authored, these methods perform no heap
allocation. They are suitable for audio-thread use when the caller keeps output
buffers preallocated, avoids closing or mutating the same `VLFile` concurrently,
and performs file loading, slice creation, saving, and buffer allocation outside
the audio callback.


#### `vl_set_output_sample_rate`

```c
VLError vl_set_output_sample_rate(VLFile file, int32_t rate);
```

Set the output sample rate for `vl_decode_slice()`.

Resampling is not yet implemented.  Returns `VL_ERROR_NOT_IMPLEMENTED` if
`rate` differs from the file's native rate.

**Returns** `VL_OK` when `rate` matches the native rate.


#### `vl_get_slice_frame_count`

```c
int32_t vl_get_slice_frame_count(VLFile file, int32_t index);
```

Return the number of frames `vl_decode_slice()` will write for a given slice.

Includes any transient-stretch tail frames.  Use this to pre-allocate the
`left`/`right` buffers.  After the file has been opened or authored, this
function performs no heap allocation and only reads immutable slice metadata.

**Returns** Frame count (>= 1) on success, or a negative `VLError` cast to
`int32_t` on failure.


#### `vl_decode_slice`

```c
VLError vl_decode_slice(VLFile file, int32_t index,
                         float* left, float* right,
                         int32_t frame_offset,
                         int32_t capacity, int32_t* frames_out);
```

Decode one slice into caller-supplied float PCM buffers (range [-1.0, 1.0]).
After the file has been opened or authored, this function performs no heap
allocation, does not resize internal storage, and writes only to
caller-supplied buffers.

The rendering pipeline applies:
- Processing gain from the GLOB chunk
- Transient-stretch tail (reverse-playback loop with linear amplitude decay)
- A two-frame read offset matching `REXRenderSlice` behaviour

For mono files the left channel is mirrored to `right` when `right != NULL`.

**Parameters**

| Name | Description |
|------|-------------|
| `file` | Open `VLFile` handle. |
| `index` | Zero-based slice index. |
| `left` | Output buffer for the left (or mono) channel. Must hold at least `capacity` floats. |
| `right` | Output buffer for the right channel. May be NULL. |
| `frame_offset` | Starting frame inside the rendered slice. Use 0 to decode from the beginning. |
| `capacity` | Buffer size in frames. Must be >= `vl_get_slice_frame_count(file, index) - frame_offset`. |
| `frames_out` | If non-NULL, receives the actual frame count written on `VL_OK`. |

**Returns** `VL_OK` on success.
`VL_ERROR_BUFFER_TOO_SMALL` if `capacity` is insufficient.
`VL_ERROR_INVALID_SLICE` for an out-of-range index.
`VL_ERROR_INVALID_ARG` if `left` is NULL or `frame_offset` is outside the
rendered slice.


### Write: Assembly


#### `vl_set_info`

```c
VLError vl_set_info(VLFile file, const VLFileInfo* info);
```

Overwrite file-level metadata on a handle from `vl_create_new()`.

Must be called before the first `vl_add_slice()`.  The library ignores
`slice_count`, `total_frames`, `loop_start`, and `loop_end` (computed from
slices).

**Returns** `VL_OK`, `VL_ERROR_INVALID_HANDLE`, `VL_ERROR_INVALID_ARG`, or
`VL_ERROR_ALREADY_HAS_DATA`.


#### `vl_set_creator_info`

```c
VLError vl_set_creator_info(VLFile file, const VLCreatorInfo* info);
```

Set creator/tag metadata.  Must be called before `vl_add_slice()`.

**Returns** Same as `vl_set_info()`.


#### `vl_add_slice`

```c
int32_t vl_add_slice(VLFile file, int32_t ppq_pos,
                      const float* left, const float* right,
                      int32_t frames);
```

Append a slice from caller-supplied float audio.

Slices must be added in ascending `ppq_pos` order.  DWOP compression is
deferred until `vl_save()` or `vl_save_to_memory()`.

**Parameters**

| Name | Description |
|------|-------------|
| `file` | Handle from `vl_create_new()`. |
| `ppq_pos` | Slice position in PPQ ticks. |
| `left` | Left (or mono) channel samples in [-1.0, 1.0]. |
| `right` | Right channel samples. May be NULL for mono files. |
| `frames` | Number of frames in each buffer. |

**Returns** Assigned slice index (>= 0) on success, or a negative `VLError`.


#### `vl_remove_slice`

```c
VLError vl_remove_slice(VLFile file, int32_t index);
```

Remove a slice by index.  Subsequent indices are shifted down by one.
Invalidates any previously cached rendered lengths.

**Returns** `VL_OK`, `VL_ERROR_INVALID_HANDLE`, or `VL_ERROR_INVALID_SLICE`.


#### `vl_save`

```c
VLError vl_save(VLFile file, const char* path);
```

Encode all slices with DWOP and write the IFF/CIFF container to disk.

The handle remains open and valid after this call.

**Returns** `VL_OK` on success, or a negative `VLError` on failure.


#### `vl_save_to_memory`

```c
VLError vl_save_to_memory(VLFile file, void* buf, size_t* size_out);
```

Encode and serialise the file to a caller-owned buffer.

Two-call pattern:

```c
size_t sz = 0;
vl_save_to_memory(file, NULL, &sz);   /* query */
uint8_t *buf = malloc(sz);
vl_save_to_memory(file, buf, &sz);    /* write */
```

When `buf` is NULL, writes the required byte count to `*size_out` and returns
`VL_OK`.  When `buf` is non-NULL, writes up to `*size_out` bytes and updates
`*size_out` with the actual count.

**Returns** `VL_OK` on success, or a negative `VLError` on failure.


### Utility


#### `vl_error_string`

```c
const char* vl_error_string(VLError err);
```

Return a static, human-readable description of an error code.

The returned pointer is valid for the lifetime of the process and must not be
freed.  Unknown codes return `"unknown error"`.


#### `vl_version_string`

```c
const char* vl_version_string(void);
```

Return the library version string (e.g. `"velociloops 0.1.0"`).

The returned pointer is valid for the lifetime of the process.
