#include "velociloops.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <vector>

/* -----------------------------------------------------------------------
   DWOP decompressor
   Ported from COptimizedDWOPDecompressor (decompiled DLL, macOS variant).
   ----------------------------------------------------------------------- */

namespace {

struct VLChannelState {
    int32_t  deltas[5]   = {0, 0, 0, 0, 0};
    uint32_t averages[5] = {2560, 2560, 2560, 2560, 2560};
};

class VLDWOPDecompressor {
public:
    VLChannelState   ch[2];
    uint32_t         currentWord = 0;
    int32_t          bitsLeft    = 0;
    const uint32_t*  inputPtr    = nullptr;
    const uint32_t*  endPtr      = nullptr;
    std::vector<uint32_t> buf;

    void init(const uint8_t* data, size_t size) {
        const size_t words = size / 4;
        buf.resize(words);
        for (size_t i = 0; i < words; ++i) {
            const size_t b = i * 4;
            buf[i] = ((uint32_t)data[b]     << 24)
                   | ((uint32_t)data[b + 1] << 16)
                   | ((uint32_t)data[b + 2] <<  8)
                   |  (uint32_t)data[b + 3];
        }
        inputPtr    = buf.data();
        endPtr      = buf.data() + buf.size();
        currentWord = 0;
        bitsLeft    = 0;
    }

    bool decompressMono(uint32_t frameCount, int16_t* out) {
        if (!out || frameCount == 0) return false;

        int32_t  d0 = ch[0].deltas[0] * 2, d1 = ch[0].deltas[1] * 2,
                 d2 = ch[0].deltas[2] * 2, d3 = ch[0].deltas[3] * 2,
                 d4 = ch[0].deltas[4] * 2;
        uint32_t a0 = ch[0].averages[0], a1 = ch[0].averages[1],
                 a2 = ch[0].averages[2], a3 = ch[0].averages[3],
                 a4 = ch[0].averages[4];

        uint32_t j    = 2;
        int32_t  rbits = 0;

        uint32_t        cw  = currentWord;
        int32_t         bl  = bitsLeft;
        const uint32_t* inp = inputPtr;
        bool            eof = false;

        for (uint32_t f = 0; f < frameCount && !eof; ++f) {
            uint32_t minAvg = a0; int minIdx = 0;
            if (a1 < minAvg) { minAvg = a1; minIdx = 1; }
            if (a2 < minAvg) { minAvg = a2; minIdx = 2; }
            if (a3 < minAvg) { minAvg = a3; minIdx = 3; }
            if (a4 < minAvg) { minAvg = a4; minIdx = 4; }

            uint32_t step     = ((minAvg * 3u) + 36u) >> 7;
            uint32_t prefixSum = 0;
            int      zerosWin  = 7;

            while (true) {
                bool bit = readBit(cw, bl, inp, eof);
                if (eof) break;
                if (bit) break;
                prefixSum += step;
                if (--zerosWin == 0) {
                    step <<= 2;
                    zerosWin = 7;
                }
            }
            if (eof) break;

            adjustJRbits(step, j, rbits);

            uint32_t rem = (rbits > 0) ? readBits(rbits, cw, bl, inp, eof) : 0;
            if (eof) break;

            const int32_t thresh = (int32_t)j - (int32_t)step;
            if ((int32_t)rem - thresh >= 0) {
                const uint32_t extra = readBits(1, cw, bl, inp, eof);
                if (eof) break;
                rem = rem * 2u - (uint32_t)thresh + extra;
            }

            const uint32_t codeVal  = rem + prefixSum;
            const int32_t  signed2x = -(int32_t)(codeVal & 1u) ^ (int32_t)codeVal;
            const int32_t  s2x      = applyPredictor(minIdx, signed2x, d0, d1, d2, d3, d4);
            out[f] = clampSample(s2x >> 1);
            updateAverages(a0, a1, a2, a3, a4, d0, d1, d2, d3, d4);
        }

        ch[0].deltas[0] = d0 >> 1; ch[0].deltas[1] = d1 >> 1;
        ch[0].deltas[2] = d2 >> 1; ch[0].deltas[3] = d3 >> 1;
        ch[0].deltas[4] = d4 >> 1;
        ch[0].averages[0] = a0; ch[0].averages[1] = a1; ch[0].averages[2] = a2;
        ch[0].averages[3] = a3; ch[0].averages[4] = a4;
        currentWord = cw; bitsLeft = bl; inputPtr = inp;
        return !eof;
    }

    bool decompressStereo(uint32_t frameCount, int16_t* out) {
        if (!out || frameCount == 0) return false;

        int32_t  d[2][5];
        uint32_t a[2][5];
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 5; ++i) {
                d[c][i] = ch[c].deltas[i] * 2;
                a[c][i] = ch[c].averages[i];
            }

        uint32_t j[2]    = {2, 2};
        int32_t  rbits[2] = {0, 0};

        uint32_t        cw  = currentWord;
        int32_t         bl  = bitsLeft;
        const uint32_t* inp = inputPtr;
        bool            eof = false;

        for (uint32_t f = 0; f < frameCount && !eof; ++f) {
            int32_t ch2x[2] = {0, 0};
            for (int c = 0; c < 2 && !eof; ++c) {
                uint32_t minAvg = a[c][0]; int minIdx = 0;
                if (a[c][1] < minAvg) { minAvg = a[c][1]; minIdx = 1; }
                if (a[c][2] < minAvg) { minAvg = a[c][2]; minIdx = 2; }
                if (a[c][3] < minAvg) { minAvg = a[c][3]; minIdx = 3; }
                if (a[c][4] < minAvg) { minAvg = a[c][4]; minIdx = 4; }

                uint32_t step      = ((minAvg * 3u) + 36u) >> 7;
                uint32_t prefixSum = 0;
                int      zerosWin  = 7;

                while (true) {
                    bool bit = readBit(cw, bl, inp, eof);
                    if (eof) break;
                    if (bit) break;
                    prefixSum += step;
                    if (--zerosWin == 0) {
                        step <<= 2;
                        zerosWin = 7;
                    }
                }
                if (eof) break;

                adjustJRbits(step, j[c], rbits[c]);

                uint32_t rem = (rbits[c] > 0) ? readBits(rbits[c], cw, bl, inp, eof) : 0;
                if (eof) break;

                const int32_t thresh = (int32_t)j[c] - (int32_t)step;
                if ((int32_t)rem - thresh >= 0) {
                    const uint32_t extra = readBits(1, cw, bl, inp, eof);
                    if (eof) break;
                    rem = rem * 2u - (uint32_t)thresh + extra;
                }

                const uint32_t codeVal  = rem + prefixSum;
                const int32_t  signed2x = -(int32_t)(codeVal & 1u) ^ (int32_t)codeVal;
                ch2x[c] = applyPredictor(minIdx, signed2x, d[c][0], d[c][1], d[c][2], d[c][3], d[c][4]);
                updateAverages(a[c][0], a[c][1], a[c][2], a[c][3], a[c][4],
                               d[c][0], d[c][1], d[c][2], d[c][3], d[c][4]);
            }
            if (eof) break;

            out[f * 2 + 0] = clampSample(ch2x[0] >> 1);
            out[f * 2 + 1] = clampSample((int32_t)(((int64_t)ch2x[0] + (int64_t)ch2x[1]) >> 1));
        }

        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < 5; ++i) {
                ch[c].deltas[i]   = d[c][i] >> 1;
                ch[c].averages[i] = a[c][i];
            }
        }
        currentWord = cw; bitsLeft = bl; inputPtr = inp;
        return !eof;
    }

private:
    static int16_t clampSample(int32_t v) {
        if (v >  32767) return  32767;
        if (v < -32768) return -32768;
        return (int16_t)v;
    }

    bool readBit(uint32_t& cw, int32_t& bl, const uint32_t*& inp, bool& eof) {
        const int32_t blBefore = bl--;
        if (blBefore - 1 < 0) {
            if (inp >= endPtr) { eof = true; return false; }
            cw = *inp++;
            bl = 0x1f;
        }
        const bool bit = (int32_t)cw < 0;
        cw <<= 1;
        return bit;
    }

    uint32_t readBits(int n, uint32_t& cw, int32_t& bl, const uint32_t*& inp, bool& eof) {
        if (n <= 0) return 0;
        uint32_t      result    = cw >> (32 - n);
        cw <<= n;
        const int32_t blBefore  = bl;
        bl -= n;
        if (blBefore - n < 0) {
            if (inp >= endPtr) { eof = true; return result; }
            const uint32_t next = *inp++;
            bl     += 32;
            result |= next >> bl;
            cw      = next << (32 - bl);
        }
        return result;
    }

    static void adjustJRbits(uint32_t step, uint32_t& j, int32_t& rbits) {
        if (step < j) {
            for (uint32_t jt = j >> 1; step < jt; jt >>= 1) { j = jt; --rbits; }
        } else {
            while (step >= j) {
                const uint32_t prev = j;
                j <<= 1; ++rbits;
                if (j <= prev) { j = prev; break; }
            }
        }
    }

    static int32_t applyPredictor(int idx, int32_t s2x,
                                   int32_t& d0, int32_t& d1, int32_t& d2,
                                   int32_t& d3, int32_t& d4) {
        switch (idx) {
            case 0: {
                const int32_t t0 = s2x - d0, t1 = t0 - d1, t2 = t1 - d2;
                d4 = t2 - d3; d3 = t2; d2 = t1; d1 = t0; d0 = s2x;
                return s2x;
            }
            case 1: {
                const int32_t t1 = s2x - d1, t2 = t1 - d2, nd0 = d0 + s2x;
                d4 = t2 - d3; d3 = t2; d2 = t1; d1 = s2x; d0 = nd0;
                return nd0;
            }
            case 2: {
                const int32_t nd1 = d1 + s2x, nd0 = d0 + nd1, t = s2x - d2;
                d4 = t - d3; d3 = t; d2 = s2x; d1 = nd1; d0 = nd0;
                return nd0;
            }
            case 3: {
                const int32_t nd2 = d2 + s2x, nd1 = d1 + nd2, nd0 = d0 + nd1;
                d4 = s2x - d3; d3 = s2x; d2 = nd2; d1 = nd1; d0 = nd0;
                return nd0;
            }
            case 4: {
                const int32_t nd3 = d3 + s2x, nd2 = d2 + nd3,
                              nd1 = d1 + nd2, nd0 = d0 + nd1;
                d4 = s2x; d3 = nd3; d2 = nd2; d1 = nd1; d0 = nd0;
                return nd0;
            }
            default: return d0; // LCOV_EXCL_LINE idx comes from a 0..4 minimum search.
        }
    }

    static void updateAverages(uint32_t& a0, uint32_t& a1, uint32_t& a2,
                                uint32_t& a3, uint32_t& a4,
                                int32_t d0, int32_t d1, int32_t d2,
                                int32_t d3, int32_t d4) {
        auto mag = [](int32_t v) -> uint32_t { return (uint32_t)(v ^ (v >> 31)); };
        a0 = a0 + mag(d0) - (a0 >> 5);
        a1 = a1 + mag(d1) - (a1 >> 5);
        a2 = a2 + mag(d2) - (a2 >> 5);
        a3 = a3 + mag(d3) - (a3 >> 5);
        a4 = a4 + mag(d4) - (a4 >> 5);
    }
};

class VLBitWriter {
public:
    void writeBit(bool bit) {
        if (bit) currentWord |= (uint32_t)1u << (31 - bitCount);
        if (++bitCount == 32) flushWord();
    }

    void writeBits(uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i)
            writeBit(((value >> i) & 1u) != 0);
    }

    std::vector<uint8_t> finish() {
        if (bitCount > 0) flushWord();
        return bytes;
    }

private:
    std::vector<uint8_t> bytes;
    uint32_t currentWord = 0;
    int bitCount = 0;

    void flushWord() {
        bytes.push_back((uint8_t)(currentWord >> 24));
        bytes.push_back((uint8_t)(currentWord >> 16));
        bytes.push_back((uint8_t)(currentWord >> 8));
        bytes.push_back((uint8_t)currentWord);
        currentWord = 0;
        bitCount = 0;
    }
};

class VLDWOPCompressor {
public:
    VLChannelState ch[2];

    std::vector<uint8_t> compressMono(const int16_t* in, uint32_t frameCount) {
        reset();
        VLBitWriter bw;
        int32_t d[5] = {};
        uint32_t a[5] = {2560, 2560, 2560, 2560, 2560};
        uint32_t j = 2;
        int32_t rbits = 0;

        for (uint32_t f = 0; f < frameCount; ++f)
            encodeChannel((int32_t)in[f] * 2, d, a, j, rbits, bw);

        return bw.finish();
    }

    std::vector<uint8_t> compressStereo(const int16_t* in, uint32_t frameCount) {
        reset();
        VLBitWriter bw;
        int32_t d[2][5] = {};
        uint32_t a[2][5] = {{2560, 2560, 2560, 2560, 2560},
                            {2560, 2560, 2560, 2560, 2560}};
        uint32_t j[2] = {2, 2};
        int32_t rbits[2] = {0, 0};

        for (uint32_t f = 0; f < frameCount; ++f) {
            const int32_t left2x = (int32_t)in[(size_t)f * 2] * 2;
            const int32_t right2x = (int32_t)in[(size_t)f * 2 + 1] * 2;
            encodeChannel(left2x, d[0], a[0], j[0], rbits[0], bw);
            encodeChannel(right2x - left2x, d[1], a[1], j[1], rbits[1], bw);
        }

        return bw.finish();
    }

private:
    void reset() {
        for (auto& c : ch) {
            std::fill(c.deltas, c.deltas + 5, 0);
            std::fill(c.averages, c.averages + 5, 2560);
        }
    }

    static uint32_t toCodeValue(int32_t signed2x) {
        if (signed2x >= 0) return (uint32_t)signed2x;
        return (uint32_t)(-signed2x - 1);
    }

    static int minAverageIndex(const uint32_t a[5]) {
        int idx = 0;
        for (int i = 1; i < 5; ++i)
            if (a[i] < a[idx]) idx = i;
        return idx;
    }

    static int32_t predictorResidual(int idx, int32_t sample2x, const int32_t d[5]) {
        switch (idx) {
            case 0: return sample2x;
            case 1: return sample2x - d[0];
            case 2: return sample2x - d[0] - d[1];
            case 3: return sample2x - d[0] - d[1] - d[2];
            case 4: return sample2x - d[0] - d[1] - d[2] - d[3];
            default: return sample2x; // LCOV_EXCL_LINE idx comes from a 0..4 minimum search.
        }
    }

    static int32_t applyPredictor(int idx, int32_t s2x, int32_t d[5]) {
        switch (idx) {
            case 0: {
                const int32_t t0 = s2x - d[0], t1 = t0 - d[1], t2 = t1 - d[2];
                d[4] = t2 - d[3]; d[3] = t2; d[2] = t1; d[1] = t0; d[0] = s2x;
                return s2x;
            }
            case 1: {
                const int32_t t1 = s2x - d[1], t2 = t1 - d[2], nd0 = d[0] + s2x;
                d[4] = t2 - d[3]; d[3] = t2; d[2] = t1; d[1] = s2x; d[0] = nd0;
                return nd0;
            }
            case 2: {
                const int32_t nd1 = d[1] + s2x, nd0 = d[0] + nd1, t = s2x - d[2];
                d[4] = t - d[3]; d[3] = t; d[2] = s2x; d[1] = nd1; d[0] = nd0;
                return nd0;
            }
            case 3: {
                const int32_t nd2 = d[2] + s2x, nd1 = d[1] + nd2, nd0 = d[0] + nd1;
                d[4] = s2x - d[3]; d[3] = s2x; d[2] = nd2; d[1] = nd1; d[0] = nd0;
                return nd0;
            }
            case 4: {
                const int32_t nd3 = d[3] + s2x, nd2 = d[2] + nd3,
                              nd1 = d[1] + nd2, nd0 = d[0] + nd1;
                d[4] = s2x; d[3] = nd3; d[2] = nd2; d[1] = nd1; d[0] = nd0;
                return nd0;
            }
            default: return d[0]; // LCOV_EXCL_LINE idx comes from a 0..4 minimum search.
        }
    }

    static void updateAverages(uint32_t a[5], const int32_t d[5]) {
        auto mag = [](int32_t v) -> uint32_t { return (uint32_t)(v ^ (v >> 31)); };
        for (int i = 0; i < 5; ++i)
            a[i] = a[i] + mag(d[i]) - (a[i] >> 5);
    }

    static void adjustJRbits(uint32_t step, uint32_t& j, int32_t& rbits) {
        if (step < j) {
            for (uint32_t jt = j >> 1; step < jt; jt >>= 1) { j = jt; --rbits; }
        } else {
            while (step >= j) {
                const uint32_t prev = j;
                j <<= 1; ++rbits;
                if (j <= prev) { j = prev; break; }
            }
        }
    }

    static bool encodeRemainder(uint32_t raw, uint32_t step,
                                uint32_t j, int32_t rbits,
                                uint32_t& remBits, bool& hasExtra, bool& extraBit) {
        if (rbits < 0 || rbits > 31) return false;
        const uint32_t limit = rbits == 31 ? 0x80000000u : (1u << rbits);
        const int32_t threshSigned = (int32_t)j - (int32_t)step;
        if (threshSigned < 0) return false;
        const uint32_t thresh = (uint32_t)threshSigned;

        if (raw < thresh) {
            if (raw >= limit) return false;
            remBits = raw;
            hasExtra = false;
            extraBit = false;
            return true;
        }

        const uint32_t folded = raw + thresh;
        remBits = folded >> 1;
        hasExtra = true;
        extraBit = (folded & 1u) != 0;
        return remBits >= thresh && remBits < limit;
    }

    static void writeCodeValue(uint32_t codeVal, uint32_t baseStep,
                               uint32_t& j, int32_t& rbits, VLBitWriter& bw) {
        uint32_t prefixSum = 0;
        uint32_t step = baseStep;
        int zerosWin = 7;

        for (uint32_t zeros = 0; zeros < 0x100000; ++zeros) {
            if (codeVal >= prefixSum) {
                uint32_t trialJ = j;
                int32_t trialRbits = rbits;
                adjustJRbits(step, trialJ, trialRbits);

                uint32_t remBits = 0;
                bool hasExtra = false;
                bool extraBit = false;
                if (encodeRemainder(codeVal - prefixSum, step, trialJ, trialRbits,
                                    remBits, hasExtra, extraBit)) {
                    for (uint32_t i = 0; i < zeros; ++i) bw.writeBit(false);
                    bw.writeBit(true);
                    if (trialRbits > 0) bw.writeBits(remBits, trialRbits);
                    if (hasExtra) bw.writeBit(extraBit);
                    j = trialJ;
                    rbits = trialRbits;
                    return;
                }
            }

            if (baseStep == 0) break;
            if (UINT32_MAX - prefixSum < step) break;
            prefixSum += step;
            if (prefixSum > codeVal && step != 0) break;
            if (--zerosWin == 0) {
                step <<= 2;
                zerosWin = 7;
            }
        }

        bw.writeBit(true); // LCOV_EXCL_LINE emergency fallback; valid sample residuals encode above.
    }

    static void encodeChannel(int32_t sample2x, int32_t d[5], uint32_t a[5],
                              uint32_t& j, int32_t& rbits, VLBitWriter& bw) {
        const int idx = minAverageIndex(a);
        const uint32_t baseStep = ((a[idx] * 3u) + 36u) >> 7;
        const int32_t residual = predictorResidual(idx, sample2x, d);
        writeCodeValue(toCodeValue(residual), baseStep, j, rbits, bw);
        applyPredictor(idx, residual, d);
        updateAverages(a, d);
    }
};

class VLIFFWriter {
public:
    std::vector<uint8_t> data;

    size_t beginChunk(const char id[4]) {
        const size_t start = data.size();
        data.insert(data.end(), id, id + 4);
        put32(0);
        return start;
    }

    size_t beginCat(const char type[4]) {
        const size_t start = beginChunk("CAT ");
        data.insert(data.end(), type, type + 4);
        return start;
    }

    void endChunk(size_t start) {
        const size_t payloadStart = start + 8;
        const uint32_t size = (uint32_t)(data.size() - payloadStart);
        data[start + 4] = (uint8_t)(size >> 24);
        data[start + 5] = (uint8_t)(size >> 16);
        data[start + 6] = (uint8_t)(size >> 8);
        data[start + 7] = (uint8_t)size;
        if (data.size() & 1u) data.push_back(0);
    }

    void put8(uint8_t v) { data.push_back(v); }
    void put16(uint16_t v) {
        data.push_back((uint8_t)(v >> 8));
        data.push_back((uint8_t)v);
    }
    void put32(uint32_t v) {
        data.push_back((uint8_t)(v >> 24));
        data.push_back((uint8_t)(v >> 16));
        data.push_back((uint8_t)(v >> 8));
        data.push_back((uint8_t)v);
    }
    void putBytes(const uint8_t* p, size_t n) { data.insert(data.end(), p, p + n); }
};

/* -----------------------------------------------------------------------
   REX2 file parser + DWOP decompressor wrapper
   ----------------------------------------------------------------------- */

enum VLSliceState { kSliceNormal = 1, kSliceMuted = 2, kSliceLocked = 3 };

struct VLSliceEntry {
    uint32_t     ppq_pos        = 0;
    uint32_t     sample_length  = 0;
    uint32_t     rendered_length= 0;
    uint32_t     sample_start   = 0;
    uint16_t     points         = 0x7fff;
    uint8_t      selected_flag  = 0;
    VLSliceState state          = kSliceNormal;
    bool         synthetic_leading = false;
};

class VLFileImpl {
public:
    VLFileInfo              info      = {};
    VLCreatorInfo           creator   = {};
    std::vector<VLSliceEntry> slices;
    std::vector<uint8_t>    fileData;
    std::vector<int16_t>    pcm;
    uint32_t                totalFrames         = 0;
    uint32_t                loopStart           = 0;
    uint32_t                loopEnd             = 0;
    bool                    transientEnabled    = true;
    uint16_t                transientAttack     = 0x15;
    uint16_t                transientDecay      = 0x3ff;
    uint16_t                transientStretch    = 0x28;
    uint16_t                processingGain      = 1000;
    bool                    silenceSelected     = false;
    bool                    headerValid         = true;

    static constexpr int32_t kREXPPQ = 15360;

    bool loadFromBuffer(const char* buf, size_t size) {
        fileData.assign((const uint8_t*)buf, (const uint8_t*)buf + size);

        info.channels     = 1;
        info.sample_rate  = 44100;
        info.slice_count  = 0;
        info.tempo        = 120000;
        info.original_tempo = 120000;
        info.ppq_length   = 61440;
        info.time_sig_num = 4;
        info.time_sig_den = 4;
        info.bit_depth    = 16;
        info.total_frames = 0;
        info.loop_start = 0;
        info.loop_end = 0;
        info.processing_gain = processingGain;
        info.transient_enabled = transientEnabled ? 1 : 0;
        info.transient_attack = transientAttack;
        info.transient_decay = transientDecay;
        info.transient_stretch = transientStretch;
        info.silence_selected = silenceSelected ? 1 : 0;
        headerValid = true;
        std::memset(&creator, 0, sizeof(creator));

        if (fileData.size() < 12) return false;
        if (fileData[0]!='C'||fileData[1]!='A'||fileData[2]!='T'||fileData[3]!=' ')
            return false;

        size_t dwopOffset = 0, dwopSize = 0;
        bool   hasDWOP    = false;

        parseIFF(8 + 4, fileData.size(), dwopOffset, dwopSize, hasDWOP);
        finalizeSlices();

        if (!headerValid) return false;
        if (!hasDWOP || dwopSize == 0) return false;
        if (dwopOffset + dwopSize > fileData.size()) return false;
        if (totalFrames == 0) return false;

        pcm.assign((size_t)totalFrames * (size_t)info.channels, 0);

        VLDWOPDecompressor dec;
        dec.init(&fileData[dwopOffset], dwopSize);

        uint32_t done = 0;
        bool ok = true;
        while (done < totalFrames && ok) {
            const uint32_t chunk = std::min<uint32_t>(0x100000, totalFrames - done);
            if (info.channels == 1)
                ok = dec.decompressMono(chunk, &pcm[done]);
            else
                ok = dec.decompressStereo(chunk, &pcm[(size_t)done * 2]);
            done += chunk;
        }
        if (!ok) return false;
        finalizeRenderedLengths();
        return true;
    }

private:
    static uint32_t be32(const uint8_t* p) {
        return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
    }
    static uint16_t be16(const uint8_t* p) {
        return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);
    }

    void parseIFF(size_t start, size_t end,
                  size_t& dwopOffset, size_t& dwopSize, bool& hasDWOP) {
        size_t off = start;
        while (off + 8 < end && off + 8 < fileData.size()) {
            char id[5] = {};
            std::memcpy(id, &fileData[off], 4);
            off += 4;
            const uint32_t sz = be32(&fileData[off]);
            off += 4;
            if (off + sz > fileData.size()) break;

            if      (std::strcmp(id, "HEAD") == 0) parseHEAD(&fileData[off], sz);
            else if (std::strcmp(id, "CREI") == 0) parseCREI(&fileData[off], sz);
            else if (std::strcmp(id, "SINF") == 0) parseSINF(&fileData[off], sz);
            else if (std::strcmp(id, "GLOB") == 0) parseGLOB(&fileData[off], sz);
            else if (std::strcmp(id, "TRSH") == 0) parseTRSH(&fileData[off], sz);
            else if (std::strcmp(id, "RECY") == 0) parseRECY(&fileData[off], sz);
            else if (std::strcmp(id, "SLCE") == 0) parseSLCE(&fileData[off], sz);
            else if ((std::strcmp(id, "SDAT") == 0 || std::strcmp(id, "DWOP") == 0) && !hasDWOP) {
                dwopOffset = off;
                dwopSize   = sz;
                hasDWOP    = true;
            } else if (std::strcmp(id, "CAT ") == 0 && sz >= 4) {
                parseIFF(off + 4, off + sz, dwopOffset, dwopSize, hasDWOP);
            }

            off += sz;
            if (off & 1) ++off;
        }
    }

    void parseSINF(const uint8_t* d, uint32_t sz) {
        if (sz < 18) return;
        info.channels    = d[0];
        const uint8_t bd = d[1];
        info.sample_rate = (int32_t)be32(d + 2);
        totalFrames      = be32(d + 6);
        loopStart        = be32(d + 10);
        loopEnd          = be32(d + 14);
        info.total_frames = (int32_t)totalFrames;
        info.loop_start   = (int32_t)loopStart;
        info.loop_end     = (int32_t)loopEnd;

        switch (bd) {
            case 1: info.bit_depth =  8; break;
            case 3: info.bit_depth = 16; break;
            case 5: info.bit_depth = 24; break;
            case 7: info.bit_depth = 32; break;
            default: info.bit_depth = 16; break;
        }

        const uint32_t frames = (loopEnd > loopStart) ? (loopEnd - loopStart) : totalFrames;
        if (frames > 0 && info.sample_rate > 0 && info.ppq_length > 0) {
            const double beats = (double)info.ppq_length / (double)kREXPPQ;
            const double bpm   = beats * 60.0 * (double)info.sample_rate / (double)frames;
            const int32_t t    = (int32_t)std::lround(bpm * 1000.0);
            if (t > 0) info.original_tempo = t;
        }
        if (info.original_tempo == 0) info.original_tempo = info.tempo;
    }

    void parseHEAD(const uint8_t* d, uint32_t sz) {
        if (sz < 6 || be32(d) != 0x490cf18du) {
            headerValid = false;
            return;
        }

        if (d[4] != 0xbc || d[5] > 0x03) {
            headerValid = false;
        }
    }

    void parseGLOB(const uint8_t* d, uint32_t sz) {
        if (sz < 22) return;
        info.slice_count  = (int32_t)be32(d);
        info.time_sig_num = d[7];
        info.time_sig_den = d[8];
        info.tempo        = (int32_t)be32(d + 16);
        info.ppq_length   = 61440;
        processingGain    = be16(d + 12);
        silenceSelected   = d[21] != 0;
        info.processing_gain = processingGain;
        info.silence_selected = silenceSelected ? 1 : 0;
    }

    void parseCREI(const uint8_t* d, uint32_t sz) {
        uint32_t off = 0;
        auto readString = [&](char* dst, size_t dstSize) {
            if (!dst || dstSize == 0) return;
            dst[0] = '\0';
            if (off + 4 > sz) return;

            const uint32_t n = be32(d + off);
            off += 4;
            if (off + n > sz) {
                off = sz;
                return;
            }

            const size_t copy = std::min<size_t>(n, dstSize - 1);
            if (copy) std::memcpy(dst, d + off, copy);
            dst[copy] = '\0';
            off += n;
        };

        readString(creator.name, sizeof(creator.name));
        readString(creator.copyright, sizeof(creator.copyright));
        readString(creator.url, sizeof(creator.url));
        readString(creator.email, sizeof(creator.email));
        readString(creator.free_text, sizeof(creator.free_text));
    }

    void parseTRSH(const uint8_t* d, uint32_t sz) {
        if (sz < 7) return;
        transientEnabled = d[0] != 0;
        transientAttack = be16(d + 1);
        transientDecay = be16(d + 3);
        transientStretch = be16(d + 5);
        info.transient_enabled = transientEnabled ? 1 : 0;
        info.transient_attack = transientAttack;
        info.transient_decay = transientDecay;
        info.transient_stretch = transientStretch;
    }

    void parseRECY(const uint8_t* d, uint32_t sz) {
        if (sz < 12) return;
        const int32_t t = (int32_t)be32(d + 8);
        if (t > 0) info.original_tempo = t;
    }

    void parseSLCE(const uint8_t* d, uint32_t sz) {
        if (sz < 8) return;
        VLSliceEntry s;
        s.sample_start  = be32(d);
        s.sample_length = be32(d + 4);
        s.points        = be16(d + 8);
        const uint8_t flags = sz > 10 ? d[10] : 0;
        s.selected_flag = (flags & 0x04) ? 1 : 0;
        if      (flags & 0x02) s.state = kSliceLocked;
        else if (flags & 0x01) s.state = kSliceMuted;
        else                   s.state = kSliceNormal;
        slices.push_back(s);
        info.slice_count = (int32_t)slices.size();
    }

    void finalizeSlices() {
        const uint32_t denom = (loopEnd > loopStart) ? (loopEnd - loopStart)
                             : (totalFrames ? totalFrames : 1u);

        std::sort(slices.begin(), slices.end(),
                  [](const VLSliceEntry& a, const VLSliceEntry& b){
                      return a.sample_start < b.sample_start; });

        std::vector<VLSliceEntry> out;
        for (auto s : slices) {
            if (s.sample_length <= 1) continue;
            const uint32_t rel = (s.sample_start > loopStart) ? (s.sample_start - loopStart) : 0;
            s.ppq_pos = (uint32_t)(((uint64_t)rel * info.ppq_length + denom / 2) / denom);
            out.push_back(s);
        }

        if (!out.empty() && loopStart > 0 && loopEnd > loopStart
            && out.front().sample_start > loopStart
            && out.front().sample_start <= totalFrames) {
            VLSliceEntry leading;
            leading.ppq_pos = 0;
            leading.sample_start = loopStart;
            leading.sample_length = out.front().sample_start - loopStart;
            leading.points = 0x7fff;
            leading.selected_flag = 1;
            leading.state = kSliceNormal;
            leading.synthetic_leading = true;
            out.insert(out.begin(), leading);
        }

        slices.swap(out);
        info.slice_count = (int32_t)slices.size();
    }

public:
    struct SegLoop {
        uint32_t start = 0;
        uint32_t end = 0;
        float volumeCompensation = 1.0f;
    };

    SegLoop findSegmentLoop(uint32_t start, uint32_t end) const {
        SegLoop r{start, end};
        const uint32_t sr  = info.sample_rate ? (uint32_t)info.sample_rate : 44100u;
        const uint32_t srch = std::max(1u, (400u * sr) / 44100u);
        const uint32_t mhl  = std::max(1u, (20000u * sr) / 44100u);
        if (end <= start || end - start < srch * 3u) return r;

        const uint32_t ch = std::max(1, info.channels);
        auto leftAbs = [&](uint32_t f) -> int {
            const size_t i = (size_t)f * ch;
            return i < pcm.size() ? std::abs((int)pcm[i]) : 0;
        };

        uint32_t loopEnd = end - srch;
        int      peak    = -1;
        for (uint32_t i = 0, f = end - srch; i < srch && f > start; ++i, --f) {
            const int p = leftAbs(f);
            if (p > peak) { peak = p; loopEnd = f; }
        }

        uint32_t hl    = std::min((loopEnd - start) / 2u, mhl);
        uint32_t ls    = loopEnd - hl;
        uint32_t loopStart = ls;
        int      lspeak = -1;
        for (uint32_t i = 0, f = ls; i < srch && f >= start; ++i) {
            const int p = leftAbs(f);
            if (p > lspeak) { lspeak = p; loopStart = f; }
            if (f == 0) break; --f;
        }

        r.start = std::clamp(loopStart, start, end - 1u);
        r.end   = std::clamp(loopEnd,   r.start + 1u, end);
        if (lspeak > 0 && peak > 0)
            r.volumeCompensation = std::min(10.0f, (float)peak / (float)lspeak);
        return r;
    }

private:
    uint32_t calcRenderedLength(const VLSliceEntry& s) const {
        const uint32_t start = s.sample_start;
        uint32_t       end   = std::min(start + s.sample_length, totalFrames);
        if (loopEnd > loopStart && start < loopEnd)
            end = std::min(end, loopEnd);
        if (end <= start) return 1u;

        const uint32_t segLen    = end - start;
        const uint32_t loopE     = findSegmentLoop(start, end).end;
        const uint32_t stretchN  = transientEnabled ? (uint32_t)transientStretch + 1u : 1u;
        const uint32_t stretchT  = (uint32_t)(((uint64_t)(loopE - start) * stretchN) / 100u);
        return std::max(1u, segLen + stretchT);
    }

public:
    void finalizeRenderedLengths() {
        for (auto& s : slices)
            s.rendered_length = calcRenderedLength(s);
    }
};

static int16_t floatToS16(float s) {
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    const float scaled = s >= 0.0f ? s * 32767.0f : s * 32768.0f;
    return (int16_t)std::lround(scaled);
}

static uint8_t bitDepthCode(int32_t bitDepth) {
    switch (bitDepth) {
        case 8:  return 1; // LCOV_EXCL_LINE saves normalize authored audio to 16-bit PCM.
        case 16: return 3;
        case 24: return 5; // LCOV_EXCL_LINE saves normalize authored audio to 16-bit PCM.
        case 32: return 7; // LCOV_EXCL_LINE saves normalize authored audio to 16-bit PCM.
        default: return 3; // LCOV_EXCL_LINE saves normalize authored audio to 16-bit PCM.
    }
}

static bool hasCreatorInfo(const VLCreatorInfo& c) {
    return c.name[0] || c.copyright[0] || c.url[0] || c.email[0] || c.free_text[0];
}

static void writeCStringChunkString(VLIFFWriter& w, const char* s) {
    const size_t n = s ? std::min<size_t>(std::strlen(s), 255) : 0;
    w.put32((uint32_t)n);
    if (n) w.putBytes((const uint8_t*)s, n);
}

static void writeSimpleChunk(VLIFFWriter& w, const char id[4],
                             const std::vector<uint8_t>& payload) {
    const size_t c = w.beginChunk(id);
    if (!payload.empty()) w.putBytes(payload.data(), payload.size());
    w.endChunk(c);
}

static uint32_t calcSampleStartFromPPQ(const VLFileImpl& impl, uint32_t ppq) {
    const VLFileInfo& info = impl.info;
    if (info.ppq_length > 0 && info.loop_end > info.loop_start) {
        const uint32_t denom = (uint32_t)(info.loop_end - info.loop_start);
        return (uint32_t)info.loop_start
             + (uint32_t)(((uint64_t)ppq * denom + (uint32_t)info.ppq_length / 2u)
                          / (uint32_t)info.ppq_length);
    }

    const uint32_t tempo = info.tempo > 0 ? (uint32_t)info.tempo : 120000u;
    const uint32_t sr = info.sample_rate > 0 ? (uint32_t)info.sample_rate : 44100u;
    return (uint32_t)(((uint64_t)ppq * sr * 60000u + (uint64_t)tempo * VLFileImpl::kREXPPQ / 2u)
                      / ((uint64_t)tempo * VLFileImpl::kREXPPQ));
}

static void normaliseInfoForSave(VLFileImpl& impl) {
    impl.info.channels = std::clamp(impl.info.channels, 1, 2);
    if (impl.info.sample_rate <= 0) impl.info.sample_rate = 44100;
    if (impl.info.tempo <= 0) impl.info.tempo = 120000;
    if (impl.info.original_tempo <= 0) impl.info.original_tempo = impl.info.tempo;
    if (impl.info.time_sig_num <= 0) impl.info.time_sig_num = 4;
    if (impl.info.time_sig_den <= 0) impl.info.time_sig_den = 4;
    impl.info.bit_depth = 16;
    impl.info.slice_count = (int32_t)impl.slices.size();

    impl.totalFrames = (uint32_t)(impl.pcm.size() / (size_t)impl.info.channels);
    impl.info.total_frames = (int32_t)impl.totalFrames;

    if (impl.info.loop_start < 0 || (uint32_t)impl.info.loop_start >= impl.totalFrames)
        impl.info.loop_start = 0;
    if (impl.info.loop_end <= impl.info.loop_start || (uint32_t)impl.info.loop_end > impl.totalFrames)
        impl.info.loop_end = (int32_t)impl.totalFrames;

    impl.loopStart = (uint32_t)std::max(0, impl.info.loop_start);
    impl.loopEnd = (uint32_t)std::max(impl.info.loop_start, impl.info.loop_end);

    if (impl.info.ppq_length <= 0) {
        const uint32_t frames = impl.loopEnd > impl.loopStart ? impl.loopEnd - impl.loopStart : impl.totalFrames;
        const double beats = frames > 0
            ? ((double)frames * (double)impl.info.tempo)
              / (60000.0 * (double)impl.info.sample_rate)
            : 4.0;
        impl.info.ppq_length = std::max(1, (int32_t)std::lround(beats * VLFileImpl::kREXPPQ));
    }

    impl.processingGain = (uint16_t)std::clamp(impl.info.processing_gain > 0 ? impl.info.processing_gain : 1000, 0, 1000);
    impl.transientEnabled = impl.info.transient_enabled != 0;
    impl.transientAttack = (uint16_t)std::clamp(impl.info.transient_attack, 0, 1023);
    impl.transientDecay = (uint16_t)std::clamp(impl.info.transient_decay > 0 ? impl.info.transient_decay : 1023, 0, 1023);
    impl.transientStretch = (uint16_t)std::clamp(impl.info.transient_stretch, 0, 100);
    impl.silenceSelected = impl.info.silence_selected != 0;
    impl.finalizeRenderedLengths();
}

static std::vector<uint8_t> buildREX2File(VLFileImpl& impl) {
    normaliseInfoForSave(impl);

    VLDWOPCompressor comp;
    const std::vector<uint8_t> sdat = impl.info.channels == 2
        ? comp.compressStereo(impl.pcm.data(), impl.totalFrames)
        : comp.compressMono(impl.pcm.data(), impl.totalFrames);

    VLIFFWriter w;
    const size_t root = w.beginCat("REX2");

    {
        const size_t c = w.beginChunk("HEAD");
        const uint8_t head[] = {
            0x49, 0x0c, 0xf1, 0x8d, 0xbc, 0x02, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00
        };
        w.putBytes(head, sizeof(head));
        w.endChunk(c);
    }

    if (hasCreatorInfo(impl.creator)) {
        const size_t c = w.beginChunk("CREI");
        writeCStringChunkString(w, impl.creator.name);
        writeCStringChunkString(w, impl.creator.copyright);
        writeCStringChunkString(w, impl.creator.url);
        writeCStringChunkString(w, impl.creator.email);
        writeCStringChunkString(w, impl.creator.free_text);
        w.endChunk(c);
    }

    {
        const size_t c = w.beginChunk("GLOB");
        w.put32((uint32_t)impl.slices.size());
        w.put8(0);
        w.put8(1);
        w.put8(0);
        w.put8((uint8_t)impl.info.time_sig_num);
        w.put8((uint8_t)impl.info.time_sig_den);
        w.put8(0x4e);
        w.put8(0);
        w.put8(0);
        w.put16(impl.processingGain);
        w.put16(1);
        w.put32((uint32_t)impl.info.tempo);
        w.put8(1);
        w.put8(impl.silenceSelected ? 1 : 0);
        w.endChunk(c);
    }

    {
        const size_t c = w.beginChunk("RECY");
        w.put8(0xbc);
        w.put8(0x02);
        w.put8(0);
        w.put8(0);
        w.put8(0);
        w.put8(1);
        w.put8(0);
        w.put8(0);
        w.put32((uint32_t)impl.info.original_tempo);
        w.put16(0);
        w.put8(8);
        w.endChunk(c);
    }

    {
        const size_t devl = w.beginCat("DEVL");
        const size_t trsh = w.beginChunk("TRSH");
        w.put8(impl.transientEnabled ? 1 : 0);
        w.put16(impl.transientAttack);
        w.put16(impl.transientDecay);
        w.put16(impl.transientStretch);
        w.endChunk(trsh);

        const size_t eq = w.beginChunk("EQ  ");
        const uint8_t eqPayload[] = {
            0x00, 0x00, 0x0f, 0x00, 0x64, 0x00, 0x00, 0x03, 0xe8,
            0x09, 0xc4, 0x00, 0x00, 0x03, 0xe8, 0x4e, 0x20
        };
        w.putBytes(eqPayload, sizeof(eqPayload));
        w.endChunk(eq);

        const size_t comp = w.beginChunk("COMP");
        const uint8_t compPayload[] = {
            0x00, 0x00, 0x4d, 0x00, 0x27, 0x00, 0x42, 0x00, 0x38
        };
        w.putBytes(compPayload, sizeof(compPayload));
        w.endChunk(comp);

        w.endChunk(devl);
    }

    {
        const size_t slcl = w.beginCat("SLCL");
        std::vector<VLSliceEntry> sorted = impl.slices;
        std::sort(sorted.begin(), sorted.end(),
                  [](const VLSliceEntry& a, const VLSliceEntry& b) {
                      return a.sample_start < b.sample_start;
                  });
        for (const auto& s : sorted) {
            const size_t c = w.beginChunk("SLCE");
            w.put32(s.sample_start);
            w.put32(std::max<uint32_t>(1, s.sample_length));
            w.put16(s.points);
            uint8_t flags = 0;
            if (s.state == kSliceMuted) flags |= 0x01;
            else if (s.state == kSliceLocked) flags |= 0x02;
            if (s.selected_flag) flags |= 0x04;
            w.put8(flags);
            w.endChunk(c);
        }
        w.endChunk(slcl);
    }

    {
        const size_t c = w.beginChunk("SINF");
        w.put8((uint8_t)impl.info.channels);
        w.put8(bitDepthCode(impl.info.bit_depth));
        w.put32((uint32_t)impl.info.sample_rate);
        w.put32(impl.totalFrames);
        w.put32(impl.loopStart);
        w.put32(impl.loopEnd);
        w.endChunk(c);
    }

    writeSimpleChunk(w, "SDAT", sdat);
    w.endChunk(root);
    return w.data;
}

} // anonymous namespace

/* -----------------------------------------------------------------------
   VLFile_s  —  the opaque handle
   ----------------------------------------------------------------------- */

struct VLFile_s {
    VLFileImpl impl;
    int32_t    outputSampleRate = 44100;
    bool       isNew            = false;
    bool       dirty            = false;
};

/* -----------------------------------------------------------------------
   Open / close
   ----------------------------------------------------------------------- */

VLFile vl_open(const char* path, VLError* err) {
    auto set = [&](VLError e){ if (err) *err = e; };
    if (!path) { set(VL_ERROR_INVALID_ARG); return nullptr; }

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { set(VL_ERROR_FILE_NOT_FOUND); return nullptr; }

    const std::streamsize sz = f.tellg();
    f.seekg(0);
    std::vector<char> buf((size_t)sz);
    if (!f.read(buf.data(), sz)) { set(VL_ERROR_FILE_CORRUPT); return nullptr; }

    return vl_open_from_memory(buf.data(), (size_t)sz, err);
}

VLFile vl_open_from_memory(const void* data, size_t size, VLError* err) {
    auto set = [&](VLError e){ if (err) *err = e; };
    if (!data || size == 0) { set(VL_ERROR_INVALID_ARG); return nullptr; }

    VLFile_s* h = new (std::nothrow) VLFile_s();
    if (!h) { set(VL_ERROR_OUT_OF_MEMORY); return nullptr; }

    if (!h->impl.loadFromBuffer((const char*)data, size)) {
        delete h; set(VL_ERROR_FILE_CORRUPT); return nullptr;
    }

    h->outputSampleRate = h->impl.info.sample_rate ? h->impl.info.sample_rate : 44100;
    set(VL_OK);
    return h;
}

VLFile vl_create_new(int32_t channels, int32_t sample_rate, int32_t tempo, VLError* err) {
    auto set = [&](VLError e){ if (err) *err = e; };
    if (channels != 1 && channels != 2) { set(VL_ERROR_INVALID_ARG); return nullptr; }
    if (sample_rate < 8000 || sample_rate > 192000) { set(VL_ERROR_INVALID_SAMPLE_RATE); return nullptr; }
    if (tempo <= 0) { set(VL_ERROR_INVALID_ARG); return nullptr; }

    VLFile_s* h = new (std::nothrow) VLFile_s();
    if (!h) { set(VL_ERROR_OUT_OF_MEMORY); return nullptr; }
    h->isNew            = true;
    h->outputSampleRate = sample_rate;

    h->impl.info.channels = channels;
    h->impl.info.sample_rate = sample_rate;
    h->impl.info.slice_count = 0;
    h->impl.info.tempo = tempo;
    h->impl.info.original_tempo = tempo;
    h->impl.info.ppq_length = VLFileImpl::kREXPPQ * 4;
    h->impl.info.time_sig_num = 4;
    h->impl.info.time_sig_den = 4;
    h->impl.info.bit_depth = 16;
    h->impl.info.total_frames = 0;
    h->impl.info.loop_start = 0;
    h->impl.info.loop_end = 0;
    h->impl.info.processing_gain = 1000;
    h->impl.info.transient_enabled = 1;
    h->impl.info.transient_attack = 0;
    h->impl.info.transient_decay = 1023;
    h->impl.info.transient_stretch = 0;
    h->impl.info.silence_selected = 0;
    h->impl.processingGain = 1000;
    h->impl.transientEnabled = true;
    h->impl.transientAttack = 0;
    h->impl.transientDecay = 1023;
    h->impl.transientStretch = 0;
    h->impl.silenceSelected = false;

    set(VL_OK);
    return h;
}

void vl_close(VLFile file) { delete file; }

/* -----------------------------------------------------------------------
   Read: metadata
   ----------------------------------------------------------------------- */

VLError vl_get_info(VLFile file, VLFileInfo* out) {
    if (!file) return VL_ERROR_INVALID_HANDLE;
    if (!out)  return VL_ERROR_INVALID_ARG;
    *out = file->impl.info;
    return VL_OK;
}

VLError vl_get_creator_info(VLFile file, VLCreatorInfo* out) {
    if (!file) return VL_ERROR_INVALID_HANDLE;
    if (!out)  return VL_ERROR_INVALID_ARG;
    const VLCreatorInfo& src = file->impl.creator;
    if (!src.name[0] && !src.copyright[0] && !src.url[0] && !src.email[0] && !src.free_text[0])
        return VL_ERROR_NO_CREATOR_INFO;
    *out = src;
    return VL_OK;
}

/* -----------------------------------------------------------------------
   Read: slice enumeration
   ----------------------------------------------------------------------- */

VLError vl_get_slice_info(VLFile file, int32_t index, VLSliceInfo* out) {
    if (!file) return VL_ERROR_INVALID_HANDLE;
    if (!out)  return VL_ERROR_INVALID_ARG;
    if (index < 0 || (size_t)index >= file->impl.slices.size()) return VL_ERROR_INVALID_SLICE;
    const VLSliceEntry& s = file->impl.slices[(size_t)index];
    out->ppq_pos      = (int32_t)s.ppq_pos;
    out->sample_length= (int32_t)s.sample_length;
    out->sample_start = (int32_t)s.sample_start;
    return VL_OK;
}

/* -----------------------------------------------------------------------
   Read: sample extraction
   ----------------------------------------------------------------------- */

VLError vl_set_output_sample_rate(VLFile file, int32_t rate) {
    if (!file) return VL_ERROR_INVALID_HANDLE;
    if (rate < 8000 || rate > 192000) return VL_ERROR_INVALID_SAMPLE_RATE;
    if (rate != file->impl.info.sample_rate) return VL_ERROR_NOT_IMPLEMENTED;
    file->outputSampleRate = rate;
    return VL_OK;
}

int32_t vl_get_slice_frame_count(VLFile file, int32_t index) {
    if (!file) return (int32_t)VL_ERROR_INVALID_HANDLE;
    if (index < 0 || (size_t)index >= file->impl.slices.size())
        return (int32_t)VL_ERROR_INVALID_SLICE;
    return (int32_t)file->impl.slices[(size_t)index].rendered_length;
}

VLError vl_decode_slice(VLFile file, int32_t index,
                         float* left, float* right,
                         int32_t capacity, int32_t* frames_out) {
    if (!file)  return VL_ERROR_INVALID_HANDLE;
    if (!left)  return VL_ERROR_INVALID_ARG;
    if (index < 0 || (size_t)index >= file->impl.slices.size()) return VL_ERROR_INVALID_SLICE;

    const VLSliceEntry& s      = file->impl.slices[(size_t)index];
    const int32_t       needed = (int32_t)s.rendered_length;
    if (capacity < needed) return VL_ERROR_BUFFER_TOO_SMALL;

    const auto& pcm = file->impl.pcm;
    if (pcm.empty()) return VL_ERROR_FILE_CORRUPT;

    const uint32_t requestedFrames = (uint32_t)needed;
    uint32_t sourceStart = s.sample_start;
    if (sourceStart >= file->impl.totalFrames) {
        std::fill(left, left + needed, 0.f);
        if (right) std::fill(right, right + needed, 0.f);
        if (frames_out) *frames_out = needed;
        return VL_OK;
    }

    uint32_t sourceFrames = s.sample_length;
    if (file->impl.loopEnd > file->impl.loopStart && sourceStart < file->impl.loopEnd)
        sourceFrames = std::min<uint32_t>(sourceFrames, file->impl.loopEnd - sourceStart);
    sourceFrames = std::min<uint32_t>(sourceFrames, file->impl.totalFrames - sourceStart);

    const uint32_t sourceEnd = sourceStart + sourceFrames;
    const auto segmentLoop = file->impl.findSegmentLoop(sourceStart, sourceEnd);
    const int32_t ch = std::max(1, file->impl.info.channels);
    const float samplerGain = (float)file->impl.processingGain * 0.000833333354f;

    uint32_t samplePos = std::min(sourceStart + 2u, sourceEnd);
    int loopPhase = 0; // 0: forward source, 1: forward loop, 2: backward loop
    bool stretchPhase = false;
    float stretchEnv = 1.0f;
    const uint32_t stretchFrameCount = std::max<uint32_t>(
        1u,
        sourceEnd - segmentLoop.end + (requestedFrames > sourceFrames ? requestedFrames - sourceFrames : 0u));
    const float stretchEnvDec = 1.0f / (float)stretchFrameCount;
    float loopLevelComp = 1.0f;
    const float loopLevelCompInc = (segmentLoop.end > segmentLoop.start)
        ? (1.0f - segmentLoop.volumeCompensation) / (float)(segmentLoop.end - segmentLoop.start)
        : 0.0f;

    for (uint32_t f = 0; f < requestedFrames; ++f) {
        const size_t src = (size_t)samplePos * (size_t)ch;
        float l = 0.f;
        float r = 0.f;
        if (src < pcm.size()) {
            const float level = samplerGain * stretchEnv * loopLevelComp;
            l = ((float)pcm[src] / 32768.f) * level;
            r = (ch >= 2 && src + 1 < pcm.size())
                ? ((float)pcm[src + 1] / 32768.f) * level
                : l;
        }

        left[f] = l;
        if (right) right[f] = r;

        if (stretchPhase) {
            stretchEnv = std::max(0.0f, stretchEnv - stretchEnvDec);
            if (loopPhase == 1)
                loopLevelComp += loopLevelCompInc;
            else if (loopPhase == 2)
                loopLevelComp -= loopLevelCompInc;
        }

        if (loopPhase <= 1) {
            ++samplePos;
            if (samplePos >= segmentLoop.end) {
                stretchPhase = true;
                loopPhase = 2;
                if (samplePos > 0) --samplePos;
                if (samplePos <= segmentLoop.start) loopPhase = 1;
            }
        } else {
            if (samplePos > 0) --samplePos;
            if (samplePos <= segmentLoop.start) loopPhase = 1;
        }
    }

    if (frames_out) *frames_out = needed;
    return VL_OK;
}

/* -----------------------------------------------------------------------
   Write: assembly from audio slices
   ----------------------------------------------------------------------- */

VLError vl_set_info(VLFile file, const VLFileInfo* info) {
    if (!file) return VL_ERROR_INVALID_HANDLE;
    if (!info) return VL_ERROR_INVALID_ARG;
    if (!file->impl.slices.empty() || !file->impl.pcm.empty())
        return VL_ERROR_ALREADY_HAS_DATA;
    if (info->channels != 1 && info->channels != 2) return VL_ERROR_INVALID_ARG;
    if (info->sample_rate < 8000 || info->sample_rate > 192000)
        return VL_ERROR_INVALID_SAMPLE_RATE;
    if (info->tempo <= 0) return VL_ERROR_INVALID_ARG;

    file->impl.info = *info;
    file->impl.info.channels = info->channels;
    file->impl.info.sample_rate = info->sample_rate;
    file->impl.info.bit_depth = 16;
    file->impl.processingGain = (uint16_t)std::clamp(info->processing_gain > 0 ? info->processing_gain : 1000, 0, 1000);
    file->impl.transientEnabled = info->transient_enabled != 0;
    file->impl.transientAttack = (uint16_t)std::clamp(info->transient_attack, 0, 1023);
    file->impl.transientDecay = (uint16_t)std::clamp(info->transient_decay > 0 ? info->transient_decay : 1023, 0, 1023);
    file->impl.transientStretch = (uint16_t)std::clamp(info->transient_stretch, 0, 100);
    file->impl.silenceSelected = info->silence_selected != 0;
    file->outputSampleRate = info->sample_rate;
    file->dirty = true;
    return VL_OK;
}

VLError vl_set_creator_info(VLFile file, const VLCreatorInfo* info) {
    if (!file) return VL_ERROR_INVALID_HANDLE;
    if (!info) return VL_ERROR_INVALID_ARG;
    if (!file->impl.slices.empty() || !file->impl.pcm.empty())
        return VL_ERROR_ALREADY_HAS_DATA;
    file->impl.creator = *info;
    file->impl.creator.name[sizeof(file->impl.creator.name) - 1] = '\0';
    file->impl.creator.copyright[sizeof(file->impl.creator.copyright) - 1] = '\0';
    file->impl.creator.url[sizeof(file->impl.creator.url) - 1] = '\0';
    file->impl.creator.email[sizeof(file->impl.creator.email) - 1] = '\0';
    file->impl.creator.free_text[sizeof(file->impl.creator.free_text) - 1] = '\0';
    file->dirty = true;
    return VL_OK;
}

static int32_t addSliceAtSample(VLFile file, uint32_t sample_start, int32_t ppq_pos,
                                const float* left, const float* right,
                                int32_t frames) {
    if (!file) return (int32_t)VL_ERROR_INVALID_HANDLE;
    if (!left || frames <= 0 || ppq_pos < 0) return (int32_t)VL_ERROR_INVALID_ARG;
    const int32_t channels = std::clamp(file->impl.info.channels, 1, 2);
    if (channels == 2 && !right) return (int32_t)VL_ERROR_INVALID_ARG;

    const uint32_t end = sample_start + (uint32_t)frames;
    const uint32_t declaredFrames = file->impl.info.total_frames > 0
        ? (uint32_t)file->impl.info.total_frames
        : 0u;
    const uint32_t requiredFrames = std::max(end, declaredFrames);
    const size_t required = (size_t)requiredFrames * (size_t)channels;

    if (required > file->impl.pcm.max_size())
        return (int32_t)VL_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE max_size is not reachable in tests.
    if (file->impl.pcm.size() < required)
        file->impl.pcm.resize(required, 0);

    for (int32_t f = 0; f < frames; ++f) {
        const size_t dst = ((size_t)sample_start + (size_t)f) * (size_t)channels;
        file->impl.pcm[dst] = floatToS16(left[f]);
        if (channels == 2)
            file->impl.pcm[dst + 1] = floatToS16(right[f]);
    }

    VLSliceEntry s;
    s.ppq_pos = (uint32_t)ppq_pos;
    s.sample_start = sample_start;
    s.sample_length = (uint32_t)frames;
    s.rendered_length = (uint32_t)frames;
    s.points = 0x7fff;
    s.selected_flag = 0;
    s.state = kSliceNormal;

    if (file->impl.slices.size() == file->impl.slices.max_size())
        return (int32_t)VL_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE max_size is not reachable in tests.
    file->impl.slices.push_back(s);

    file->impl.totalFrames = (uint32_t)(file->impl.pcm.size() / (size_t)channels);
    file->impl.info.total_frames = (int32_t)file->impl.totalFrames;
    file->impl.info.slice_count = (int32_t)file->impl.slices.size();
    if (file->impl.info.loop_end <= file->impl.info.loop_start)
        file->impl.info.loop_end = file->impl.info.total_frames;
    file->dirty = true;

    return (int32_t)file->impl.slices.size() - 1;
}

int32_t vl_add_slice(VLFile file, int32_t ppq_pos,
                     const float* left, const float* right,
                     int32_t frames) {
    if (!file) return (int32_t)VL_ERROR_INVALID_HANDLE;
    if (ppq_pos < 0) return (int32_t)VL_ERROR_INVALID_ARG;
    const uint32_t start = calcSampleStartFromPPQ(file->impl, (uint32_t)ppq_pos);
    return addSliceAtSample(file, start, ppq_pos, left, right, frames);
}

VLError vl_remove_slice(VLFile file, int32_t index) {
    if (!file) return VL_ERROR_INVALID_HANDLE;
    if (index < 0 || (size_t)index >= file->impl.slices.size())
        return VL_ERROR_INVALID_SLICE;
    file->impl.slices.erase(file->impl.slices.begin() + index);
    file->impl.info.slice_count = (int32_t)file->impl.slices.size();
    file->dirty = true;
    return VL_OK;
}

VLError vl_save(VLFile file, const char* path) {
    if (!file) return VL_ERROR_INVALID_HANDLE;
    if (!path) return VL_ERROR_INVALID_ARG;

    size_t size = 0;
    VLError e = vl_save_to_memory(file, nullptr, &size);
    if (e != VL_OK) return e;

    std::vector<uint8_t> buf(size);
    e = vl_save_to_memory(file, buf.data(), &size);
    if (e != VL_OK) return e;

    std::ofstream out(path, std::ios::binary);
    if (!out) return VL_ERROR_INVALID_ARG;
    out.write((const char*)buf.data(), (std::streamsize)size);
    return out ? VL_OK : VL_ERROR_FILE_CORRUPT;
}

VLError vl_save_to_memory(VLFile file, void* buf, size_t* size_out) {
    if (!file) return VL_ERROR_INVALID_HANDLE;
    if (!size_out) return VL_ERROR_INVALID_ARG;

    if (!file->isNew && !file->dirty && !file->impl.fileData.empty()) {
        const std::vector<uint8_t>& encoded = file->impl.fileData;
        if (!buf) {
            *size_out = encoded.size();
            return VL_OK;
        }
        if (*size_out < encoded.size()) {
            *size_out = encoded.size();
            return VL_ERROR_BUFFER_TOO_SMALL;
        }
        std::memcpy(buf, encoded.data(), encoded.size());
        *size_out = encoded.size();
        return VL_OK;
    }

    if (file->impl.slices.empty() || file->impl.pcm.empty())
        return VL_ERROR_INVALID_ARG;

    std::vector<uint8_t> encoded = buildREX2File(file->impl);

    if (!buf) {
        *size_out = encoded.size();
        return VL_OK;
    }

    if (*size_out < encoded.size()) {
        *size_out = encoded.size();
        return VL_ERROR_BUFFER_TOO_SMALL;
    }

    std::memcpy(buf, encoded.data(), encoded.size());
    *size_out = encoded.size();
    return VL_OK;
}

/* -----------------------------------------------------------------------
   Utility
   ----------------------------------------------------------------------- */

const char* vl_error_string(VLError err) {
    switch (err) {
        case VL_OK:                        return "OK";
        case VL_ERROR_INVALID_HANDLE:      return "invalid handle";
        case VL_ERROR_INVALID_ARG:         return "invalid argument";
        case VL_ERROR_FILE_NOT_FOUND:      return "file not found";
        case VL_ERROR_FILE_CORRUPT:        return "file corrupt or unsupported format";
        case VL_ERROR_OUT_OF_MEMORY:       return "out of memory";
        case VL_ERROR_INVALID_SLICE:       return "invalid slice index";
        case VL_ERROR_INVALID_SAMPLE_RATE: return "invalid sample rate";
        case VL_ERROR_BUFFER_TOO_SMALL:    return "buffer too small";
        case VL_ERROR_NO_CREATOR_INFO:     return "no creator info available";
        case VL_ERROR_NOT_IMPLEMENTED:     return "not implemented";
        case VL_ERROR_ALREADY_HAS_DATA:    return "already has data";
        default:                           return "unknown error";
    }
}

const char* vl_version_string(void) { return "velociloops 0.1.0"; }
