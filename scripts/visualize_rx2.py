"""Visualize an RX2 file waveform with slice markers using velociloops_shared."""

import ctypes
import os
import sys
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from pathlib import Path

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).parent.parent

if sys.platform == "darwin":
    extension = "dylib"
elif sys.platform == "win32":
    extension = "dll"
else:
    extension = "so"
LIB_PATH  = REPO_ROOT / "build" / "Debug" / f"libvelociloops_shared.{extension}"

if not LIB_PATH.exists():
    sys.exit(f"Library not found: {LIB_PATH}")

lib = ctypes.CDLL(str(LIB_PATH))

# ---------------------------------------------------------------------------
# C types
# ---------------------------------------------------------------------------

VLFile  = ctypes.c_void_p
VLError = ctypes.c_int

class VLFileInfo(ctypes.Structure):
    _fields_ = [
        ("channels",          ctypes.c_int32),
        ("sample_rate",       ctypes.c_int32),
        ("slice_count",       ctypes.c_int32),
        ("tempo",             ctypes.c_int32),
        ("original_tempo",    ctypes.c_int32),
        ("ppq_length",        ctypes.c_int32),
        ("time_sig_num",      ctypes.c_int32),
        ("time_sig_den",      ctypes.c_int32),
        ("bit_depth",         ctypes.c_int32),
        ("total_frames",      ctypes.c_int32),
        ("loop_start",        ctypes.c_int32),
        ("loop_end",          ctypes.c_int32),
        ("processing_gain",   ctypes.c_int32),
        ("transient_enabled", ctypes.c_int32),
        ("transient_attack",  ctypes.c_int32),
        ("transient_decay",   ctypes.c_int32),
        ("transient_stretch", ctypes.c_int32),
        ("silence_selected",  ctypes.c_int32),
    ]

class VLSliceInfo(ctypes.Structure):
    _fields_ = [
        ("ppq_pos",       ctypes.c_int32),
        ("sample_length", ctypes.c_int32),
        ("sample_start",  ctypes.c_int32),
    ]

lib.vl_open.argtypes             = [ctypes.c_char_p, ctypes.POINTER(VLError)]
lib.vl_open.restype              = VLFile
lib.vl_close.argtypes            = [VLFile]
lib.vl_close.restype             = None
lib.vl_get_info.argtypes         = [VLFile, ctypes.POINTER(VLFileInfo)]
lib.vl_get_info.restype          = VLError
lib.vl_get_slice_info.argtypes   = [VLFile, ctypes.c_int32, ctypes.POINTER(VLSliceInfo)]
lib.vl_get_slice_info.restype    = VLError
lib.vl_get_slice_frame_count.argtypes = [VLFile, ctypes.c_int32]
lib.vl_get_slice_frame_count.restype  = ctypes.c_int32
lib.vl_decode_slice.argtypes     = [
    VLFile, ctypes.c_int32,
    ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
    ctypes.c_int32, ctypes.POINTER(ctypes.c_int32),
]
lib.vl_decode_slice.restype      = VLError
lib.vl_error_string.argtypes     = [VLError]
lib.vl_error_string.restype      = ctypes.c_char_p

# ---------------------------------------------------------------------------
# Load
# ---------------------------------------------------------------------------

def load_rx2(path: str):
    err = VLError(0)
    handle = lib.vl_open(path.encode(), ctypes.byref(err))
    if not handle:
        raise RuntimeError(lib.vl_error_string(err).decode())

    info = VLFileInfo()
    lib.vl_get_info(handle, ctypes.byref(info))

    sr           = info.sample_rate
    total        = info.total_frames
    # Use actual audio length for timing — the PPQ formula uses 15360 ticks/beat
    loop_dur_sec = total / sr

    # Build full waveform buffer — place each decoded slice at its sample_start
    # offset, limiting to sample_length frames (no transient-stretch tail).
    left_full  = np.zeros(total, dtype=np.float32)
    right_full = np.zeros(total, dtype=np.float32)
    slice_times = []

    for i in range(info.slice_count):
        si = VLSliceInfo()
        lib.vl_get_slice_info(handle, i, ctypes.byref(si))

        slice_times.append(si.sample_start / sr)

        n_render = lib.vl_get_slice_frame_count(handle, i)
        if n_render <= 0:
            continue

        L = (ctypes.c_float * n_render)()
        R = (ctypes.c_float * n_render)()
        written = ctypes.c_int32(0)
        e = lib.vl_decode_slice(handle, i, L, R, n_render, ctypes.byref(written))
        if e != 0:
            print(f"  slice {i}: {lib.vl_error_string(e).decode()}", file=sys.stderr)
            continue

        # Only use the raw sample_length frames (skip transient-stretch tail)
        count = min(written.value, si.sample_length)
        start = si.sample_start
        end   = min(start + count, total)
        span  = end - start
        if span > 0:
            left_full[start:end]  = np.array(L[:span], dtype=np.float32)
            right_full[start:end] = np.array(R[:span], dtype=np.float32)

    lib.vl_close(handle)

    time_axis = np.linspace(0.0, loop_dur_sec, total, endpoint=False)
    bpm = (info.tempo / 1000.0)
    return time_axis, left_full, right_full, slice_times, loop_dur_sec, bpm, sr, info.channels

# ---------------------------------------------------------------------------
# Peak-envelope downsampling for display
# ---------------------------------------------------------------------------

def peak_envelope(samples: np.ndarray, n_bins: int):
    """Return (mins, maxs) arrays of length n_bins via simple strided reshape."""
    n = len(samples)
    if n == 0:
        return np.zeros(n_bins), np.zeros(n_bins)
    # Pad to multiple of n_bins
    pad = (-n) % n_bins
    padded = np.concatenate([samples, np.zeros(pad, dtype=samples.dtype)])
    reshaped = padded.reshape(n_bins, -1)
    return reshaped.min(axis=1), reshaped.max(axis=1)

# ---------------------------------------------------------------------------
# Plot
# ---------------------------------------------------------------------------

# dBFS levels → linear amplitude positions for Y-axis reference lines
DB_REFS  = [0, -6, -12]
LIN_REFS = [10 ** (db / 20.0) for db in DB_REFS]

WAVEFORM_COLOR = "#5c7191"
BG_COLOR       = "#f1f2f4"
RULER_BG       = "#e8e9eb"
GRID_COLOR     = "#b3b5b8"
ZERO_COLOR     = "#b3b5b8"
MARKER_COLOR   = "#919497"

DISPLAY_BINS = 4000


def _draw_channel(ax, samples, t_start, t_end, slice_times):
    t_bins = np.linspace(t_start, t_end, DISPLAY_BINS, endpoint=False)
    mins, maxs = peak_envelope(samples, DISPLAY_BINS)

    ax.set_facecolor(BG_COLOR)

    # Waveform: fill between min and max envelope (proper peak-hold display)
    ax.fill_between(t_bins, mins, maxs, color=WAVEFORM_COLOR, linewidth=0, alpha=1.0, zorder=3)
    ax.plot(t_bins, maxs, color=WAVEFORM_COLOR, linewidth=0.7, zorder=4)
    ax.plot(t_bins, mins, color=WAVEFORM_COLOR, linewidth=0.7, zorder=4)

    # Slice markers
    for t in slice_times:
        ax.axvline(t, color=MARKER_COLOR, linewidth=0.75, alpha=0.95, zorder=5)

    # Y axis: ticks at dBFS reference positions
    y_pos, y_labels = [], []
    for db, lin in zip(DB_REFS, LIN_REFS):
        label = "0" if db == 0 else str(db)
        y_pos.append( lin); y_labels.append(label)
        y_pos.append(-lin); y_labels.append("" if db != 0 else "")
    y_pos.append(0); y_labels.append("0")

    ax.set_yticks(y_pos)
    ax.set_yticklabels(y_labels, fontsize=6.5)
    ax.set_ylim(-1.05, 1.05)
    ax.tick_params(axis="y", length=2, pad=2, colors=MARKER_COLOR)

    for spine in ax.spines.values():
        spine.set_edgecolor("#aaaacc")
        spine.set_linewidth(0.5)


def plot(time_axis, left, right, slice_times, loop_dur, bpm, sr, channels, title):
    n_ch = 2 if channels == 2 else 1
    fig_h = 6.2 if n_ch == 2 else 3.5

    height_ratios = [0.075] + [1.0] * n_ch
    fig, all_axes = plt.subplots(
        n_ch + 1, 1,
        figsize=(13.4, fig_h),
        gridspec_kw={"hspace": 0.0, "height_ratios": height_ratios},
    )
    fig.patch.set_facecolor("#f2f2f8")

    ax_ruler = all_axes[0]
    ch_axes  = all_axes[1:]

    # ---- Ruler (top bar with time ticks and triangle slice markers) ----
    ax_ruler.set_facecolor(RULER_BG)
    ax_ruler.set_xlim(0, loop_dur)
    ax_ruler.set_ylim(0, 1)
    ax_ruler.axis("off")

    tick_step = max(0.05, round(loop_dur / 40, 2))
    t = 0.0
    while t <= loop_dur + 1e-9:
        ax_ruler.axvline(t, color=MARKER_COLOR, linewidth=0.4, alpha=0.4)
        ax_ruler.text(t + 0.002, 0.08, f"{t:.2g}",
                      ha="left", va="bottom", fontsize=5.5, color=MARKER_COLOR)
        t = round(t + tick_step, 10)

    # Downward triangles at slice positions
    for t in slice_times:
        ax_ruler.annotate(
            "", xy=(t, 0.05), xytext=(t, 0.85),
            arrowprops=dict(arrowstyle="-|>", color=MARKER_COLOR,
                            lw=0.7, mutation_scale=5),
        )

    # ---- Channel panels ----
    t0, t1 = time_axis[0], time_axis[-1]
    data_list = [left, right] if n_ch == 2 else [left]

    for ax, data in zip(ch_axes, data_list):
        _draw_channel(ax, data, t0, t1, slice_times)
        ax.set_xlim(0, loop_dur)
        ax.tick_params(axis="x", bottom=False, labelbottom=False)

    # X ticks on bottom channel only
    ax_bot = ch_axes[-1]
    ax_bot.tick_params(axis="x", bottom=True, labelbottom=True,
                       labelsize=6.5, length=2, colors=MARKER_COLOR)
    ax_bot.xaxis.set_major_locator(ticker.MultipleLocator(tick_step))
    ax_bot.xaxis.set_major_formatter(
        ticker.FuncFormatter(lambda v, _: f"{v:.2g}")
    )
    plt.subplots_adjust(left=0.03, right=1.0, top=1.0, bottom=0.04)
    return fig

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    rx2_path = sys.argv[1] if len(sys.argv) > 1 else str(
        REPO_ROOT / "tests" / "data" / "120Stereo.rx2"
    )

    if not os.path.exists(rx2_path):
        sys.exit(f"File not found: {rx2_path}")

    print(f"Loading {rx2_path} …")
    time_axis, left, right, slice_times, loop_dur, bpm, sr, channels = load_rx2(rx2_path)
    print(f"  {channels}ch  {sr} Hz  {bpm:.1f} BPM  {loop_dur:.3f}s  {len(slice_times)} slices")

    fig = plot(time_axis, left, right, slice_times, loop_dur, bpm, sr, channels,
               title=Path(rx2_path).name)

    out_png = REPO_ROOT / (Path(rx2_path).stem + ".png")
    fig.savefig(out_png, dpi=100, bbox_inches=None, facecolor=fig.get_facecolor())
    print(f"Saved: {out_png}")
    plt.show()
