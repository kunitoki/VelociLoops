/**
 * @file velociloops.h
 * @brief VelociLoops — C API for reading and writing REX2 audio loop files.
 *
 * REX2 (.rx2) is a loop-centric audio container developed by Propellerhead
 * Software.  It stores a full-loop waveform compressed with the proprietary
 * DWOP codec, together with per-slice metadata that lets a host reconstruct
 * the loop at any tempo without pitch-shifting the transients.
 *
 * This library provides:
 *  - DWOP decode and encode (mono/stereo 16-bit and 24-bit integer paths)
 *  - Per-slice float extraction with transient-stretch rendering
 *  - Full round-trip: read an existing file, mutate slices, save back
 *  - A plain C89-compatible interface suitable for embedding in C and C++
 *    hosts, DAWs, and plug-ins
 *
 * ### Typical read workflow
 * @code
 *   VLError err;
 *   VLFile  f = vl_open("loop.rx2", &err);
 *   if (!f) { fprintf(stderr, "%s\n", vl_error_string(err)); return; }
 *
 *   VLFileInfo info;
 *   vl_get_info(f, &info);
 *
 *   int32_t n = vl_get_slice_frame_count(f, 0);
 *   float  *L = malloc(n * sizeof(float));
 *   float  *R = malloc(n * sizeof(float));
 *   int32_t written;
 *   vl_decode_slice(f, 0, L, R, 0, n, &written);
 *
 *   vl_close(f);
 * @endcode
 *
 * ### Typical write workflow
 * @code
 *   VLFile out = vl_create_new(2, 44100, 120000, NULL);  // stereo, 44.1 kHz, 120 BPM
 *   vl_add_slice(out, 0, left, right, frames);
 *   vl_save(out, "out.rx2");
 *   vl_close(out);
 * @endcode
 */

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Opaque handle representing an open REX2 file.
 *
 * Obtained from vl_open(), vl_open_from_memory(), or vl_create_new().
 * Must be released with vl_close() when no longer needed.
 */
typedef struct VLFile_s* VLFile;

/* -----------------------------------------------------------------------
   Error codes
   ----------------------------------------------------------------------- */

/**
 * @brief Return codes for all fallible API calls.
 *
 * All functions that can fail return a VLError (or use it as an out-parameter).
 * Zero (VL_OK) indicates success; every negative value is a distinct failure
 * reason.  Use vl_error_string() to obtain a human-readable description.
 */
typedef enum
{
    VL_OK = 0,                         /**< Success. */
    VL_ERROR_INVALID_HANDLE = -1,      /**< NULL or already-closed VLFile passed. */
    VL_ERROR_INVALID_ARG = -2,         /**< A required pointer argument was NULL or a
                                             numeric argument was out of range. */
    VL_ERROR_FILE_NOT_FOUND = -3,      /**< Path does not exist or cannot be opened. */
    VL_ERROR_FILE_CORRUPT = -4,        /**< IFF/DWOP structure is malformed or
                                             mandatory chunks are missing. */
    VL_ERROR_OUT_OF_MEMORY = -5,       /**< Heap allocation failed. */
    VL_ERROR_INVALID_SLICE = -6,       /**< Slice index is negative or >= slice_count. */
    VL_ERROR_INVALID_SAMPLE_RATE = -7, /**< Sample rate is zero, negative, or
                                             unsupported for the requested operation. */
    VL_ERROR_BUFFER_TOO_SMALL = -8,    /**< Caller-supplied buffer capacity is less than
                                             vl_get_slice_frame_count() for that slice. */
    VL_ERROR_NO_CREATOR_INFO = -9,     /**< The file has no CREI chunk; creator info
                                             cannot be read. */
    VL_ERROR_NOT_IMPLEMENTED = -10,    /**< Feature is parsed/stored but not yet
                                             functional (e.g. resampling). */
    VL_ERROR_ALREADY_HAS_DATA = -11,   /**< vl_set_info() or vl_set_creator_info() was
                                             called after slices were already added. */
    VL_ERROR_FILE_TOO_NEW = -12,       /**< File was written by a newer unsupported
                                             ReCycle/REX version. */
    VL_ERROR_ZERO_LOOP_LENGTH = -13,   /**< File declares no usable loop length. */
    VL_ERROR_INVALID_SIZE = -14,       /**< File or chunk size fields are invalid or
                                             inconsistent with the payload. */
    VL_ERROR_INVALID_TEMPO = -15,      /**< Tempo field is zero or outside the
                                             supported ReCycle range. */
} VLError;

/**
 * @brief Bitmask flags for slice editing state.
 *
 * These are exposed in VLSliceInfo::flags and can be set via vl_set_slice_info().
 * Muted, locked, and selected state are preserved in the SLCE chunk.  Marker
 * and synthetic bits are derived by the reader from raw SLCE entries and ignored
 * by the writer.
 */
enum
{
    VL_SLICE_FLAG_MUTED = 1 << 0,
    VL_SLICE_FLAG_LOCKED = 1 << 1,
    VL_SLICE_FLAG_SELECTED = 1 << 2,
    VL_SLICE_FLAG_MARKER = 1 << 3,
    VL_SLICE_FLAG_SYNTHETIC = 1 << 4
};

/* -----------------------------------------------------------------------
   Metadata structures
   ----------------------------------------------------------------------- */

/**
 * @brief File-level metadata for a REX2 loop.
 *
 * Populated by vl_get_info().  All integer fields use the host's native byte
 * order.  When creating a new file, fill this struct and pass it to
 * vl_set_info() before adding slices.
 */
typedef struct
{
    int32_t channels;    /**< Channel count: 1 = mono, 2 = stereo. */
    int32_t sample_rate; /**< Native sample rate in Hz (e.g. 44100, 48000). */
    int32_t slice_count; /**< Number of slices in the file. */

    /**
     * @brief Playback tempo in units of BPM × 1000.
     *
     * Example: 120 BPM is stored as 120000.  Supplied to vl_create_new() and
     * can be overridden via vl_set_info().
     */
    int32_t tempo;

    /**
     * @brief Original recording tempo in units of BPM × 1000.
     *
     * Stored in the RECY chunk.  May differ from @c tempo when the loop has
     * been time-stretched in ReCycle.
     */
    int32_t original_tempo;

    /**
     * @brief Loop length in PPQ (Pulse Per Quarter-note) ticks.
     *
     * The REX2 format uses kREXPPQ = 15360 ticks per bar (4/4).  One quarter-
     * note is therefore 3840 ticks.  Use this together with @c tempo to compute
     * the loop duration in seconds:
     * @code
     *   double bars     = (double)info.ppq_length / 15360.0;
     *   double bpm      = info.tempo / 1000.0;
     *   double seconds  = bars * (4.0 * 60.0 / bpm);
     * @endcode
     */
    int32_t ppq_length;

    int32_t time_sig_num; /**< Time-signature numerator (e.g. 4 for 4/4). */
    int32_t time_sig_den; /**< Time-signature denominator (e.g. 4 for 4/4). */
    int32_t bit_depth;    /**< Source/authored bit depth: 16 or 24. */

    /**
     * @brief Total PCM frame count decoded from the SDAT/DWOP payload.
     *
     * This is the length of the raw waveform before any slice-level rendering.
     * Frames are interleaved for stereo (left, right, left, right, …).
     */
    int32_t total_frames;

    /**
     * @brief First PCM frame of the loop region (inclusive).
     *
     * Together with @c loop_end this defines the portion of the waveform that
     * was marked as the active loop in ReCycle.
     */
    int32_t loop_start;

    /**
     * @brief Last PCM frame of the loop region (exclusive).
     *
     * Zero indicates no explicit loop end (treat the whole file as the loop).
     */
    int32_t loop_end;

    /**
     * @brief GLOB gain scalar applied during slice decode.
     *
     * Stored as a raw integer from the GLOB chunk.  The decoder converts it to
     * a floating-point level via: @c level = processing_gain × 0.000833333354f.
     * A value of 1200 corresponds to unity gain.  Read-only; not used when
     * assembling a new file from scratch.
     */
    int32_t processing_gain;

    /** @brief Non-zero if the transient shaper is active. */
    int32_t transient_enabled;

    /**
     * @brief Transient shaper attack time (raw TRSH chunk value).
     *
     * Controls how quickly the shaper responds to the onset of a transient.
     * Larger values = slower attack.
     */
    int32_t transient_attack;

    /**
     * @brief Transient shaper decay time (raw TRSH chunk value).
     *
     * Controls how quickly the shaper releases after a transient.
     */
    int32_t transient_decay;

    /**
     * @brief Transient stretch amount (raw TRSH chunk value).
     *
     * A non-zero value extends the rendered frame count beyond the raw slice
     * length (see vl_get_slice_frame_count()).  The extra frames are filled by
     * a reverse-playback loop that decays to silence.
     */
    int32_t transient_stretch;

    /**
     * @brief Non-zero if the "silence selected" flag is set in the GLOB chunk.
     *
     * When set, the original ReCycle session had silence in the selection.
     */
    int32_t silence_selected;
} VLFileInfo;

/**
 * @brief Per-slice metadata for one audio segment.
 *
 * Populated by vl_get_slice_info().  Indices are zero-based.
 */
typedef struct
{
    /**
     * @brief Slice position in PPQ ticks from the loop start.
     *
     * Use together with VLFileInfo::tempo and VLFileInfo::ppq_length to map
     * slices onto a musical timeline.
     */
    int32_t ppq_pos;

    /**
     * @brief Raw frame count of this slice in the decoded PCM buffer.
     *
     * This is the length of the source waveform segment at the file's native
     * sample rate.  It does NOT include any transient-stretch tail.  For the
     * actual number of frames vl_decode_slice() will write, call
     * vl_get_slice_frame_count().
     */
    int32_t sample_length;

    /**
     * @brief Frame offset into the decoded PCM buffer where this slice starts.
     *
     * Multiply by VLFileInfo::channels to get the interleaved sample index.
     */
    int32_t sample_start;

    /**
     * @brief Raw slice-analysis point value from the SLCE chunk.
     *
     * Official files use this together with analysis sensitivity to decide
     * whether a marker-like SLCE entry becomes a visible slice boundary.
     * VelociLoops applies that visibility pass when reading and preserves the
     * raw value for public inspection and re-emission.
     */
    int32_t analysis_points;

    /**
     * @brief Bitmask of VL_SLICE_FLAG_* values.
     *
     * Muted, locked, selected, and synthetic-leading state are exposed here so
     * callers can inspect and preserve slice editing state. Marker-like raw
     * SLCE entries are recognized during parsing but filtered out of the
     * public renderable slice list.
     */
    int32_t flags;
} VLSliceInfo;

/**
 * @brief Optional creator and tag metadata embedded in the REX2 CREI chunk.
 *
 * Populated by vl_get_creator_info().  All fields are NUL-terminated UTF-8
 * strings.  Fields absent in the file are returned as empty strings ("").
 * Returns VL_ERROR_NO_CREATOR_INFO when the file contains no CREI chunk.
 */
typedef struct
{
    char name[256];      /**< Creator or artist name. */
    char copyright[256]; /**< Copyright notice. */
    char url[256];       /**< Creator website URL. */
    char email[256];     /**< Creator contact e-mail. */
    char free_text[256]; /**< Arbitrary free-form text. */
} VLCreatorInfo;

/**
 * @brief Tunable parameters for vl_create_from_superflux().
 *
 * Initialize with vl_superflux_default_options() before changing individual
 * fields.  Pass NULL to vl_create_from_superflux() to use the same defaults.
 *
 * Time fields are seconds except @c combine_ms and @c delay_ms, which are
 * milliseconds to match common onset-picking terminology.
 */
typedef struct
{
    int32_t frame_size;       /**< FFT/window size in samples. Must be a power of two. Default: 2048. */
    int32_t fps;              /**< Onset detection frames per second. Default: 200. */
    int32_t filter_bands;     /**< Log-frequency filter bands per octave. Default: 24. */
    int32_t max_bins;         /**< Frequency bins for SuperFlux maximum filtering. Default: 3. */
    int32_t diff_frames;      /**< Previous-frame distance; <= 0 derives it from @c ratio. */
    int32_t min_slice_frames; /**< Minimum frames between slice starts. <= 0 uses 10 ms. */
    int32_t filter_equal;     /**< Non-zero normalizes each triangular filter area. */
    int32_t online;           /**< Non-zero uses causal framing and peak picking. */

    float threshold;    /**< Peak-picking threshold over local average. Default: 1.1. */
    float combine_ms;   /**< Suppress detections within this many ms. Default: 50. */
    float pre_avg;      /**< Seconds before the peak for moving average. Default: 0.15. */
    float pre_max;      /**< Seconds before the peak for moving maximum. Default: 0.01. */
    float post_avg;     /**< Seconds after the peak for moving average. Default: 0.0. */
    float post_max;     /**< Seconds after the peak for moving maximum. Default: 0.05. */
    float delay_ms;     /**< Detection timestamp offset in ms. Default: 0. */
    float ratio;        /**< Window ratio used to derive @c diff_frames. Default: 0.5. */
    float fmin;         /**< Filterbank minimum frequency in Hz. Default: 30. */
    float fmax;         /**< Filterbank maximum frequency in Hz. Default: 17000. */
    float log_mul;      /**< Magnitude multiplier before log10. Default: 1. */
    float log_add;      /**< Positive value added before log10. Default: 1. */
} VLSuperFluxOptions;

/* -----------------------------------------------------------------------
   Open / close
   ----------------------------------------------------------------------- */

/**
 * @brief Load and decode a REX2 file from disk.
 *
 * Parses the IFF/CIFF container, validates mandatory chunks (HEAD, SINF, GLOB,
 * SLCL, SDAT), and decompresses the DWOP bitstream into an internal PCM
 * buffer.  Decompression is O(n) in the total frame count.
 *
 * @param path  NUL-terminated path to the .rx2 file.
 * @param err   If non-NULL, receives the status code on both success and
 *              failure.  On success, *err is set to VL_OK.
 * @return      A valid VLFile handle, or NULL on failure.
 *              Always call vl_close() on a non-NULL return value.
 */
VLFile vl_open(const char* path, VLError* err);

/**
 * @brief Load and decode a REX2 file from a caller-owned memory buffer.
 *
 * Identical to vl_open() except it reads from @p data instead of a file.
 * The library copies whatever internal data it needs; the caller may free
 * @p data after this call returns.
 *
 * @param data  Pointer to the raw .rx2 file bytes.
 * @param size  Byte length of @p data.
 * @param err   Receives the status code; may be NULL.
 * @return      A valid VLFile handle, or NULL on failure.
 */
VLFile vl_open_from_memory(const void* data, size_t size, VLError* err);

/**
 * @brief Create a new, empty file handle for assembling a REX2 loop.
 *
 * The returned handle is ready for vl_set_info(), vl_set_creator_info(), and
 * repeated vl_add_slice() calls, followed by vl_save() or vl_save_to_memory().
 * Metadata may be set freely until the first vl_add_slice() call; after that
 * vl_set_info() and vl_set_creator_info() return VL_ERROR_ALREADY_HAS_DATA.
 *
 * @param channels     1 (mono) or 2 (stereo).
 * @param sample_rate  Output sample rate in Hz (e.g. 44100).
 * @param tempo        Playback tempo in BPM × 1000 (e.g. 120000 for 120 BPM).
 * @param err          Receives the status code; may be NULL.
 * @return             A valid VLFile handle, or NULL on failure.
 */
VLFile vl_create_new(int32_t channels, int32_t sample_rate, int32_t tempo, VLError* err);

/**
 * @brief Fill a VLSuperFluxOptions struct with the library defaults.
 *
 * @param out Pointer to caller-allocated options. NULL is ignored.
 */
void vl_superflux_default_options(VLSuperFluxOptions* out);

/**
 * @brief Create a sliced REX2 authoring handle from full-loop float PCM.
 *
 * The input is a complete mono or stereo loop as non-interleaved float buffers.
 * VelociLoops downmixes stereo to mono for SuperFlux onset detection, then
 * copies the original channel data into contiguous slices.  The returned handle
 * is equivalent to one assembled with vl_create_new() and vl_add_slice(), and
 * can be passed directly to vl_save() or vl_save_to_memory().
 *
 * @param channels     1 (mono) or 2 (stereo).
 * @param sample_rate  Input/output sample rate in Hz.
 * @param tempo        Playback tempo in BPM × 1000.
 * @param left         Left or mono input buffer, @p frames samples.
 * @param right        Right input buffer for stereo; must be non-NULL when
 *                     @p channels is 2. Ignored for mono.
 * @param frames       Number of PCM frames in each input buffer.
 * @param options      SuperFlux parameters, or NULL for defaults.
 * @param err          Receives the status code; may be NULL.
 * @return             A valid VLFile handle ready to save, or NULL on failure.
 */
VLFile vl_create_from_superflux(int32_t channels,
                                int32_t sample_rate,
                                int32_t tempo,
                                const float* left,
                                const float* right,
                                int32_t frames,
                                const VLSuperFluxOptions* options,
                                VLError* err);

/**
 * @brief Release all resources associated with a VLFile handle.
 *
 * The handle is invalid after this call.  Safe to call with NULL (no-op).
 */
void vl_close(VLFile file);

/* -----------------------------------------------------------------------
   Read: metadata
   ----------------------------------------------------------------------- */

/**
 * @brief Retrieve file-level metadata.
 *
 * @param file  Open VLFile handle.
 * @param out   Pointer to a caller-allocated VLFileInfo; filled on VL_OK.
 *              All integer fields are returned in host byte order.
 * @return      VL_OK, VL_ERROR_INVALID_HANDLE, or VL_ERROR_INVALID_ARG.
 */
VLError vl_get_info(VLFile file, VLFileInfo* out);

/**
 * @brief Retrieve creator/tag metadata from the CREI chunk.
 *
 * @param file  Open VLFile handle.
 * @param out   Pointer to a caller-allocated VLCreatorInfo; filled on VL_OK.
 * @return      VL_OK on success.
 *              VL_ERROR_NO_CREATOR_INFO if the file contains no CREI chunk.
 *              VL_ERROR_INVALID_HANDLE or VL_ERROR_INVALID_ARG on bad input.
 */
VLError vl_get_creator_info(VLFile file, VLCreatorInfo* out);

/* -----------------------------------------------------------------------
   Read: slice enumeration
   ----------------------------------------------------------------------- */

/**
 * @brief Retrieve per-slice metadata by index.
 *
 * Realtime safety: after the VLFile has been opened or authored, this function
 * performs no heap allocation and writes only to the caller-supplied @p out
 * struct. It is suitable for an audio thread when no other thread is mutating
 * or closing the same VLFile.
 *
 * @param file   Open VLFile handle.
 * @param index  Zero-based slice index in [0, VLFileInfo::slice_count).
 * @param out    Pointer to a caller-allocated VLSliceInfo; filled on VL_OK.
 * @return       VL_OK on success.
 *               VL_ERROR_INVALID_SLICE if @p index is out of range.
 */
VLError vl_get_slice_info(VLFile file, int32_t index, VLSliceInfo* out);

/**
 * @brief Update the public slice editing state for an existing slice.
 *
 * Only VL_SLICE_FLAG_MUTED, VL_SLICE_FLAG_LOCKED, and
 * VL_SLICE_FLAG_SELECTED are serialized to SLCE flags. Marker and synthetic
 * bits are derived by the reader and ignored here.
 *
 * @param file            Open VLFile handle.
 * @param index           Zero-based slice index.
 * @param flags           Bitmask of VL_SLICE_FLAG_* values.
 * @param analysis_points Raw analysis point value, or a negative value to keep
 *                        the current value.
 * @return                VL_OK, VL_ERROR_INVALID_HANDLE,
 *                        VL_ERROR_INVALID_SLICE, or VL_ERROR_INVALID_ARG.
 */
VLError vl_set_slice_info(VLFile file, int32_t index, int32_t flags, int32_t analysis_points);

/* -----------------------------------------------------------------------
   Read: sample extraction
   ----------------------------------------------------------------------- */

/**
 * @brief Set the output sample rate for subsequent vl_decode_slice() calls.
 *
 * Stores @p rate for future use.  Resampling is not yet implemented; calling
 * this function with a rate that differs from the file's native rate currently
 * returns VL_ERROR_NOT_IMPLEMENTED.
 *
 * @param file  Open VLFile handle.
 * @param rate  Desired output sample rate in Hz.
 * @return      VL_OK if @p rate matches the file's native rate.
 *              VL_ERROR_NOT_IMPLEMENTED for any other non-zero rate.
 *              VL_ERROR_INVALID_ARG if @p rate <= 0.
 */
VLError vl_set_output_sample_rate(VLFile file, int32_t rate);

/**
 * @brief Return the number of frames vl_decode_slice() will write for a slice.
 *
 * This value accounts for any transient-stretch tail appended beyond the raw
 * slice waveform.  Use it to pre-allocate the left/right buffers before
 * calling vl_decode_slice().
 *
 * Realtime safety: after the VLFile has been opened or authored, this function
 * performs no heap allocation and only reads immutable slice metadata. It is
 * suitable for an audio thread when no other thread is mutating or closing the
 * same VLFile.
 *
 * @param file   Open VLFile handle.
 * @param index  Zero-based slice index.
 * @return       Frame count (>= 1) on success, or a negative VLError code
 *               cast to int32_t (e.g. VL_ERROR_INVALID_SLICE).
 */
int32_t vl_get_slice_frame_count(VLFile file, int32_t index);

/**
 * @brief Decode one slice into caller-supplied float PCM buffers.
 *
 * Renders the slice waveform into normalized floating-point samples in the
 * range [-1.0, 1.0].  The rendering pipeline includes:
 *  - Processing gain from the GLOB chunk
 *  - Transient-stretch tail (reverse-playback loop with envelope decay)
 *  - A two-frame read offset matching the behaviour of REXRenderSlice
 *
 * For mono files, the left channel is mirrored into @p right when @p right
 * is non-NULL.
 *
 * Realtime safety: after the VLFile has been opened or authored, this function
 * performs no heap allocation, does not resize internal storage, and writes
 * only to caller-supplied buffers. It is suitable for an audio thread when
 * @p left and @p right are preallocated, @p capacity is large enough, and no
 * other thread is mutating or closing the same VLFile.
 *
 * @param file       Open VLFile handle.
 * @param index      Zero-based slice index.
 * @param left       Output buffer for the left (or mono) channel.
 *                   Must hold at least @p capacity floats.
 * @param right      Output buffer for the right channel; may be NULL for
 *                   mono extraction.
 * @param frame_offset Starting frame inside the rendered slice.  Use 0 to
 *                   decode from the beginning.
 * @param capacity   Size of each buffer in frames.
 *                   Must be >= vl_get_slice_frame_count(file, index) -
 *                   @p frame_offset.
 * @param frames_out If non-NULL, receives the actual frame count written
 *                   (<= @p capacity).  Always equals
 *                   vl_get_slice_frame_count(file, index) - @p frame_offset
 *                   on VL_OK.
 * @return           VL_OK on success.
 *                   VL_ERROR_BUFFER_TOO_SMALL if capacity is insufficient.
 *                   VL_ERROR_INVALID_SLICE for an out-of-range index.
 *                   VL_ERROR_INVALID_ARG if @p left is NULL or
 *                   @p frame_offset is outside the rendered slice.
 */
VLError vl_decode_slice(VLFile file, int32_t index, float* left, float* right, int32_t frame_offset, int32_t capacity, int32_t* frames_out);

/* -----------------------------------------------------------------------
   Write: assembly from audio slices
   ----------------------------------------------------------------------- */

/**
 * @brief Overwrite file-level metadata on a handle from vl_create_new().
 *
 * Must be called before the first vl_add_slice(); afterwards returns
 * VL_ERROR_ALREADY_HAS_DATA.
 *
 * @param file  Handle created with vl_create_new().
 * @param info  Metadata to apply.  The library ignores @c slice_count,
 *              @c total_frames, @c loop_start, and @c loop_end (these are
 *              computed from the added slices).
 * @return      VL_OK, VL_ERROR_INVALID_HANDLE, VL_ERROR_INVALID_ARG, or
 *              VL_ERROR_ALREADY_HAS_DATA.
 */
VLError vl_set_info(VLFile file, const VLFileInfo* info);

/**
 * @brief Set creator/tag metadata on a handle from vl_create_new().
 *
 * Must be called before the first vl_add_slice(); afterwards returns
 * VL_ERROR_ALREADY_HAS_DATA.
 *
 * @param file  Handle created with vl_create_new().
 * @param info  Creator fields to embed.  Any field may be an empty string.
 * @return      VL_OK, VL_ERROR_INVALID_HANDLE, VL_ERROR_INVALID_ARG, or
 *              VL_ERROR_ALREADY_HAS_DATA.
 */
VLError vl_set_creator_info(VLFile file, const VLCreatorInfo* info);

/**
 * @brief Append a slice from caller-supplied float audio data.
 *
 * Slices must be added in ascending @p ppq_pos order.  Each call stores the
 * raw float samples internally and assigns the next sequential slice index.
 * DWOP compression is deferred until vl_save() or vl_save_to_memory().
 *
 * @param file   Handle created with vl_create_new().
 * @param ppq_pos  Slice position in PPQ ticks from the loop start.
 * @param left   Left (or mono) channel samples in [-1.0, 1.0].
 * @param right  Right channel samples; may be NULL for mono files.
 * @param frames Number of frames in each buffer.
 * @return       The assigned slice index (>= 0) on success, or a negative
 *               VLError code on failure.
 */
int32_t vl_add_slice(VLFile file, int32_t ppq_pos, const float* left, const float* right, int32_t frames);

/**
 * @brief Remove a slice by index, shifting subsequent indices down.
 *
 * After this call, the slice that was at @p index + 1 is now at @p index.
 * Invalidates any previously cached rendered lengths.
 *
 * @param file   Open VLFile handle (read or write path).
 * @param index  Zero-based slice index to remove.
 * @return       VL_OK, VL_ERROR_INVALID_HANDLE, or VL_ERROR_INVALID_SLICE.
 */
VLError vl_remove_slice(VLFile file, int32_t index);

/**
 * @brief Encode and serialise the assembled file to disk.
 *
 * Compresses all slices with DWOP, builds the IFF/CIFF container, and writes
 * the result atomically to @p path.  The handle remains open and valid after
 * this call.
 *
 * @param file  Handle created with vl_create_new(), with at least one slice.
 * @param path  Destination file path (created or overwritten).
 * @return      VL_OK on success, or a negative VLError on failure.
 */
VLError vl_save(VLFile file, const char* path);

/**
 * @brief Encode and serialise the assembled file to a caller-owned buffer.
 *
 * Supports a two-call pattern for pre-allocation:
 *  1. Call with @p buf = NULL to obtain the required byte count in @p size_out.
 *  2. Allocate the buffer and call again with @p buf non-NULL.
 *
 * @code
 *   size_t sz = 0;
 *   vl_save_to_memory(file, NULL, &sz);        // query size
 *   uint8_t* buf = malloc(sz);
 *   vl_save_to_memory(file, buf, &sz);         // write
 * @endcode
 *
 * @param file      Handle created with vl_create_new().
 * @param buf       Destination buffer, or NULL for a size query.
 * @param size_out  On entry: capacity of @p buf (ignored when @p buf is NULL).
 *                  On exit: actual byte count written (or required).
 * @return          VL_OK on success, or a negative VLError on failure.
 */
VLError vl_save_to_memory(VLFile file, void* buf, size_t* size_out);

/* -----------------------------------------------------------------------
   Utility
   ----------------------------------------------------------------------- */

/**
 * @brief Return a static, human-readable string for an error code.
 *
 * The returned pointer is valid for the lifetime of the process and must not
 * be freed.  Unknown codes return "unknown error".
 *
 * @param err  Any VLError value.
 * @return     NUL-terminated ASCII description.
 */
const char* vl_error_string(VLError err);

/**
 * @brief Return the library version string.
 *
 * Format: "velociloops MAJOR.MINOR.PATCH" (e.g. "velociloops 0.1.0").
 * The returned pointer is valid for the lifetime of the process.
 *
 * @return NUL-terminated ASCII version string.
 */
const char* vl_version_string(void);

#ifdef __cplusplus
}
#endif
