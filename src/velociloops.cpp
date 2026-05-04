#include "velociloops.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>


/* -----------------------------------------------------------------------
   DWOP decompressor
   ----------------------------------------------------------------------- */

namespace
{

#include "fft8g.h"

inline int32_t clampInt32(int64_t v)
{
    if (v > std::numeric_limits<int32_t>::max())
        return std::numeric_limits<int32_t>::max();

    if (v < std::numeric_limits<int32_t>::min())
        return std::numeric_limits<int32_t>::min();

    return static_cast<int32_t>(v);
}

inline int32_t addInt32(int32_t a, int32_t b)
{
    return clampInt32(static_cast<int64_t>(a) + static_cast<int64_t>(b));
}

inline int32_t subInt32(int32_t a, int32_t b)
{
    return clampInt32(static_cast<int64_t>(a) - static_cast<int64_t>(b));
}

struct VLChannelState
{
    int32_t deltas[5] = {0, 0, 0, 0, 0};
    uint32_t averages[5] = {2560, 2560, 2560, 2560, 2560};
};

class VLDWOPDecompressor
{
public:
    VLChannelState ch[2];
    uint32_t currentWord = 0;
    int32_t bitsLeft = 0;
    const uint32_t* inputPtr = nullptr;
    const uint32_t* endPtr = nullptr;
    std::vector<uint32_t> buf;

    void init(const uint8_t* data, size_t size)
    {
        const size_t words = size / 4;
        buf.resize(words);

        for (size_t i = 0; i < words; ++i)
        {
            const size_t b = i * 4;
            buf[i] = ((uint32_t)data[b] << 24) | ((uint32_t)data[b + 1] << 16) | ((uint32_t)data[b + 2] << 8) | (uint32_t)data[b + 3];
        }

        inputPtr = buf.data();
        endPtr = buf.data() + buf.size();
        currentWord = 0;
        bitsLeft = 0;
    }

    bool decompressMono(uint32_t frameCount, int32_t* out, int32_t bitDepth)
    {
        if (!out || frameCount == 0)
            return false;

        int32_t d0 = ch[0].deltas[0] * 2, d1 = ch[0].deltas[1] * 2, d2 = ch[0].deltas[2] * 2, d3 = ch[0].deltas[3] * 2, d4 = ch[0].deltas[4] * 2;

        uint32_t a0 = ch[0].averages[0], a1 = ch[0].averages[1], a2 = ch[0].averages[2], a3 = ch[0].averages[3], a4 = ch[0].averages[4];

        uint32_t j = 2;
        int32_t rbits = 0;

        uint32_t cw = currentWord;
        int32_t bl = bitsLeft;
        const uint32_t* inp = inputPtr;
        bool eof = false;

        for (uint32_t f = 0; f < frameCount && !eof; ++f)
        {
            uint32_t minAvg = a0;
            int minIdx = 0;

            if (a1 < minAvg)
            {
                minAvg = a1;
                minIdx = 1;
            }

            if (a2 < minAvg)
            {
                minAvg = a2;
                minIdx = 2;
            }

            if (a3 < minAvg)
            {
                minAvg = a3;
                minIdx = 3;
            }

            if (a4 < minAvg)
            {
                minAvg = a4;
                minIdx = 4;
            }

            uint32_t step = ((minAvg * 3u) + 36u) >> 7;
            uint32_t prefixSum = 0;
            int zerosWin = 7;

            while (true)
            {
                bool bit = readBit(cw, bl, inp, eof);

                if (eof)
                    break;

                if (bit)
                    break;

                if (step > 0 && prefixSum > 0xFFFFFFFFu - step)
                {
                    eof = true;
                    break;
                }

                prefixSum += step;
                if (--zerosWin == 0)
                {
                    step <<= 2;
                    zerosWin = 7;
                }
            }

            if (eof)
                break;

            adjustJRbits(step, j, rbits);

            uint32_t rem = (rbits > 0) ? readBits(rbits, cw, bl, inp, eof) : 0;
            if (eof)
                break;

            const int64_t thresh = static_cast<int64_t>(j) - static_cast<int64_t>(step);
            if (static_cast<int64_t>(rem) - thresh >= 0)
            {
                const uint32_t extra = readBits(1, cw, bl, inp, eof);

                if (eof)
                    break;

                rem = rem * 2u - static_cast<uint32_t>(thresh) + extra;
            }

            const uint32_t codeVal = rem + prefixSum;
            const int32_t signed2x = -(int32_t)(codeVal & 1u) ^ (int32_t)codeVal;
            const int32_t s2x = applyPredictor(minIdx, signed2x, d0, d1, d2, d3, d4);
            out[f] = clampSample(s2x >> 1, bitDepth);
            updateAverages(a0, a1, a2, a3, a4, d0, d1, d2, d3, d4);
        }

        ch[0].deltas[0] = d0 >> 1;
        ch[0].deltas[1] = d1 >> 1;
        ch[0].deltas[2] = d2 >> 1;
        ch[0].deltas[3] = d3 >> 1;
        ch[0].deltas[4] = d4 >> 1;
        ch[0].averages[0] = a0;
        ch[0].averages[1] = a1;
        ch[0].averages[2] = a2;
        ch[0].averages[3] = a3;
        ch[0].averages[4] = a4;
        currentWord = cw;
        bitsLeft = bl;
        inputPtr = inp;
        return !eof;
    }

    bool decompressStereo(uint32_t frameCount, int32_t* out, int32_t bitDepth)
    {
        if (!out || frameCount == 0)
            return false;

        int32_t d[2][5];
        uint32_t a[2][5];
        for (int c = 0; c < 2; ++c)
        {
            for (int i = 0; i < 5; ++i)
            {
                d[c][i] = ch[c].deltas[i] * 2;
                a[c][i] = ch[c].averages[i];
            }
        }

        uint32_t j[2] = {2, 2};
        int32_t rbits[2] = {0, 0};

        uint32_t cw = currentWord;
        int32_t bl = bitsLeft;
        const uint32_t* inp = inputPtr;
        bool eof = false;

        for (uint32_t f = 0; f < frameCount && !eof; ++f)
        {
            int32_t ch2x[2] = {0, 0};
            for (int c = 0; c < 2 && !eof; ++c)
            {
                uint32_t minAvg = a[c][0];
                int minIdx = 0;
                if (a[c][1] < minAvg)
                {
                    minAvg = a[c][1];
                    minIdx = 1;
                }

                if (a[c][2] < minAvg)
                {
                    minAvg = a[c][2];
                    minIdx = 2;
                }

                if (a[c][3] < minAvg)
                {
                    minAvg = a[c][3];
                    minIdx = 3;
                }

                if (a[c][4] < minAvg)
                {
                    minAvg = a[c][4];
                    minIdx = 4;
                }

                uint32_t step = ((minAvg * 3u) + 36u) >> 7;
                uint32_t prefixSum = 0;
                int zerosWin = 7;

                while (true)
                {
                    bool bit = readBit(cw, bl, inp, eof);

                    if (eof)
                        break;

                    if (bit)
                        break;

                    if (step > 0 && prefixSum > 0xFFFFFFFFu - step)
                    {
                        eof = true;
                        break;
                    }

                    prefixSum += step;
                    if (--zerosWin == 0)
                    {
                        step <<= 2;
                        zerosWin = 7;
                    }
                }

                if (eof)
                    break;

                adjustJRbits(step, j[c], rbits[c]);

                uint32_t rem = (rbits[c] > 0) ? readBits(rbits[c], cw, bl, inp, eof) : 0;
                if (eof)
                    break;

                const int64_t thresh = static_cast<int64_t>(j[c]) - static_cast<int64_t>(step);
                if (static_cast<int64_t>(rem) - thresh >= 0)
                {
                    const uint32_t extra = readBits(1, cw, bl, inp, eof);

                    if (eof)
                        break;

                    rem = rem * 2u - static_cast<uint32_t>(thresh) + extra;
                }

                const uint32_t codeVal = rem + prefixSum;
                const int32_t signed2x = -(int32_t)(codeVal & 1u) ^ (int32_t)codeVal;
                ch2x[c] = applyPredictor(minIdx, signed2x, d[c][0], d[c][1], d[c][2], d[c][3], d[c][4]);
                updateAverages(a[c][0], a[c][1], a[c][2], a[c][3], a[c][4], d[c][0], d[c][1], d[c][2], d[c][3], d[c][4]);
            }

            if (eof)
                break;

            out[f * 2 + 0] = clampSample(ch2x[0] >> 1, bitDepth);
            out[f * 2 + 1] = clampSample(((int64_t)ch2x[0] + (int64_t)ch2x[1]) >> 1, bitDepth);
        }

        for (int c = 0; c < 2; ++c)
        {
            for (int i = 0; i < 5; ++i)
            {
                ch[c].deltas[i] = d[c][i] >> 1;
                ch[c].averages[i] = a[c][i];
            }
        }
        currentWord = cw;
        bitsLeft = bl;
        inputPtr = inp;
        return !eof;
    }

private:
    static int32_t clampSample(int64_t v, int32_t bitDepth)
    {
        const int32_t maxSample = bitDepth == 24 ? 8388607 : 32767;
        const int32_t minSample = bitDepth == 24 ? -8388608 : -32768;

        if (v > maxSample)
            return maxSample;

        if (v < minSample)
            return minSample;

        return (int32_t)v;
    }

    bool readBit(uint32_t& cw, int32_t& bl, const uint32_t*& inp, bool& eof)
    {
        const int32_t blBefore = bl--;
        if (blBefore - 1 < 0)
        {
            if (inp >= endPtr)
            {
                eof = true;
                return false;
            }

            cw = *inp++;
            bl = 0x1f;
        }

        const bool bit = (int32_t)cw < 0;
        cw <<= 1;
        return bit;
    }

    uint32_t readBits(int n, uint32_t& cw, int32_t& bl, const uint32_t*& inp, bool& eof)
    {
        if (n <= 0 || n > 31)
        {
            eof = true;
            return 0;
        }

        uint32_t result = cw >> (32 - n);
        cw <<= n;

        const int32_t blBefore = bl;
        bl -= n;

        if (blBefore - n < 0)
        {
            if (inp >= endPtr)
            {
                eof = true;
                return result;
            }

            const uint32_t next = *inp++;
            bl += 32;
            result |= next >> bl;
            cw = next << (32 - bl);
        }

        return result;
    }

    static void adjustJRbits(uint32_t step, uint32_t& j, int32_t& rbits)
    {
        if (step < j)
        {
            for (uint32_t jt = j >> 1; step < jt; jt >>= 1)
            {
                j = jt;
                --rbits;
            }
        }
        else
        {
            while (step >= j)
            {
                const uint32_t prev = j;

                j <<= 1;
                ++rbits;

                if (j <= prev)
                {
                    j = prev;
                    break;
                }
            }
        }
    }

    static int32_t applyPredictor(int idx, int32_t s2x, int32_t& d0, int32_t& d1, int32_t& d2, int32_t& d3, int32_t& d4)
    {
        switch (idx)
        {
            case 0:
            {
                const int32_t t0 = subInt32(s2x, d0), t1 = subInt32(t0, d1), t2 = subInt32(t1, d2);
                d4 = subInt32(t2, d3);
                d3 = t2;
                d2 = t1;
                d1 = t0;
                d0 = s2x;
                return s2x;
            }

            case 1:
            {
                const int32_t t1 = subInt32(s2x, d1), t2 = subInt32(t1, d2), nd0 = addInt32(d0, s2x);
                d4 = subInt32(t2, d3);
                d3 = t2;
                d2 = t1;
                d1 = s2x;
                d0 = nd0;
                return nd0;
            }

            case 2:
            {
                const int32_t nd1 = addInt32(d1, s2x), nd0 = addInt32(d0, nd1), t = subInt32(s2x, d2);
                d4 = subInt32(t, d3);
                d3 = t;
                d2 = s2x;
                d1 = nd1;
                d0 = nd0;
                return nd0;
            }

            case 3:
            {
                const int32_t nd2 = addInt32(d2, s2x), nd1 = addInt32(d1, nd2), nd0 = addInt32(d0, nd1);
                d4 = subInt32(s2x, d3);
                d3 = s2x;
                d2 = nd2;
                d1 = nd1;
                d0 = nd0;
                return nd0;
            }

            case 4:
            {
                const int32_t nd3 = addInt32(d3, s2x), nd2 = addInt32(d2, nd3), nd1 = addInt32(d1, nd2), nd0 = addInt32(d0, nd1);
                d4 = s2x;
                d3 = nd3;
                d2 = nd2;
                d1 = nd1;
                d0 = nd0;
                return nd0;
            }

            default: return d0; // LCOV_EXCL_LINE idx comes from a 0..4 minimum search.
        }
    }

    static void updateAverages(uint32_t& a0, uint32_t& a1, uint32_t& a2, uint32_t& a3, uint32_t& a4, int32_t d0, int32_t d1, int32_t d2, int32_t d3, int32_t d4)
    {
        auto mag = [](int32_t v) -> uint32_t
        {
            return (uint32_t)(v ^ (v >> 31));
        };

        a0 = a0 + mag(d0) - (a0 >> 5);
        a1 = a1 + mag(d1) - (a1 >> 5);
        a2 = a2 + mag(d2) - (a2 >> 5);
        a3 = a3 + mag(d3) - (a3 >> 5);
        a4 = a4 + mag(d4) - (a4 >> 5);
    }
};

class VLBitWriter
{
public:
    void writeBit(bool bit)
    {
        if (bit)
            currentWord |= (uint32_t)1u << (31 - bitCount);
        if (++bitCount == 32)
            flushWord();
    }

    void writeBits(uint32_t value, int count)
    {
        for (int i = count - 1; i >= 0; --i)
            writeBit(((value >> i) & 1u) != 0);
    }

    std::vector<uint8_t> finish()
    {
        if (bitCount > 0)
            flushWord();

        return bytes;
    }

private:
    std::vector<uint8_t> bytes;
    uint32_t currentWord = 0;
    int bitCount = 0;

    void flushWord()
    {
        bytes.push_back((uint8_t)(currentWord >> 24));
        bytes.push_back((uint8_t)(currentWord >> 16));
        bytes.push_back((uint8_t)(currentWord >> 8));
        bytes.push_back((uint8_t)currentWord);
        currentWord = 0;
        bitCount = 0;
    }
};

class VLDWOPCompressor
{
public:
    VLChannelState ch[2];

    std::vector<uint8_t> compressMono(const int32_t* in, uint32_t frameCount)
    {
        reset();

        VLBitWriter bw;
        int32_t d[5] = {};
        uint32_t a[5] = {2560, 2560, 2560, 2560, 2560};
        uint32_t j = 2;
        int32_t rbits = 0;

        for (uint32_t f = 0; f < frameCount; ++f)
            encodeChannel(in[f] * 2, d, a, j, rbits, bw);

        return bw.finish();
    }

    std::vector<uint8_t> compressStereo(const int32_t* in, uint32_t frameCount)
    {
        reset();

        VLBitWriter bw;
        int32_t d[2][5] = {};
        uint32_t a[2][5] = {{2560, 2560, 2560, 2560, 2560}, {2560, 2560, 2560, 2560, 2560}};
        uint32_t j[2] = {2, 2};
        int32_t rbits[2] = {0, 0};

        for (uint32_t f = 0; f < frameCount; ++f)
        {
            const int32_t left2x = in[(size_t)f * 2] * 2;
            const int32_t right2x = in[(size_t)f * 2 + 1] * 2;
            encodeChannel(left2x, d[0], a[0], j[0], rbits[0], bw);
            encodeChannel(right2x - left2x, d[1], a[1], j[1], rbits[1], bw);
        }

        return bw.finish();
    }

private:
    void reset()
    {
        for (auto& c : ch)
        {
            std::fill(c.deltas, c.deltas + 5, 0);
            std::fill(c.averages, c.averages + 5, 2560);
        }
    }

    static uint32_t toCodeValue(int32_t signed2x)
    {
        if (signed2x >= 0)
            return (uint32_t)signed2x;

        return (uint32_t)(-signed2x - 1);
    }

    static int minAverageIndex(const uint32_t a[5])
    {
        int idx = 0;

        for (int i = 1; i < 5; ++i)
        {
            if (a[i] < a[idx])
                idx = i;
        }

        return idx;
    }

    static int32_t predictorResidual(int idx, int32_t sample2x, const int32_t d[5])
    {
        switch (idx)
        {
            case 0: return sample2x;

            case 1: return sample2x - d[0];

            case 2: return sample2x - d[0] - d[1];

            case 3: return sample2x - d[0] - d[1] - d[2];

            case 4: return sample2x - d[0] - d[1] - d[2] - d[3];

            default: return sample2x; // LCOV_EXCL_LINE idx comes from a 0..4 minimum search.
        }
    }

    static int32_t applyPredictor(int idx, int32_t s2x, int32_t d[5])
    {
        switch (idx)
        {
            case 0:
            {
                const int32_t t0 = s2x - d[0], t1 = t0 - d[1], t2 = t1 - d[2];
                d[4] = t2 - d[3];
                d[3] = t2;
                d[2] = t1;
                d[1] = t0;
                d[0] = s2x;
                return s2x;
            }

            case 1:
            {
                const int32_t t1 = s2x - d[1], t2 = t1 - d[2], nd0 = d[0] + s2x;
                d[4] = t2 - d[3];
                d[3] = t2;
                d[2] = t1;
                d[1] = s2x;
                d[0] = nd0;
                return nd0;
            }

            case 2:
            {
                const int32_t nd1 = d[1] + s2x, nd0 = d[0] + nd1, t = s2x - d[2];
                d[4] = t - d[3];
                d[3] = t;
                d[2] = s2x;
                d[1] = nd1;
                d[0] = nd0;
                return nd0;
            }

            case 3:
            {
                const int32_t nd2 = d[2] + s2x, nd1 = d[1] + nd2, nd0 = d[0] + nd1;
                d[4] = s2x - d[3];
                d[3] = s2x;
                d[2] = nd2;
                d[1] = nd1;
                d[0] = nd0;
                return nd0;
            }

            case 4:
            {
                const int32_t nd3 = d[3] + s2x, nd2 = d[2] + nd3, nd1 = d[1] + nd2, nd0 = d[0] + nd1;
                d[4] = s2x;
                d[3] = nd3;
                d[2] = nd2;
                d[1] = nd1;
                d[0] = nd0;
                return nd0;
            }

            default: return d[0]; // LCOV_EXCL_LINE idx comes from a 0..4 minimum search.
        }
    }

    static void updateAverages(uint32_t a[5], const int32_t d[5])
    {
        auto mag = [](int32_t v) -> uint32_t
        {
            return (uint32_t)(v ^ (v >> 31));
        };

        for (int i = 0; i < 5; ++i)
        {
            a[i] = a[i] + mag(d[i]) - (a[i] >> 5);
        }
    }

    static void adjustJRbits(uint32_t step, uint32_t& j, int32_t& rbits)
    {
        if (step < j)
        {
            for (uint32_t jt = j >> 1; step < jt; jt >>= 1)
            {
                j = jt;
                --rbits;
            }
        }
        else
        {
            while (step >= j)
            {
                const uint32_t prev = j;

                j <<= 1;
                ++rbits;

                if (j <= prev)
                {
                    j = prev;
                    break;
                }
            }
        }
    }

    static bool encodeRemainder(uint32_t raw, uint32_t step, uint32_t j, int32_t rbits, uint32_t& remBits, bool& hasExtra, bool& extraBit)
    {
        if (rbits < 0 || rbits > 31)
            return false;

        const uint32_t limit = rbits == 31 ? 0x80000000u : (1u << rbits);
        const int32_t threshSigned = (int32_t)j - (int32_t)step;
        if (threshSigned < 0)
            return false;

        const uint32_t thresh = (uint32_t)threshSigned;

        if (raw < thresh)
        {
            if (raw >= limit)
                return false;

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

    static void writeCodeValue(uint32_t codeVal, uint32_t baseStep, uint32_t& j, int32_t& rbits, VLBitWriter& bw)
    {
        uint32_t prefixSum = 0;
        uint32_t step = baseStep;
        int zerosWin = 7;

        for (uint32_t zeros = 0; zeros < 0x100000; ++zeros)
        {
            if (codeVal >= prefixSum)
            {
                uint32_t trialJ = j;
                int32_t trialRbits = rbits;
                adjustJRbits(step, trialJ, trialRbits);

                uint32_t remBits = 0;
                bool hasExtra = false;
                bool extraBit = false;
                if (encodeRemainder(codeVal - prefixSum, step, trialJ, trialRbits, remBits, hasExtra, extraBit))
                {
                    for (uint32_t i = 0; i < zeros; ++i)
                        bw.writeBit(false);

                    bw.writeBit(true);

                    if (trialRbits > 0)
                        bw.writeBits(remBits, trialRbits);

                    if (hasExtra)
                        bw.writeBit(extraBit);

                    j = trialJ;
                    rbits = trialRbits;
                    return;
                }
            }

            if (baseStep == 0)
                break;

            if (UINT32_MAX - prefixSum < step)
                break;

            prefixSum += step;
            if (prefixSum > codeVal && step != 0)
                break;

            if (--zerosWin == 0)
            {
                step <<= 2;
                zerosWin = 7;
            }
        }

        bw.writeBit(true); // LCOV_EXCL_LINE emergency fallback; valid sample residuals encode above.
    }

    static void encodeChannel(int32_t sample2x, int32_t d[5], uint32_t a[5], uint32_t& j, int32_t& rbits, VLBitWriter& bw)
    {
        const int idx = minAverageIndex(a);
        const uint32_t baseStep = ((a[idx] * 3u) + 36u) >> 7;
        const int32_t residual = predictorResidual(idx, sample2x, d);
        writeCodeValue(toCodeValue(residual), baseStep, j, rbits, bw);
        applyPredictor(idx, residual, d);
        updateAverages(a, d);
    }
};

class VLIFFWriter
{
public:
    std::vector<uint8_t> data;

    size_t beginChunk(const char id[4])
    {
        const size_t start = data.size();
        data.insert(data.end(), id, id + 4);
        put32(0);
        return start;
    }

    size_t beginCat(const char type[4])
    {
        const size_t start = beginChunk("CAT ");
        data.insert(data.end(), type, type + 4);
        return start;
    }

    void endChunk(size_t start)
    {
        const size_t payloadStart = start + 8;
        const uint32_t size = (uint32_t)(data.size() - payloadStart);

        data[start + 4] = (uint8_t)(size >> 24);
        data[start + 5] = (uint8_t)(size >> 16);
        data[start + 6] = (uint8_t)(size >> 8);
        data[start + 7] = (uint8_t)size;

        if (data.size() & 1u)
            data.push_back(0);
    }

    void put8(uint8_t v)
    {
        data.push_back(v);
    }

    void put16(uint16_t v)
    {
        data.push_back((uint8_t)(v >> 8));
        data.push_back((uint8_t)v);
    }

    void put32(uint32_t v)
    {
        data.push_back((uint8_t)(v >> 24));
        data.push_back((uint8_t)(v >> 16));
        data.push_back((uint8_t)(v >> 8));
        data.push_back((uint8_t)v);
    }

    void putBytes(const uint8_t* p, size_t n)
    {
        data.insert(data.end(), p, p + n);
    }
};

/* -----------------------------------------------------------------------
   REX2 file parser + DWOP decompressor wrapper
   ----------------------------------------------------------------------- */

enum VLSliceState
{
    kSliceNormal = 1,
    kSliceMuted = 2,
    kSliceLocked = 3
};

struct VLSliceEntry
{
    uint32_t ppq_pos = 0;
    uint32_t sample_length = 0;
    uint32_t rendered_length = 0;
    uint32_t sample_start = 0;
    uint32_t render_loop_start = 0;
    uint32_t render_loop_end = 0;
    float render_loop_volume_compensation = 1.0f;
    uint16_t points = 0x7fff;
    uint8_t selected_flag = 0;
    VLSliceState state = kSliceNormal;
    bool synthetic_leading = false;
    bool marker = false;
};

int32_t sliceFlags(const VLSliceEntry& s)
{
    int32_t flags = 0;
    if (s.state == kSliceMuted)
        flags |= VL_SLICE_FLAG_MUTED;
    else if (s.state == kSliceLocked)
        flags |= VL_SLICE_FLAG_LOCKED;
    if (s.selected_flag)
        flags |= VL_SLICE_FLAG_SELECTED;
    if (s.marker)
        flags |= VL_SLICE_FLAG_MARKER;
    if (s.synthetic_leading)
        flags |= VL_SLICE_FLAG_SYNTHETIC;
    return flags;
}

VLError applySliceFlags(VLSliceEntry& s, int32_t flags, int32_t analysisPoints)
{
    static constexpr int32_t kKnownFlags = VL_SLICE_FLAG_MUTED | VL_SLICE_FLAG_LOCKED | VL_SLICE_FLAG_SELECTED | VL_SLICE_FLAG_MARKER | VL_SLICE_FLAG_SYNTHETIC;

    if (flags & ~kKnownFlags)
        return VL_ERROR_INVALID_ARG;

    if ((flags & VL_SLICE_FLAG_MUTED) && (flags & VL_SLICE_FLAG_LOCKED))
        return VL_ERROR_INVALID_ARG;

    if (flags & VL_SLICE_FLAG_LOCKED)
        s.state = kSliceLocked;
    else if (flags & VL_SLICE_FLAG_MUTED)
        s.state = kSliceMuted;
    else
        s.state = kSliceNormal;

    s.selected_flag = (flags & VL_SLICE_FLAG_SELECTED) ? 1 : 0;

    if (analysisPoints >= 0)
    {
        if (analysisPoints > 0x7fff)
            return VL_ERROR_INVALID_ARG;
        s.points = (uint16_t)analysisPoints;
    }

    return VL_OK;
}

template <typename T> class VLHeapArray
{
public:
    VLHeapArray() = default;

    VLHeapArray(const VLHeapArray&) = delete;
    VLHeapArray& operator=(const VLHeapArray&) = delete;

    VLHeapArray(VLHeapArray&& o) noexcept : ptr_(std::move(o.ptr_)), size_(o.size_)
    {
        o.size_ = 0;
    }

    VLHeapArray& operator=(VLHeapArray&& o) noexcept
    {
        ptr_ = std::move(o.ptr_);
        size_ = o.size_;
        o.size_ = 0;
        return *this;
    }

    [[nodiscard]] bool assign(std::size_t n, T val) noexcept
    {
        if (n == 0)
        {
            ptr_.reset();
            size_ = 0;
            return true;
        }

        T* raw = new (std::nothrow) T[n];
        if (!raw)
            return false;

        std::fill_n(raw, n, val);
        ptr_.reset(raw);
        size_ = n;

        return true;
    }

    [[nodiscard]] bool resize(std::size_t n, T val) noexcept
    {
        if (n == size_)
            return true;

        if (n == 0)
        {
            ptr_.reset();
            size_ = 0;
            return true;
        }

        T* raw = new (std::nothrow) T[n];
        if (!raw)
            return false;

        const std::size_t keep = std::min(size_, n);
        if (keep)
            std::memcpy(raw, ptr_.get(), keep * sizeof(T));

        if (n > size_)
            std::fill_n(raw + size_, n - size_, val);

        ptr_.reset(raw);
        size_ = n;
        return true;
    }

    T& operator[](std::size_t i) noexcept
    {
        assert(i < size_);
        return ptr_[i];
    }

    const T& operator[](std::size_t i) const noexcept
    {
        assert(i < size_);
        return ptr_[i];
    }

    T* data() noexcept
    {
        return ptr_.get();
    }

    const T* data() const noexcept
    {
        return ptr_.get();
    }

    std::size_t size() const noexcept
    {
        return size_;
    }

    bool empty() const noexcept
    {
        return size_ == 0;
    }

    static constexpr std::size_t max_size() noexcept
    {
        return std::numeric_limits<std::size_t>::max() / sizeof(T);
    }

private:
    std::unique_ptr<T[]> ptr_;
    std::size_t size_ = 0;
};

class VLFileImpl
{
public:
    VLFileInfo info = {};
    VLCreatorInfo creator = {};
    std::vector<VLSliceEntry> slices;
    std::vector<uint8_t> fileData;
    VLHeapArray<int32_t> pcm;
    uint32_t totalFrames = 0;
    uint32_t loopStart = 0;
    uint32_t loopEnd = 0;
    bool transientEnabled = true;
    uint16_t transientAttack = 0x15;
    uint16_t transientDecay = 0x3ff;
    uint16_t transientStretch = 0x28;
    uint16_t processingGain = 1000;
    uint8_t analysisSensitivity = 0;
    uint16_t gateSensitivity = 0;
    bool silenceSelected = false;
    bool headerValid = true;
    VLError loadError = VL_OK;
    bool loadedFromFile = false;
    uint16_t globBars = 1;
    uint8_t globBeats = 0;

    static constexpr int32_t kREXPPQ = 15360;

    bool loadFromBuffer(const char* buf, size_t size)
    {
        fileData.assign((const uint8_t*)buf, (const uint8_t*)buf + size);

        info.channels = 1;
        info.sample_rate = 44100;
        info.slice_count = 0;
        info.tempo = 120000;
        info.original_tempo = 120000;
        info.ppq_length = 61440;
        info.time_sig_num = 4;
        info.time_sig_den = 4;
        info.bit_depth = 16;
        info.total_frames = 0;
        info.loop_start = 0;
        info.loop_end = 0;
        info.processing_gain = processingGain;
        info.transient_enabled = transientEnabled ? 1 : 0;
        info.transient_attack = transientAttack;
        info.transient_decay = transientDecay;
        info.transient_stretch = transientStretch;
        info.silence_selected = silenceSelected ? 1 : 0;
        analysisSensitivity = 0;
        gateSensitivity = 0;
        headerValid = true;
        loadError = VL_OK;
        loadedFromFile = false;
        globBars = 1;
        globBeats = 0;

        std::memset(&creator, 0, sizeof(creator));

        if (fileData.size() < 12)
            return fail(VL_ERROR_INVALID_SIZE);

        if (fileData[0] == 'F' && fileData[1] == 'O' && fileData[2] == 'R' && fileData[3] == 'M' && fileData[8] == 'A' && fileData[9] == 'I' &&
            fileData[10] == 'F' && fileData[11] == 'F')
        {
            const bool ok = loadLegacyAIFF();
            if (ok)
                loadedFromFile = true;
            return ok;
        }

        if (fileData[0] != 'C' || fileData[1] != 'A' || fileData[2] != 'T' || fileData[3] != ' ')
            return fail(VL_ERROR_FILE_CORRUPT);

        size_t dwopOffset = 0, dwopSize = 0;
        bool hasDWOP = false;

        parseIFF(8 + 4, fileData.size(), dwopOffset, dwopSize, hasDWOP);
        if (loadError != VL_OK)
            return false;

        finalizeSlices();

        if (!headerValid)
            return false;

        if (!hasDWOP || dwopSize == 0)
            return fail(VL_ERROR_FILE_CORRUPT);

        if (dwopOffset + dwopSize > fileData.size())
            return fail(VL_ERROR_INVALID_SIZE);

        if (totalFrames == 0)
            return fail(VL_ERROR_INVALID_SIZE);

        // 3600 * 192000 = 691,200,000 — exactly 1 hour at 192 kHz max sample rate.
        static constexpr uint32_t kMaxTotalFrames = 3600u * 192000u;
        if (totalFrames > kMaxTotalFrames)
            return fail(VL_ERROR_INVALID_SIZE);

        const size_t pcmElements = (size_t)totalFrames * (size_t)info.channels;

        if (!pcm.assign(pcmElements, 0))
            return fail(VL_ERROR_OUT_OF_MEMORY);

        VLDWOPDecompressor dec;
        dec.init(&fileData[dwopOffset], dwopSize);

        uint32_t done = 0;
        bool ok = true;
        while (done < totalFrames && ok)
        {
            const uint32_t chunk = std::min<uint32_t>(0x100000, totalFrames - done);

            if (info.channels == 1)
                ok = dec.decompressMono(chunk, &pcm[done], info.bit_depth);
            else
                ok = dec.decompressStereo(chunk, &pcm[(size_t)done * 2], info.bit_depth);

            done += chunk;
        }

        if (!ok)
            return fail(VL_ERROR_FILE_CORRUPT);

        finalizeRenderedLengths();
        loadedFromFile = true;
        return true;
    }

private:
    bool fail(VLError error)
    {
        if (loadError == VL_OK)
            loadError = error;
        return false;
    }

    static uint32_t be32(const uint8_t* p)
    {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }

    static uint16_t be16(const uint8_t* p)
    {
        return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
    }

    static int32_t readAIFFExtendedRate(const uint8_t* p)
    {
        const uint16_t expon = be16(p);
        uint64_t mant = 0;
        for (int i = 0; i < 8; ++i)
            mant = (mant << 8) | p[2 + i];

        if (expon == 0 || mant == 0)
            return 0;

        const int sign = (expon & 0x8000u) ? -1 : 1;
        const int exp = (int)(expon & 0x7fffu) - 16383;
        const long double value = (long double)sign * (long double)mant * std::ldexp((long double)1.0, exp - 63);
        if (value <= 0.0L || value > (long double)std::numeric_limits<int32_t>::max())
            return 0;

        return (int32_t)std::lround((double)value);
    }

    static int32_t readSignedSampleBE(const uint8_t* p, uint16_t bits)
    {
        switch (bits)
        {
            case 8: return (int32_t)(int8_t)p[0] * 256;
            case 16: return (int16_t)be16(p);
            case 24:
            {
                int32_t v = ((int32_t)p[0] << 16) | ((int32_t)p[1] << 8) | (int32_t)p[2];
                if (v & 0x800000)
                    v |= ~0xffffff;
                return v;
            }
            case 32:
            {
                int32_t v = (int32_t)be32(p);
                return v >> 16;
            }
            default: return 0;
        }
    }

    bool parseLegacyTempo(const uint8_t* d, uint32_t sz, bool appIsReCy)
    {
        const uint32_t tempoOffset = appIsReCy ? 14u : 16u;
        if (sz < tempoOffset + 4u)
            return false;

        const uint32_t v = be32(d + tempoOffset);
        if (v < 20000u || v > 450000u)
            return false;

        info.tempo = (int32_t)v;
        info.original_tempo = (int32_t)v;
        return true;
    }

    static uint16_t legacyReCycleFilterPoints(uint16_t sensitivity)
    {
        const uint32_t sens = std::min<uint32_t>(sensitivity, 1000u);
        const uint32_t visibleRange = (sens * 0x7fffu + 999u) / 1000u;
        return (uint16_t)(0x7fffu - visibleRange);
    }

    bool parseLegacyReCycleSlices(const uint8_t* d, uint32_t sz, std::vector<VLSliceEntry>& out)
    {
        if (sz < 4u + 0xa0u)
            return false;

        const uint8_t* binary = d + 4;
        const uint32_t binarySize = sz - 4u;
        if (be32(binary) != 0xd1daded0u)
            return false;

        const uint16_t sensitivity = be16(binary + 0x14);
        const uint16_t filterPoints = legacyReCycleFilterPoints(sensitivity);
        const uint16_t storedCount = be16(binary + 0x9e);
        if (storedCount == 0 || storedCount > 1000)
            return false;

        if (binarySize < 0xa0u + (uint32_t)storedCount * 8u)
            return false;

        std::vector<VLSliceEntry> parsed;
        parsed.reserve(storedCount);
        for (uint16_t i = 0; i < storedCount; ++i)
        {
            const uint8_t* rec = binary + 0xa0u + (uint32_t)i * 8u;
            const uint8_t state = rec[0] & 0x7f;
            const bool selected = (rec[0] & 0x80) != 0;
            const uint32_t start = ((uint32_t)rec[1] << 24) | ((uint32_t)rec[2] << 16) | ((uint32_t)rec[3] << 8) | rec[4];
            const uint16_t points = be16(rec + 6);

            if (!selected && (state != 0 || points <= filterPoints))
                continue;

            VLSliceEntry s;
            s.sample_start = start;
            s.sample_length = 1;
            s.points = points;
            s.selected_flag = selected ? 1 : 0;
            if (state == 1)
                s.state = kSliceLocked;
            else if (state == 2)
                s.state = kSliceMuted;
            else
                s.state = kSliceNormal;
            parsed.push_back(s);
        }

        if (parsed.empty())
            return false;

        out = std::move(parsed);
        return true;
    }

    bool parseLegacyREXSlices(const uint8_t* d, uint32_t sz, std::vector<VLSliceEntry>& out, uint32_t& ppqLength,
                              uint32_t& exportedFrameCount)
    {
        if (sz < 4u + 0x3f8u)
            return false;

        const uint8_t* binary = d + 4;
        const uint32_t binarySize = sz - 4u;
        if (be32(binary) != 0xd1d1d1dau)
            return false;

        const uint32_t storedPpqLength = be32(binary + 6);
        if (storedPpqLength == 0 || storedPpqLength > std::numeric_limits<uint32_t>::max() / 16u)
            return false;

        const uint16_t storedCount = be16(binary + 0x0a);
        if (storedCount == 0 || storedCount > 1000)
            return false;

        if (binarySize < 0x3f8u + (uint32_t)storedCount * 12u)
            return false;

        std::vector<VLSliceEntry> parsed;
        std::vector<uint32_t> ppqPositions;
        std::vector<uint32_t> sourceLengths;
        parsed.reserve(storedCount);
        ppqPositions.reserve(storedCount);
        sourceLengths.reserve(storedCount);
        for (uint16_t i = 0; i < storedCount; ++i)
        {
            const uint8_t* rec = binary + 0x3f8u + (uint32_t)i * 12u;
            const uint32_t start = be32(rec);
            const uint32_t length = be32(rec + 4);
            const uint32_t ppq16 = be32(rec + 8);
            if (ppq16 > storedPpqLength || ppq16 > std::numeric_limits<uint32_t>::max() / 16u)
                return false;
            if (length == 0)
                continue;

            VLSliceEntry s;
            s.ppq_pos = ppq16 * 16u;
            s.sample_start = start;
            s.sample_length = length;
            s.points = 0x7fff;
            s.selected_flag = i == 0 ? 1 : 0;
            s.state = kSliceNormal;
            parsed.push_back(s);
            ppqPositions.push_back(ppq16);
            sourceLengths.push_back(length);
        }

        if (parsed.empty())
            return false;

        double samplesPerPpq = 0.0;
        for (size_t i = 0; i < ppqPositions.size(); ++i)
        {
            uint32_t nextPpq = storedPpqLength;
            for (size_t j = i + 1; j < ppqPositions.size(); ++j)
            {
                if (ppqPositions[j] != ppqPositions[i])
                {
                    nextPpq = ppqPositions[j];
                    break;
                }
            }

            if (nextPpq <= ppqPositions[i])
                continue;

            const uint32_t delta = nextPpq - ppqPositions[i];
            samplesPerPpq = std::max(samplesPerPpq, (double)sourceLengths[i] / (double)delta);
        }

        if (samplesPerPpq <= 0.0)
            return false;

        ppqLength = storedPpqLength * 16u;
        exportedFrameCount = (uint32_t)(samplesPerPpq * (double)storedPpqLength);
        out = std::move(parsed);
        return true;
    }

    bool loadLegacyAIFF()
    {
        uint16_t channels = 0;
        uint32_t frameCount = 0;
        uint16_t bits = 0;
        int32_t sampleRate = 0;
        bool haveCOMM = false;
        bool haveSSND = false;
        size_t ssndOffset = 0;
        size_t ssndSize = 0;
        uint32_t markerLoopStart = 0;
        uint32_t markerLoopEnd = 0;
        bool haveLoopStart = false;
        bool haveLoopEnd = false;
        bool sawLegacyApp = false;
        bool sawLegacyTempo = false;
        bool legacyLoopLengthSet = true;
        std::vector<VLSliceEntry> legacySlices;
        bool legacySlicesHaveExplicitPpq = false;
        uint32_t legacyRexPpqLength = 0;
        uint32_t legacyRexExportedFrameCount = 0;

        const size_t formEnd = std::min(fileData.size(), (size_t)be32(&fileData[4]) + 8u);
        size_t off = 12;
        while (off + 8 <= formEnd && off + 8 <= fileData.size())
        {
            char id[5] = {};
            std::memcpy(id, &fileData[off], 4);
            const uint32_t sz = be32(&fileData[off + 4]);
            const size_t payload = off + 8;
            if (payload + sz > fileData.size())
                break;

            if (std::strcmp(id, "COMM") == 0 && sz >= 18)
            {
                channels = be16(&fileData[payload]);
                frameCount = be32(&fileData[payload + 2]);
                bits = be16(&fileData[payload + 6]);
                sampleRate = readAIFFExtendedRate(&fileData[payload + 8]);
                haveCOMM = true;
            }
            else if (std::strcmp(id, "SSND") == 0 && sz >= 8)
            {
                const uint32_t dataOffset = be32(&fileData[payload]);
                const size_t dataStart = payload + 8u + (size_t)dataOffset;
                if (dataStart <= payload + sz && dataStart <= fileData.size())
                {
                    ssndOffset = dataStart;
                    ssndSize = (payload + sz) - dataStart;
                    haveSSND = true;
                }
            }
            else if (std::strcmp(id, "MARK") == 0 && sz >= 2)
            {
                uint32_t pos = 2;
                const uint16_t count = be16(&fileData[payload]);
                for (uint16_t i = 0; i < count && pos + 7 <= sz; ++i)
                {
                    const uint32_t markerPos = be32(&fileData[payload + pos + 2]);
                    const uint8_t nameLen = fileData[payload + pos + 6];
                    pos += 7;
                    if (pos + nameLen > sz)
                        break;

                    const char* name = (const char*)&fileData[payload + pos];
                    const bool isLoopStart = nameLen == 10 && std::memcmp(name, "Loop start", 10) == 0;
                    const bool isLoopEnd = nameLen == 8 && std::memcmp(name, "Loop end", 8) == 0;
                    if (isLoopStart)
                    {
                        markerLoopStart = markerPos;
                        haveLoopStart = true;
                    }
                    else if (isLoopEnd)
                    {
                        markerLoopEnd = markerPos;
                        haveLoopEnd = true;
                    }

                    pos += nameLen + (((nameLen + 1u) & 1u) ? 1u : 0u);
                }
            }
            else if (std::strcmp(id, "APPL") == 0 && sz >= 8)
            {
                const bool appIsREX = std::memcmp(&fileData[payload], "REX ", 4) == 0;
                const bool appIsReCy = std::memcmp(&fileData[payload], "ReCy", 4) == 0;
                if (appIsREX || appIsReCy)
                {
                    sawLegacyApp = true;
                    if (appIsReCy)
                    {
                        legacyLoopLengthSet = sz > 12 && fileData[payload + 12] != 0;
                        parseLegacyReCycleSlices(&fileData[payload], sz, legacySlices);
                    }
                    else if (appIsREX)
                    {
                        legacySlicesHaveExplicitPpq =
                            parseLegacyREXSlices(&fileData[payload], sz, legacySlices, legacyRexPpqLength, legacyRexExportedFrameCount);
                    }
                    sawLegacyTempo = parseLegacyTempo(&fileData[payload], sz, appIsReCy) || sawLegacyTempo;
                }
            }

            off = payload + sz;
            if (off & 1)
                ++off;
        }

        if (!sawLegacyApp)
            return fail(VL_ERROR_FILE_CORRUPT);

        if (!sawLegacyTempo)
            return fail(VL_ERROR_INVALID_TEMPO);

        if (!legacyLoopLengthSet)
            return fail(VL_ERROR_ZERO_LOOP_LENGTH);

        if (!haveCOMM || !haveSSND || channels < 1 || channels > 2 || frameCount == 0 || sampleRate <= 0)
        {
            if (frameCount == 0)
                return fail(VL_ERROR_INVALID_SIZE);
            if (sampleRate <= 0)
                return fail(VL_ERROR_INVALID_SAMPLE_RATE);
            return fail(VL_ERROR_FILE_CORRUPT);
        }

        if (bits != 8 && bits != 16 && bits != 24 && bits != 32)
            return fail(VL_ERROR_FILE_CORRUPT);

        const size_t bytesPerSample = (size_t)((bits + 7u) / 8u);
        const size_t frameBytes = bytesPerSample * (size_t)channels;
        if (frameBytes == 0)
            return fail(VL_ERROR_INVALID_SIZE);

        const uint32_t availableFrames = (uint32_t)std::min<size_t>(frameCount, ssndSize / frameBytes);
        if (availableFrames == 0)
            return fail(VL_ERROR_INVALID_SIZE);

        if (!pcm.assign((size_t)availableFrames * channels, 0))
            return fail(VL_ERROR_OUT_OF_MEMORY);

        const uint8_t* src = &fileData[ssndOffset];
        for (uint32_t frame = 0; frame < availableFrames; ++frame)
        {
            for (uint16_t ch = 0; ch < channels; ++ch)
            {
                const size_t sample = ((size_t)frame * channels + ch) * bytesPerSample;
                pcm[(size_t)frame * channels + ch] = readSignedSampleBE(src + sample, bits);
            }
        }

        totalFrames = availableFrames;
        loopStart = haveLoopStart && markerLoopStart < availableFrames ? markerLoopStart : 0;
        loopEnd = haveLoopEnd && markerLoopEnd > loopStart && markerLoopEnd <= availableFrames ? markerLoopEnd : availableFrames;

        info.channels = channels;
        info.sample_rate = sampleRate;
        info.slice_count = 1;
        info.ppq_length = legacyRexPpqLength > 0 ? (int32_t)legacyRexPpqLength : kREXPPQ * 4;
        info.time_sig_num = 4;
        info.time_sig_den = 4;
        info.bit_depth = bits;
        info.total_frames = (int32_t)totalFrames;
        info.loop_start = (int32_t)loopStart;
        info.loop_end = (int32_t)loopEnd;
        info.processing_gain = processingGain;
        info.transient_enabled = 0;
        info.transient_attack = 0;
        info.transient_decay = 1023;
        info.transient_stretch = 0;
        info.silence_selected = 0;
        transientEnabled = false;
        transientAttack = 0;
        transientDecay = 1023;
        transientStretch = 0;

        if (legacyRexExportedFrameCount > 0 && info.ppq_length > 0)
        {
            const double beats = (double)info.ppq_length / (double)kREXPPQ;
            const double bpm = beats * 60.0 * (double)info.sample_rate / (double)legacyRexExportedFrameCount;
            const int32_t derivedOriginalTempo = (int32_t)std::lround(bpm * 1000.0);
            if (derivedOriginalTempo > 0)
                info.original_tempo = derivedOriginalTempo;
        }

        if (!legacySlices.empty())
        {
            std::sort(legacySlices.begin(), legacySlices.end(),
                      [](const VLSliceEntry& a, const VLSliceEntry& b)
                      {
                          return a.sample_start < b.sample_start;
                      });
            legacySlices.erase(std::unique(legacySlices.begin(), legacySlices.end(),
                                           [](const VLSliceEntry& a, const VLSliceEntry& b)
                                           {
                                               return a.sample_start == b.sample_start;
                                           }),
                               legacySlices.end());

            const uint32_t sliceEnd = loopEnd > loopStart ? loopEnd : totalFrames;
            for (size_t i = 0; i < legacySlices.size(); ++i)
            {
                const uint32_t next = i + 1 < legacySlices.size() ? legacySlices[i + 1].sample_start : sliceEnd;
                if (!legacySlicesHaveExplicitPpq)
                    legacySlices[i].sample_length = next > legacySlices[i].sample_start ? next - legacySlices[i].sample_start : 1u;
            }

            slices = std::move(legacySlices);
            if (!legacySlicesHaveExplicitPpq)
                finalizeSlices();
            else
                info.slice_count = (int32_t)slices.size();
            finalizeRenderedLengths();
        }
        else
        {
            VLSliceEntry s;
            s.ppq_pos = 0;
            s.sample_start = loopStart;
            s.sample_length = std::max<uint32_t>(1u, loopEnd > loopStart ? loopEnd - loopStart : totalFrames - loopStart);
            s.rendered_length = s.sample_length;
            s.points = 0x7fff;
            s.selected_flag = 1;
            s.state = kSliceNormal;
            slices.push_back(s);
            info.slice_count = (int32_t)slices.size();
            finalizeRenderedLengths();
        }
        return true;
    }

    void parseIFF(size_t start, size_t end, size_t& dwopOffset, size_t& dwopSize, bool& hasDWOP)
    {
        size_t off = start;
        while (off + 8 < end && off + 8 < fileData.size())
        {
            char id[5] = {};
            std::memcpy(id, &fileData[off], 4);
            off += 4;

            const uint32_t sz = be32(&fileData[off]);
            off += 4;

            if (off + sz > fileData.size())
            {
                fail(VL_ERROR_INVALID_SIZE);
                break;
            }

            if (std::strcmp(id, "HEAD") == 0)
                parseHEAD(&fileData[off], sz);

            else if (std::strcmp(id, "CREI") == 0)
                parseCREI(&fileData[off], sz);

            else if (std::strcmp(id, "SINF") == 0)
                parseSINF(&fileData[off], sz);

            else if (std::strcmp(id, "GLOB") == 0)
                parseGLOB(&fileData[off], sz);

            else if (std::strcmp(id, "TRSH") == 0)
                parseTRSH(&fileData[off], sz);

            else if (std::strcmp(id, "RECY") == 0)
                parseRECY(&fileData[off], sz);

            else if (std::strcmp(id, "SLCE") == 0)
                parseSLCE(&fileData[off], sz);

            else if ((std::strcmp(id, "SDAT") == 0 || std::strcmp(id, "DWOP") == 0) && !hasDWOP)
            {
                dwopOffset = off;
                dwopSize = sz;
                hasDWOP = true;
            }

            else if (std::strcmp(id, "CAT ") == 0 && sz >= 4)
            {
                parseIFF(off + 4, off + sz, dwopOffset, dwopSize, hasDWOP);
            }

            off += sz;
            if (off & 1)
                ++off;
        }
    }

    void parseSINF(const uint8_t* d, uint32_t sz)
    {
        if (sz < 18)
            return;

        info.channels = d[0];
        const uint8_t bd = d[1];
        info.sample_rate = (int32_t)be32(d + 2);
        totalFrames = be32(d + 6);
        loopStart = be32(d + 10);
        loopEnd = be32(d + 14);
        info.total_frames = (int32_t)totalFrames;
        info.loop_start = (int32_t)loopStart;
        info.loop_end = (int32_t)loopEnd;

        switch (bd)
        {
            case 1: info.bit_depth = 8; break;

            case 3: info.bit_depth = 16; break;

            case 5: info.bit_depth = 24; break;

            case 7: info.bit_depth = 32; break;

            default: info.bit_depth = 16; break;
        }

        const uint32_t frames = (loopEnd > loopStart) ? (loopEnd - loopStart) : totalFrames;
        if (frames > 0 && info.sample_rate > 0 && info.ppq_length > 0)
        {
            const double beats = (double)info.ppq_length / (double)kREXPPQ;
            const double bpm = beats * 60.0 * (double)info.sample_rate / (double)frames;

            const int32_t t = (int32_t)std::lround(bpm * 1000.0);
            if (t > 0)
                info.original_tempo = t;
        }

        if (info.original_tempo == 0)
            info.original_tempo = info.tempo;

        if (info.channels != 1 && info.channels != 2)
            info.channels = 1;
    }

    void parseHEAD(const uint8_t* d, uint32_t sz)
    {
        if (sz < 6 || be32(d) != 0x490cf18du)
        {
            headerValid = false;
            fail(VL_ERROR_FILE_CORRUPT);
            return;
        }

        if (d[4] != 0xbc)
        {
            headerValid = false;
            fail(VL_ERROR_FILE_CORRUPT);
            return;
        }

        if (d[5] > 0x03)
        {
            headerValid = false;
            fail(VL_ERROR_FILE_TOO_NEW);
        }
    }

    void parseGLOB(const uint8_t* d, uint32_t sz)
    {
        if (sz < 22)
        {
            fail(VL_ERROR_INVALID_SIZE);
            return;
        }

        info.slice_count = (int32_t)be32(d);
        globBars = be16(d + 4);
        globBeats = d[6];
        info.time_sig_num = d[7];
        info.time_sig_den = d[8];
        analysisSensitivity = d[9];
        gateSensitivity = be16(d + 10);
        const uint32_t tempo = be32(d + 16);
        if (tempo < 20000u || tempo > 450000u)
            fail(VL_ERROR_INVALID_TEMPO);
        info.tempo = (int32_t)tempo;
        {
            const int32_t timeSigNum = info.time_sig_num > 0 ? info.time_sig_num : 4;
            const int32_t totalBeats = (int32_t)globBars * timeSigNum + (int32_t)globBeats;
            info.ppq_length = totalBeats > 0 ? totalBeats * kREXPPQ : 4 * kREXPPQ;
        }
        processingGain = be16(d + 12);
        silenceSelected = d[21] != 0;
        info.processing_gain = processingGain;
        info.silence_selected = silenceSelected ? 1 : 0;
        loadedFromFile = true;
    }

    void parseCREI(const uint8_t* d, uint32_t sz)
    {
        uint32_t off = 0;
        auto readString = [&](char* dst, size_t dstSize)
        {
            if (!dst || dstSize == 0)
                return;

            dst[0] = '\0';
            if (off + 4 > sz)
                return;

            const uint32_t n = be32(d + off);
            off += 4;
            if (off + n > sz)
            {
                off = sz;
                return;
            }

            const size_t copy = std::min<size_t>(n, dstSize - 1);
            if (copy)
                std::memcpy(dst, d + off, copy);

            dst[copy] = '\0';
            off += n;
        };

        readString(creator.name, sizeof(creator.name));
        readString(creator.copyright, sizeof(creator.copyright));
        readString(creator.url, sizeof(creator.url));
        readString(creator.email, sizeof(creator.email));
        readString(creator.free_text, sizeof(creator.free_text));
    }

    void parseTRSH(const uint8_t* d, uint32_t sz)
    {
        if (sz < 7)
            return;

        transientEnabled = d[0] != 0;
        transientAttack = be16(d + 1);
        transientDecay = be16(d + 3);
        transientStretch = be16(d + 5);
        info.transient_enabled = transientEnabled ? 1 : 0;
        info.transient_attack = transientAttack;
        info.transient_decay = transientDecay;
        info.transient_stretch = transientStretch;
    }

    void parseRECY(const uint8_t* d, uint32_t sz)
    {
        if (sz < 12)
            return;

        const int32_t t = (int32_t)be32(d + 8);
        if (t > 0)
            info.original_tempo = t;
    }

    void parseSLCE(const uint8_t* d, uint32_t sz)
    {
        static constexpr size_t kMaxSlices = 1024;

        if (slices.size() >= kMaxSlices)
            return;

        if (sz < 10)
            return;

        VLSliceEntry s;
        s.sample_start = be32(d);
        s.sample_length = be32(d + 4);
        s.points = be16(d + 8);
        s.marker = s.sample_length <= 1;

        const uint8_t flags = sz > 10 ? d[10] : 0;
        s.selected_flag = (flags & 0x04) ? 1 : 0;

        if (flags & 0x02)
            s.state = kSliceLocked;
        else if (flags & 0x01)
            s.state = kSliceMuted;
        else
            s.state = kSliceNormal;

        slices.push_back(s);
        info.slice_count = (int32_t)slices.size();
    }

    static uint16_t rex2FilterPoints(uint8_t sensitivity)
    {
        const uint32_t sens = std::min<uint32_t>(sensitivity, 99u);
        const uint32_t visibleRange = (sens * 0x7fffu + 98u) / 99u;
        return (uint16_t)(0x7fffu - visibleRange);
    }

    bool isVisibleSliceBoundary(const VLSliceEntry& s) const
    {
        if (s.sample_length > 1)
            return true;

        if (s.selected_flag || s.state != kSliceNormal)
            return true;

        return s.points > rex2FilterPoints(analysisSensitivity);
    }

    uint32_t defaultSliceEnd(uint32_t start) const
    {
        if (loopEnd > loopStart && start < loopEnd)
            return loopEnd;

        return totalFrames;
    }

    uint32_t gateLengthFrames() const
    {
        if (gateSensitivity == 0)
            return 0;

        const uint32_t sr = info.sample_rate > 0 ? (uint32_t)info.sample_rate : 44100u;
        uint32_t frames = (uint32_t)(((uint64_t)gateSensitivity * sr + 4500u) / 9000u);
        frames = std::max(1u, frames);

        return std::max(1u, ((frames + 64u) / 128u) * 128u);
    }

    void finalizeSlices()
    {
        const uint32_t denom = (loopEnd > loopStart) ? (loopEnd - loopStart) : (totalFrames ? totalFrames : 1u);
        const uint32_t gatedFrames = gateLengthFrames();

        std::sort(slices.begin(), slices.end(),
                  [](const VLSliceEntry& a, const VLSliceEntry& b)
                  {
                      return a.sample_start < b.sample_start;
                  });

        std::vector<VLSliceEntry> out;
        for (auto s : slices)
        {
            if (loopEnd > loopStart && s.sample_start < totalFrames && s.sample_start >= loopEnd)
                continue;

            if (!isVisibleSliceBoundary(s))
                continue;

            const uint32_t rel = (s.sample_start > loopStart) ? (s.sample_start - loopStart) : 0;

            s.ppq_pos = (uint32_t)(((uint64_t)rel * info.ppq_length + denom / 2) / denom);
            s.marker = false;

            out.push_back(s);
        }

        for (size_t i = 0; i < out.size(); ++i)
        {
            const uint32_t start = out[i].sample_start;
            uint32_t next = defaultSliceEnd(start);
            for (size_t j = i + 1; j < out.size(); ++j)
            {
                if (out[j].sample_start > start)
                {
                    next = out[j].sample_start;
                    break;
                }
            }

            const uint32_t derived = next > start ? next - start : 1u;
            if (gateSensitivity == 0 || out[i].sample_length <= 1)
            {
                out[i].sample_length = derived;
                if (gateSensitivity != 0 && gatedFrames > 0)
                    out[i].sample_length = std::min(out[i].sample_length, gatedFrames);
            }
            else if (derived > 1)
            {
                out[i].sample_length = std::min(out[i].sample_length, derived);
            }

            out[i].sample_length = std::max<uint32_t>(1u, out[i].sample_length);
        }

        if (!out.empty() && loopEnd > loopStart && out.front().sample_start > loopStart && out.front().sample_start <= totalFrames)
        {
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

    uint32_t sourceEndForSlice(const VLSliceEntry& s) const
    {
        const uint32_t start = s.sample_start;
        if (start >= totalFrames)
            return start;

        uint32_t frames = std::min(s.sample_length, totalFrames - start);
        if (loopEnd > loopStart && start < loopEnd)
            frames = std::min(frames, loopEnd - start);

        return start + frames;
    }

public:
    struct SegmentLoop
    {
        uint32_t start = 0;
        uint32_t end = 0;
        float volumeCompensation = 1.0f;
    };

    SegmentLoop findSegmentLoop(uint32_t start, uint32_t end) const
    {
        SegmentLoop r{start, end};

        const uint32_t sr = info.sample_rate ? (uint32_t)info.sample_rate : 44100u;
        const uint32_t srch = std::max(1u, (400u * sr) / 44100u);
        const uint32_t mhl = std::max(1u, (20000u * sr) / 44100u);

        if (end <= start || end - start < srch * 3u)
            return r;

        const uint32_t ch = std::max(1, info.channels);
        auto leftAbs = [&](uint32_t f) -> int
        {
            const size_t i = (size_t)f * ch;
            return i < pcm.size() ? std::abs((int)pcm[i]) : 0;
        };

        uint32_t loopEnd = end - srch;
        int peak = -1;
        for (uint32_t i = 0, f = end - srch; i < srch && f > start; ++i, --f)
        {
            const int p = leftAbs(f);
            if (p > peak)
            {
                peak = p;
                loopEnd = f;
            }
        }

        uint32_t hl = std::min((loopEnd - start) / 2u, mhl);
        uint32_t ls = loopEnd - hl;
        uint32_t loopStart = ls;
        int lspeak = -1;
        for (uint32_t i = 0, f = ls; i < srch && f >= start; ++i)
        {
            const int p = leftAbs(f);
            if (p > lspeak)
            {
                lspeak = p;
                loopStart = f;
            }

            if (f == 0)
                break;

            --f;
        }

        r.start = std::clamp(loopStart, start, end - 1u);
        r.end = std::clamp(loopEnd, r.start + 1u, end);

        if (lspeak > 0 && peak > 0)
            r.volumeCompensation = std::min(10.0f, (float)peak / (float)lspeak);

        return r;
    }

private:
    void cacheRenderLoop(VLSliceEntry& s) const
    {
        const uint32_t start = s.sample_start;
        const uint32_t end = sourceEndForSlice(s);
        const SegmentLoop loop = findSegmentLoop(start, end);
        s.render_loop_start = loop.start;
        s.render_loop_end = loop.end;
        s.render_loop_volume_compensation = loop.volumeCompensation;
    }

    uint32_t calcRenderedLength(const VLSliceEntry& s) const
    {
        const uint32_t start = s.sample_start;
        const uint32_t end = sourceEndForSlice(s);

        if (end <= start)
            return 1u;

        const uint32_t segLen = end - start;
        if (!transientEnabled || transientStretch == 0)
            return segLen;

        const uint32_t loopE = (s.render_loop_end > start && s.render_loop_end <= end) ? s.render_loop_end : end;
        const uint32_t stretchN = (uint32_t)transientStretch + 1u;
        const uint32_t stretchT = (uint32_t)(((uint64_t)(loopE - start) * stretchN) / 100u);

        return std::max(1u, segLen + stretchT);
    }

public:
    void cacheSliceRender(VLSliceEntry& s) const
    {
        cacheRenderLoop(s);
        s.rendered_length = calcRenderedLength(s);
    }

    void cacheSliceLoop(VLSliceEntry& s) const
    {
        cacheRenderLoop(s);
    }

    void finalizeRenderedLengths()
    {
        for (auto& s : slices)
            cacheSliceRender(s);
    }
};

int32_t storageBitDepth(int32_t bitDepth)
{
    return bitDepth == 24 ? 24 : 16;
}

int32_t floatToPCM(float s, int32_t bitDepth)
{
    if (s > 1.0f)
        s = 1.0f;

    if (s < -1.0f)
        s = -1.0f;

    if (storageBitDepth(bitDepth) == 24)
    {
        const float scaled = s >= 0.0f ? s * 8388607.0f : s * 8388608.0f;
        return (int32_t)std::lround(scaled);
    }

    const float scaled = s >= 0.0f ? s * 32767.0f : s * 32768.0f;
    return (int32_t)std::lround(scaled);
}

float pcmToFloat(int32_t sample, int32_t bitDepth)
{
    return storageBitDepth(bitDepth) == 24 ? (float)sample / 8388608.0f : (float)sample / 32768.0f;
}

uint8_t bitDepthCode(int32_t bitDepth)
{
    switch (bitDepth)
    {
        case 8: return 1; // LCOV_EXCL_LINE unsupported authored depth is normalized before writing.

        case 16: return 3;

        case 24: return 5;

        case 32: return 7; // LCOV_EXCL_LINE unsupported authored depth is normalized before writing.

        default: return 3; // LCOV_EXCL_LINE unsupported authored depth is normalized before writing.
    }
}

bool hasCreatorInfo(const VLCreatorInfo& c)
{
    return c.name[0] || c.copyright[0] || c.url[0] || c.email[0] || c.free_text[0];
}

void writeCStringChunkString(VLIFFWriter& w, const char* s)
{
    const size_t n = s ? std::min<size_t>(std::strlen(s), 255) : 0;

    w.put32((uint32_t)n);

    if (n)
        w.putBytes((const uint8_t*)s, n);
}

void writeSimpleChunk(VLIFFWriter& w, const char id[4], const std::vector<uint8_t>& payload)
{
    const size_t c = w.beginChunk(id);

    if (!payload.empty())
        w.putBytes(payload.data(), payload.size());

    w.endChunk(c);
}

uint32_t calcSampleStartFromPPQ(const VLFileImpl& impl, uint32_t ppq)
{
    const VLFileInfo& info = impl.info;
    if (info.ppq_length > 0 && info.loop_end > info.loop_start)
    {
        const uint32_t denom = (uint32_t)(info.loop_end - info.loop_start);
        return (uint32_t)info.loop_start + (uint32_t)(((uint64_t)ppq * denom + (uint32_t)info.ppq_length / 2u) / (uint32_t)info.ppq_length);
    }

    const uint32_t tempo = info.tempo > 0 ? (uint32_t)info.tempo : 120000u;
    const uint32_t sr = info.sample_rate > 0 ? (uint32_t)info.sample_rate : 44100u;
    return (uint32_t)(((uint64_t)ppq * sr * 60000u + (uint64_t)tempo * VLFileImpl::kREXPPQ / 2u) / ((uint64_t)tempo * VLFileImpl::kREXPPQ));
}

void normaliseInfoForSave(VLFileImpl& impl)
{
    impl.info.channels = std::clamp(impl.info.channels, 1, 2);

    if (impl.info.sample_rate <= 0)
        impl.info.sample_rate = 44100;

    if (impl.info.tempo <= 0)
        impl.info.tempo = 120000;

    if (impl.info.original_tempo <= 0)
        impl.info.original_tempo = impl.info.tempo;

    if (impl.info.time_sig_num <= 0)
        impl.info.time_sig_num = 4;

    if (impl.info.time_sig_den <= 0)
        impl.info.time_sig_den = 4;

    impl.info.bit_depth = storageBitDepth(impl.info.bit_depth);
    impl.info.slice_count = (int32_t)impl.slices.size();

    impl.totalFrames = (uint32_t)(impl.pcm.size() / (size_t)impl.info.channels);
    impl.info.total_frames = (int32_t)impl.totalFrames;

    if (impl.info.loop_start < 0 || (uint32_t)impl.info.loop_start >= impl.totalFrames)
        impl.info.loop_start = 0;

    if (impl.info.loop_end <= impl.info.loop_start || (uint32_t)impl.info.loop_end > impl.totalFrames)
        impl.info.loop_end = (int32_t)impl.totalFrames;

    impl.loopStart = (uint32_t)std::max(0, impl.info.loop_start);
    impl.loopEnd = (uint32_t)std::max(impl.info.loop_start, impl.info.loop_end);

    if (!impl.loadedFromFile && impl.analysisSensitivity == 0)
        impl.analysisSensitivity = 99;

    if (impl.info.ppq_length <= 0)
    {
        const uint32_t frames = impl.loopEnd > impl.loopStart ? impl.loopEnd - impl.loopStart : impl.totalFrames;

        const double beats = frames > 0 ? ((double)frames * (double)impl.info.tempo) / (60000.0 * (double)impl.info.sample_rate) : 4.0;

        impl.info.ppq_length = std::max(1, (int32_t)std::lround(beats * VLFileImpl::kREXPPQ));
    }

    {
        const int32_t timeSigNum = impl.info.time_sig_num > 0 ? impl.info.time_sig_num : 4;
        const int32_t totalBeats = std::max(1, (impl.info.ppq_length + VLFileImpl::kREXPPQ / 2) / VLFileImpl::kREXPPQ);
        impl.globBars = (uint16_t)(totalBeats / timeSigNum);
        impl.globBeats = (uint8_t)(totalBeats % timeSigNum);
    }

    impl.processingGain = (uint16_t)std::clamp(impl.info.processing_gain > 0 ? impl.info.processing_gain : 1000, 0, 1000);
    impl.transientEnabled = impl.info.transient_enabled != 0;
    impl.transientAttack = (uint16_t)std::clamp(impl.info.transient_attack, 0, 1023);
    impl.transientDecay = (uint16_t)std::clamp(impl.info.transient_decay > 0 ? impl.info.transient_decay : 1023, 0, 1023);
    impl.transientStretch = (uint16_t)std::clamp(impl.info.transient_stretch, 0, 100);
    impl.silenceSelected = impl.info.silence_selected != 0;
    impl.finalizeRenderedLengths();
}

std::vector<uint8_t> buildREX2File(VLFileImpl& impl)
{
    normaliseInfoForSave(impl);

    VLDWOPCompressor comp;
    const std::vector<uint8_t> sdat =
        impl.info.channels == 2 ? comp.compressStereo(impl.pcm.data(), impl.totalFrames) : comp.compressMono(impl.pcm.data(), impl.totalFrames);

    VLIFFWriter w;
    const size_t root = w.beginCat("REX2");

    {
        const size_t c = w.beginChunk("HEAD");
        const uint8_t head[] = {0x49, 0x0c, 0xf1, 0x8d, 0xbc, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        w.putBytes(head, sizeof(head));
        w.endChunk(c);
    }

    if (hasCreatorInfo(impl.creator))
    {
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
        w.put16(impl.globBars);
        w.put8(impl.globBeats);
        w.put8((uint8_t)impl.info.time_sig_num);
        w.put8((uint8_t)impl.info.time_sig_den);
        w.put8(impl.analysisSensitivity);
        const uint16_t gate = impl.slices.empty() ? impl.gateSensitivity : std::max<uint16_t>(1, impl.gateSensitivity);
        w.put16(gate);
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
        w.put32((uint32_t)(impl.totalFrames * impl.info.channels * (impl.info.bit_depth / 8)));
        w.put32((uint32_t)impl.slices.size());
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
        const uint8_t eqPayload[] = {0x00, 0x00, 0x0f, 0x00, 0x64, 0x00, 0x00, 0x03, 0xe8, 0x09, 0xc4, 0x00, 0x00, 0x03, 0xe8, 0x4e, 0x20};
        w.putBytes(eqPayload, sizeof(eqPayload));
        w.endChunk(eq);

        const size_t comp = w.beginChunk("COMP");
        const uint8_t compPayload[] = {0x00, 0x00, 0x4d, 0x00, 0x27, 0x00, 0x42, 0x00, 0x38};
        w.putBytes(compPayload, sizeof(compPayload));
        w.endChunk(comp);

        w.endChunk(devl);
    }

    {
        const size_t slcl = w.beginCat("SLCL");
        std::vector<VLSliceEntry> sorted = impl.slices;
        std::sort(sorted.begin(), sorted.end(),
                  [](const VLSliceEntry& a, const VLSliceEntry& b)
                  {
                      return a.sample_start < b.sample_start;
                  });
        for (const auto& s : sorted)
        {
            const size_t c = w.beginChunk("SLCE");
            w.put32(s.sample_start);
            w.put32(std::max<uint32_t>(1, s.sample_length));
            w.put16(s.points);
            uint8_t flags = 0;
            if (s.state == kSliceMuted)
                flags |= 0x01;
            else if (s.state == kSliceLocked)
                flags |= 0x02;
            if (s.selected_flag)
                flags |= 0x04;
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

struct VLFile_s
{
    VLFileImpl impl;
};

/* -----------------------------------------------------------------------
   Utilities
   ----------------------------------------------------------------------- */

namespace
{

constexpr double kVLTwoPi = 6.283185307179586476925286766559;

bool isPowerOfTwo(int32_t value)
{
    return value > 0 && (value & (value - 1)) == 0;
}

VLSuperFluxOptions superfluxDefaults()
{
    VLSuperFluxOptions o = {};
    o.frame_size = 2048;
    o.fps = 200;
    o.filter_bands = 24;
    o.max_bins = 3;
    o.diff_frames = 0;
    o.min_slice_frames = 0;
    o.filter_equal = 0;
    o.online = 0;
    o.threshold = 1.1f;
    o.combine_ms = 50.0f;
    o.pre_avg = 0.15f;
    o.pre_max = 0.01f;
    o.post_avg = 0.0f;
    o.post_max = 0.05f;
    o.delay_ms = 0.0f;
    o.ratio = 0.5f;
    o.fmin = 30.0f;
    o.fmax = 17000.0f;
    o.log_mul = 1.0f;
    o.log_add = 1.0f;
    return o;
}

VLError validateSuperFluxOptions(const VLSuperFluxOptions& o, int32_t sampleRate)
{
    if (!isPowerOfTwo(o.frame_size) || o.frame_size < 64 || o.frame_size > 32768)
        return VL_ERROR_INVALID_ARG;

    if (o.fps <= 0 || o.fps > sampleRate)
        return VL_ERROR_INVALID_ARG;

    if (o.filter_bands < 1 || o.max_bins < 1)
        return VL_ERROR_INVALID_ARG;

    if (o.diff_frames < 0)
        return VL_ERROR_INVALID_ARG;

    if (o.min_slice_frames < 0)
        return VL_ERROR_INVALID_ARG;

    if (!std::isfinite(o.threshold) || !std::isfinite(o.combine_ms) || !std::isfinite(o.pre_avg) || !std::isfinite(o.pre_max) ||
        !std::isfinite(o.post_avg) || !std::isfinite(o.post_max) || !std::isfinite(o.delay_ms) || !std::isfinite(o.ratio) ||
        !std::isfinite(o.fmin) || !std::isfinite(o.fmax) || !std::isfinite(o.log_mul) || !std::isfinite(o.log_add))
    {
        return VL_ERROR_INVALID_ARG;
    }

    if (o.combine_ms < 0.0f || o.pre_avg < 0.0f || o.pre_max < 0.0f || o.post_avg < 0.0f || o.post_max < 0.0f)
        return VL_ERROR_INVALID_ARG;

    if (o.ratio < 0.0f || o.ratio > 1.0f)
        return VL_ERROR_INVALID_ARG;

    if (o.fmin <= 0.0f || o.fmax <= o.fmin || o.log_mul <= 0.0f || o.log_add <= 0.0f)
        return VL_ERROR_INVALID_ARG;

    return VL_OK;
}

std::vector<float> makeHannWindow(int32_t frameSize)
{
    std::vector<float> window((size_t)frameSize, 1.0f);
    if (frameSize <= 1)
        return window;

    for (int32_t i = 0; i < frameSize; ++i)
        window[(size_t)i] = (float)(0.5 - 0.5 * std::cos(kVLTwoPi * (double)i / (double)(frameSize - 1)));

    return window;
}

std::vector<float> superfluxFrequencies(int32_t bands, float fmin, float fmax)
{
    std::vector<float> frequencies;
    const double factor = std::pow(2.0, 1.0 / (double)bands);

    double freq = 440.0;
    frequencies.push_back((float)freq);
    while (freq <= (double)fmax)
    {
        freq *= factor;
        frequencies.push_back((float)freq);
    }

    freq = 440.0;
    while (freq >= (double)fmin)
    {
        freq /= factor;
        frequencies.push_back((float)freq);
    }

    std::sort(frequencies.begin(), frequencies.end());
    return frequencies;
}

bool buildSuperFluxFilterbank(int32_t numFftBins,
                              int32_t sampleRate,
                              const VLSuperFluxOptions& options,
                              std::vector<float>& filterbank,
                              int32_t& numBands)
{
    const float fmax = std::min(options.fmax, (float)sampleRate * 0.5f);
    std::vector<float> frequencies = superfluxFrequencies(options.filter_bands, options.fmin, fmax);
    const double factor = ((double)sampleRate * 0.5) / (double)numFftBins;

    std::vector<int32_t> bins;
    bins.reserve(frequencies.size());
    for (float frequency : frequencies)
    {
        const int32_t bin = (int32_t)std::lround((double)frequency / factor);
        if (bin >= 0 && bin < numFftBins)
            bins.push_back(bin);
    }

    std::sort(bins.begin(), bins.end());
    bins.erase(std::unique(bins.begin(), bins.end()), bins.end());
    if (bins.size() < 5)
        return false;

    numBands = (int32_t)bins.size() - 2;
    if (numBands < 3)
        return false;

    filterbank.assign((size_t)numFftBins * (size_t)numBands, 0.0f);
    for (int32_t band = 0; band < numBands; ++band)
    {
        const int32_t start = bins[(size_t)band];
        const int32_t mid = bins[(size_t)band + 1u];
        const int32_t stop = bins[(size_t)band + 2u];
        if (mid <= start || stop <= mid)
            continue;

        const float height = options.filter_equal ? 2.0f / (float)(stop - start) : 1.0f;
        for (int32_t bin = start; bin < mid; ++bin)
        {
            const float t = (float)(bin - start) / (float)(mid - start);
            filterbank[(size_t)bin * (size_t)numBands + (size_t)band] = t * height;
        }
        for (int32_t bin = mid; bin < stop; ++bin)
        {
            const float t = (float)(bin - mid) / (float)(stop - mid);
            filterbank[(size_t)bin * (size_t)numBands + (size_t)band] = (1.0f - t) * height;
        }
    }

    return true;
}

int32_t deriveSuperFluxDiffFrames(const std::vector<float>& window, double hopSize, float ratio)
{
    size_t sample = 0;
    while (sample < window.size() && window[sample] <= ratio)
        ++sample;

    const double diffSamples = (double)window.size() * 0.5 - (double)sample;
    return std::max(1, (int32_t)std::lround(diffSamples / hopSize));
}

bool computeSuperFluxActivations(const float* left,
                                 const float* right,
                                 int32_t channels,
                                 int32_t frames,
                                 int32_t sampleRate,
                                 const VLSuperFluxOptions& options,
                                 std::vector<float>& activations)
{
    const int32_t frameSize = options.frame_size;
    const int32_t numFftBins = frameSize / 2;
    const double hopSize = (double)sampleRate / (double)options.fps;
    const int32_t numFrames = std::max(1, (int32_t)std::ceil((double)frames / hopSize));

    std::vector<float> window = makeHannWindow(frameSize);
    std::vector<float> filterbank;
    int32_t numBands = 0;
    if (!buildSuperFluxFilterbank(numFftBins, sampleRate, options, filterbank, numBands))
        return false;

    std::vector<float> spec((size_t)numFrames * (size_t)numBands, 0.0f);
    std::vector<double> fftBuffer((size_t)frameSize);
    std::vector<float> magnitudes((size_t)numFftBins);
    std::vector<int> fftIp(2 + (size_t)frameSize, 0);
    std::vector<double> fftW((size_t)frameSize / 2);

    for (int32_t frame = 0; frame < numFrames; ++frame)
    {
        const int32_t seek = options.online ? (int32_t)((double)(frame + 1) * hopSize - (double)frameSize)
                                            : (int32_t)((double)frame * hopSize - (double)frameSize * 0.5);

        for (int32_t i = 0; i < frameSize; ++i)
        {
            const int32_t src = seek + i;
            float sample = 0.0f;
            if (src >= 0 && src < frames)
            {
                const float l = std::isfinite(left[src]) ? left[src] : 0.0f;
                if (channels == 2)
                {
                    const float r = std::isfinite(right[src]) ? right[src] : 0.0f;
                    sample = (l + r) * 0.5f;
                }
                else
                {
                    sample = l;
                }
            }
            fftBuffer[(size_t)i] = (double)(sample * window[(size_t)i]);
        }

        rdft(frameSize, 1, fftBuffer.data(), fftIp.data(), fftW.data());

        magnitudes[0] = (float)std::abs(fftBuffer[0]);
        for (int32_t bin = 1; bin < numFftBins; ++bin)
        {
            const double re = fftBuffer[(size_t)bin * 2];
            const double im = fftBuffer[(size_t)bin * 2 + 1];
            magnitudes[(size_t)bin] = (float)std::sqrt(re * re + im * im);
        }

        for (int32_t band = 0; band < numBands; ++band)
        {
            double value = 0.0;
            for (int32_t bin = 0; bin < numFftBins; ++bin)
                value += (double)magnitudes[(size_t)bin] * (double)filterbank[(size_t)bin * (size_t)numBands + (size_t)band];

            const float logged = std::log10(options.log_mul * (float)value + options.log_add);
            spec[(size_t)frame * (size_t)numBands + (size_t)band] = logged;
        }
    }

    const int32_t diffFrames =
        options.diff_frames > 0 ? options.diff_frames : deriveSuperFluxDiffFrames(window, hopSize, options.ratio);

    activations.assign((size_t)numFrames, 0.0f);
    for (int32_t frame = diffFrames; frame < numFrames; ++frame)
    {
        float sum = 0.0f;
        const int32_t prevFrame = frame - diffFrames;
        for (int32_t band = 0; band < numBands; ++band)
        {
            int32_t first = band - options.max_bins / 2;
            int32_t last = first + options.max_bins - 1;
            if (first < 0)
            {
                last -= first;
                first = 0;
            }
            if (last >= numBands)
            {
                first = std::max(0, first - (last - numBands + 1));
                last = numBands - 1;
            }
            float prevMax = spec[(size_t)prevFrame * (size_t)numBands + (size_t)band];
            for (int32_t k = first; k <= last; ++k)
                prevMax = std::max(prevMax, spec[(size_t)prevFrame * (size_t)numBands + (size_t)k]);

            const float diff = spec[(size_t)frame * (size_t)numBands + (size_t)band] - prevMax;
            if (diff > 0.0f)
                sum += diff;
        }
        activations[(size_t)frame] = sum;
    }

    return true;
}

std::vector<double> pickSuperFluxOnsets(const std::vector<float>& activations, int32_t fps, const VLSuperFluxOptions& options)
{
    const int32_t count = (int32_t)activations.size();
    const int32_t preAvg = std::max(0, (int32_t)std::lround((double)fps * (double)options.pre_avg));
    const int32_t preMax = std::max(0, (int32_t)std::lround((double)fps * (double)options.pre_max));
    const int32_t postAvg = options.online ? 0 : std::max(0, (int32_t)std::lround((double)fps * (double)options.post_avg));
    const int32_t postMax = options.online ? 0 : std::max(0, (int32_t)std::lround((double)fps * (double)options.post_max));
    const double combineSeconds = (double)options.combine_ms / 1000.0;
    const double delaySeconds = (double)options.delay_ms / 1000.0;

    std::vector<double> detections;
    double lastDetection = -std::numeric_limits<double>::infinity();
    for (int32_t frame = 0; frame < count; ++frame)
    {
        const int32_t maxStart = std::max(0, frame - preMax);
        const int32_t maxStop = std::min(count - 1, frame + postMax);
        float movMax = 0.0f;
        for (int32_t i = maxStart; i <= maxStop; ++i)
            movMax = std::max(movMax, activations[(size_t)i]);

        const int32_t avgStart = std::max(0, frame - preAvg);
        const int32_t avgStop = std::min(count - 1, frame + postAvg);
        double avg = 0.0;
        for (int32_t i = avgStart; i <= avgStop; ++i)
            avg += activations[(size_t)i];
        avg /= (double)(avgStop - avgStart + 1);

        const float value = activations[(size_t)frame];
        if (value <= 0.0f || value != movMax || (double)value < avg + (double)options.threshold)
            continue;

        const double time = (double)frame / (double)fps + delaySeconds;
        if (detections.empty() || time - lastDetection > combineSeconds)
        {
            detections.push_back(time);
            lastDetection = time;
        }
    }

    return detections;
}

std::vector<uint32_t> superfluxSliceBoundaries(const std::vector<double>& detections,
                                               int32_t frames,
                                               int32_t sampleRate,
                                               const VLSuperFluxOptions& options)
{
    const uint32_t combineFrames =
        (uint32_t)std::max(1, (int32_t)std::lround((double)options.combine_ms / 1000.0 * (double)sampleRate));
    const uint32_t minSliceFrames =
        (uint32_t)(options.min_slice_frames > 0 ? (uint32_t)options.min_slice_frames : std::max(combineFrames, (uint32_t)std::max(1, (int32_t)std::lround((double)sampleRate * 0.01))));

    std::vector<uint32_t> boundaries;
    boundaries.push_back(0);

    for (double detection : detections)
    {
        const int64_t rounded = (int64_t)std::llround(detection * (double)sampleRate);
        if (rounded <= 0 || rounded >= frames)
            continue;

        const uint32_t sample = (uint32_t)rounded;
        if (sample - boundaries.back() >= minSliceFrames)
            boundaries.push_back(sample);
    }

    if ((uint32_t)frames - boundaries.back() < minSliceFrames && boundaries.size() > 1)
        boundaries.pop_back();

    boundaries.push_back((uint32_t)frames);
    return boundaries;
}

int32_t addSliceAtSample(VLFile file, uint32_t sample_start, int32_t ppq_pos, const float* left, const float* right, int32_t frames)
{
    if (!file)
        return (int32_t)VL_ERROR_INVALID_HANDLE;

    if (!left || frames <= 0 || ppq_pos < 0)
        return (int32_t)VL_ERROR_INVALID_ARG;

    const int32_t channels = std::clamp(file->impl.info.channels, 1, 2);
    if (channels == 2 && !right)
        return (int32_t)VL_ERROR_INVALID_ARG;

    const uint32_t end = sample_start + (uint32_t)frames;
    const uint32_t declaredFrames = file->impl.info.total_frames > 0 ? (uint32_t)file->impl.info.total_frames : 0u;
    const uint32_t requiredFrames = std::max(end, declaredFrames);
    const size_t required = (size_t)requiredFrames * (size_t)channels;

    if (required > file->impl.pcm.max_size())
        return (int32_t)VL_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE max_size is not reachable in tests.

    if (file->impl.pcm.size() < required)
    {
        if (!file->impl.pcm.resize(required, 0))
            return (int32_t)VL_ERROR_OUT_OF_MEMORY;
    }

    for (int32_t f = 0; f < frames; ++f)
    {
        const size_t dst = ((size_t)sample_start + (size_t)f) * (size_t)channels;
        file->impl.pcm[dst] = floatToPCM(left[f], file->impl.info.bit_depth);
        if (channels == 2)
            file->impl.pcm[dst + 1] = floatToPCM(right[f], file->impl.info.bit_depth);
    }

    VLSliceEntry s;
    s.ppq_pos = (uint32_t)ppq_pos;
    s.sample_start = sample_start;
    s.sample_length = (uint32_t)frames;
    s.rendered_length = (uint32_t)frames;
    s.points = 0x7fff;
    s.selected_flag = 0;
    s.state = kSliceNormal;

    file->impl.totalFrames = (uint32_t)(file->impl.pcm.size() / (size_t)channels);
    file->impl.info.total_frames = (int32_t)file->impl.totalFrames;
    file->impl.cacheSliceLoop(s);

    if (file->impl.slices.size() == file->impl.slices.max_size())
        return (int32_t)VL_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE max_size is not reachable in tests.
    file->impl.slices.push_back(s);

    file->impl.info.slice_count = (int32_t)file->impl.slices.size();
    if (file->impl.info.loop_end <= file->impl.info.loop_start)
        file->impl.info.loop_end = file->impl.info.total_frames;

    return (int32_t)file->impl.slices.size() - 1;
}

} // anonymous namespace

/* -----------------------------------------------------------------------
   Open / close
   ----------------------------------------------------------------------- */

VLFile vl_open(const char* path, VLError* err)
{
    auto set = [&](VLError e)
    {
        if (err)
            *err = e;
    };

    if (!path)
    {
        set(VL_ERROR_INVALID_ARG);
        return nullptr;
    }

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
    {
        set(VL_ERROR_FILE_NOT_FOUND);
        return nullptr;
    }

    const std::streamsize sz = f.tellg();
    f.seekg(0);

    std::vector<char> buf((size_t)sz);
    if (!f.read(buf.data(), sz))
    {
        set(VL_ERROR_FILE_CORRUPT);
        return nullptr;
    }

    return vl_open_from_memory(buf.data(), (size_t)sz, err);
}

VLFile vl_open_from_memory(const void* data, size_t size, VLError* err)
{
    auto set = [&](VLError e)
    {
        if (err)
            *err = e;
    };

    if (!data || size == 0)
    {
        set(VL_ERROR_INVALID_ARG);
        return nullptr;
    }

    VLFile_s* h = new (std::nothrow) VLFile_s();
    if (!h)
    {
        set(VL_ERROR_OUT_OF_MEMORY);
        return nullptr;
    }

    if (!h->impl.loadFromBuffer((const char*)data, size))
    {
        const VLError loadError = h->impl.loadError != VL_OK ? h->impl.loadError : VL_ERROR_FILE_CORRUPT;
        delete h;
        set(loadError);
        return nullptr;
    }

    set(VL_OK);
    return h;
}

VLFile vl_create_new(int32_t channels, int32_t sample_rate, int32_t tempo, VLError* err)
{
    auto set = [&](VLError e)
    {
        if (err)
            *err = e;
    };

    if (channels != 1 && channels != 2)
    {
        set(VL_ERROR_INVALID_ARG);
        return nullptr;
    }

    if (sample_rate < 8000 || sample_rate > 192000)
    {
        set(VL_ERROR_INVALID_SAMPLE_RATE);
        return nullptr;
    }

    if (tempo <= 0)
    {
        set(VL_ERROR_INVALID_TEMPO);
        return nullptr;
    }

    VLFile_s* h = new (std::nothrow) VLFile_s();
    if (!h)
    {
        set(VL_ERROR_OUT_OF_MEMORY);
        return nullptr;
    }

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

void vl_superflux_default_options(VLSuperFluxOptions* out)
{
    if (out)
        *out = superfluxDefaults();
}

VLFile vl_create_from_superflux(int32_t channels,
                                int32_t sample_rate,
                                int32_t tempo,
                                const float* left,
                                const float* right,
                                int32_t frames,
                                const VLSuperFluxOptions* options,
                                VLError* err)
{
    auto set = [&](VLError e)
    {
        if (err)
            *err = e;
    };

    if (channels != 1 && channels != 2)
    {
        set(VL_ERROR_INVALID_ARG);
        return nullptr;
    }

    if (sample_rate < 8000 || sample_rate > 192000)
    {
        set(VL_ERROR_INVALID_SAMPLE_RATE);
        return nullptr;
    }

    if (tempo <= 0)
    {
        set(VL_ERROR_INVALID_TEMPO);
        return nullptr;
    }

    if (!left || frames <= 0 || (channels == 2 && !right))
    {
        set(VL_ERROR_INVALID_ARG);
        return nullptr;
    }

    VLSuperFluxOptions opts = options ? *options : superfluxDefaults();
    VLError optionError = validateSuperFluxOptions(opts, sample_rate);
    if (optionError != VL_OK)
    {
        set(optionError);
        return nullptr;
    }

    try
    {
        std::vector<float> activations;
        if (!computeSuperFluxActivations(left, right, channels, frames, sample_rate, opts, activations))
        {
            set(VL_ERROR_INVALID_ARG);
            return nullptr;
        }

        const std::vector<double> detections = pickSuperFluxOnsets(activations, opts.fps, opts);
        const std::vector<uint32_t> boundaries = superfluxSliceBoundaries(detections, frames, sample_rate, opts);

        VLError createError = VL_OK;
        VLFile file = vl_create_new(channels, sample_rate, tempo, &createError);
        if (!file)
        {
            set(createError);
            return nullptr;
        }

        VLFileInfo info = {};
        if (vl_get_info(file, &info) != VL_OK)
        {
            vl_close(file);
            set(VL_ERROR_INVALID_HANDLE); // LCOV_EXCL_LINE freshly-created handles are valid.
            return nullptr;
        }

        const double beats = ((double)frames * (double)tempo) / (60000.0 * (double)sample_rate);
        info.ppq_length = std::max(1, (int32_t)std::lround(beats * (double)VLFileImpl::kREXPPQ));
        info.total_frames = frames;
        info.loop_start = 0;
        info.loop_end = frames;
        info.transient_enabled = 0;
        info.transient_stretch = 0;
        info.transient_attack = 0;
        info.transient_decay = 1023;
        VLError infoError = vl_set_info(file, &info);
        if (infoError != VL_OK)
        {
            vl_close(file);
            set(infoError);
            return nullptr;
        }

        int32_t previousPpq = -1;
        for (size_t i = 0; i + 1 < boundaries.size(); ++i)
        {
            const uint32_t start = boundaries[i];
            const uint32_t stop = boundaries[i + 1u];
            if (stop <= start)
                continue;

            int32_t ppq = (int32_t)(((uint64_t)start * (uint64_t)info.ppq_length + (uint32_t)frames / 2u) / (uint32_t)frames);
            if (ppq <= previousPpq)
                ppq = previousPpq + 1;
            ppq = std::min(ppq, std::max(0, info.ppq_length - 1));
            previousPpq = ppq;

            const int32_t sliceIndex =
                addSliceAtSample(file, start, ppq, left + start, channels == 2 ? right + start : nullptr, (int32_t)(stop - start));
            if (sliceIndex < 0)
            {
                const VLError sliceError = (VLError)sliceIndex;
                vl_close(file);
                set(sliceError);
                return nullptr;
            }
        }

        if (file->impl.slices.empty())
        {
            vl_close(file);
            set(VL_ERROR_INVALID_ARG); // LCOV_EXCL_LINE boundaries always create at least one slice for frames > 0.
            return nullptr;
        }

        set(VL_OK);
        return file;
    }
    catch (const std::bad_alloc&)
    {
        set(VL_ERROR_OUT_OF_MEMORY);
        return nullptr;
    }
    catch (...)
    {
        set(VL_ERROR_OUT_OF_MEMORY);
        return nullptr;
    }
}

void vl_close(VLFile file)
{
    delete file;
}

/* -----------------------------------------------------------------------
   Read: metadata
   ----------------------------------------------------------------------- */

VLError vl_get_info(VLFile file, VLFileInfo* out)
{
    if (!file)
        return VL_ERROR_INVALID_HANDLE;

    if (!out)
        return VL_ERROR_INVALID_ARG;

    *out = file->impl.info;
    return VL_OK;
}

VLError vl_get_creator_info(VLFile file, VLCreatorInfo* out)
{
    if (!file)
        return VL_ERROR_INVALID_HANDLE;

    if (!out)
        return VL_ERROR_INVALID_ARG;

    const VLCreatorInfo& src = file->impl.creator;
    if (!src.name[0] && !src.copyright[0] && !src.url[0] && !src.email[0] && !src.free_text[0])
        return VL_ERROR_NO_CREATOR_INFO;

    *out = src;
    return VL_OK;
}

/* -----------------------------------------------------------------------
   Read: slice enumeration
   ----------------------------------------------------------------------- */

VLError vl_get_slice_info(VLFile file, int32_t index, VLSliceInfo* out)
{
    if (!file)
        return VL_ERROR_INVALID_HANDLE;

    if (!out)
        return VL_ERROR_INVALID_ARG;

    if (index < 0 || (size_t)index >= file->impl.slices.size())
        return VL_ERROR_INVALID_SLICE;

    const VLSliceEntry& s = file->impl.slices[(size_t)index];
    out->ppq_pos = (int32_t)s.ppq_pos;
    out->sample_length = (int32_t)s.sample_length;
    out->sample_start = (int32_t)s.sample_start;
    out->analysis_points = (int32_t)s.points;
    out->flags = sliceFlags(s);
    return VL_OK;
}

VLError vl_set_slice_info(VLFile file, int32_t index, int32_t flags, int32_t analysis_points)
{
    if (!file)
        return VL_ERROR_INVALID_HANDLE;

    if (index < 0 || (size_t)index >= file->impl.slices.size())
        return VL_ERROR_INVALID_SLICE;

    VLError err = applySliceFlags(file->impl.slices[(size_t)index], flags, analysis_points);
    if (err != VL_OK)
        return err;

    return VL_OK;
}

/* -----------------------------------------------------------------------
   Read: sample extraction
   ----------------------------------------------------------------------- */

int32_t vl_get_slice_frame_count(VLFile file, int32_t index)
{
    if (!file)
        return (int32_t)VL_ERROR_INVALID_HANDLE;

    if (index < 0 || (size_t)index >= file->impl.slices.size())
        return (int32_t)VL_ERROR_INVALID_SLICE;

    return (int32_t)file->impl.slices[(size_t)index].rendered_length;
}

VLError vl_decode_slice(VLFile file, int32_t index, float* left, float* right, int32_t frame_offset, int32_t capacity, int32_t* frames_out)
{
    if (!file)
        return VL_ERROR_INVALID_HANDLE;

    if (!left)
        return VL_ERROR_INVALID_ARG;

    if (frame_offset < 0)
        return VL_ERROR_INVALID_ARG;

    if (index < 0 || (size_t)index >= file->impl.slices.size())
        return VL_ERROR_INVALID_SLICE;

    const VLSliceEntry& s = file->impl.slices[(size_t)index];
    const int32_t renderedFrames = (int32_t)s.rendered_length;
    if (frame_offset > renderedFrames)
        return VL_ERROR_INVALID_ARG;

    const int32_t needed = renderedFrames - frame_offset;
    if (capacity < needed)
        return VL_ERROR_BUFFER_TOO_SMALL;

    const auto& pcm = file->impl.pcm;
    if (pcm.empty())
        return VL_ERROR_FILE_CORRUPT;

    const uint32_t requestedFrames = (uint32_t)renderedFrames;
    uint32_t sourceStart = s.sample_start;
    if (sourceStart >= file->impl.totalFrames)
    {
        std::fill(left, left + needed, 0.f);

        if (right)
            std::fill(right, right + needed, 0.f);

        if (frames_out)
            *frames_out = needed;

        return VL_OK;
    }

    uint32_t sourceFrames = s.sample_length;
    if (file->impl.loopEnd > file->impl.loopStart && sourceStart < file->impl.loopEnd)
        sourceFrames = std::min<uint32_t>(sourceFrames, file->impl.loopEnd - sourceStart);
    sourceFrames = std::min<uint32_t>(sourceFrames, file->impl.totalFrames - sourceStart);

    const uint32_t sourceEnd = sourceStart + sourceFrames;
    const VLFileImpl::SegmentLoop segmentLoop{
        s.render_loop_start,
        s.render_loop_end,
        s.render_loop_volume_compensation,
    };
    const int32_t ch = std::max(1, file->impl.info.channels);
    const float samplerGain = (float)file->impl.processingGain * 0.000833333354f;

    uint32_t samplePos = std::min(sourceStart + 2u, sourceEnd);
    int loopPhase = 0; // 0: forward source, 1: forward loop, 2: backward loop
    bool stretchPhase = false;
    float stretchEnv = 1.0f;
    const uint32_t stretchFrameCount =
        std::max<uint32_t>(1u, sourceEnd - segmentLoop.end + (requestedFrames > sourceFrames ? requestedFrames - sourceFrames : 0u));
    const float stretchEnvDec = 1.0f / (float)stretchFrameCount;
    float loopLevelComp = 1.0f;
    const float loopLevelCompInc =
        (segmentLoop.end > segmentLoop.start) ? (1.0f - segmentLoop.volumeCompensation) / (float)(segmentLoop.end - segmentLoop.start) : 0.0f;

    for (uint32_t f = 0; f < requestedFrames; ++f)
    {
        const size_t src = (size_t)samplePos * (size_t)ch;
        float l = 0.f;
        float r = 0.f;

        if (src < pcm.size())
        {
            const float level = samplerGain * stretchEnv * loopLevelComp;
            l = pcmToFloat(pcm[src], file->impl.info.bit_depth) * level;
            r = (ch >= 2 && src + 1 < pcm.size()) ? pcmToFloat(pcm[src + 1], file->impl.info.bit_depth) * level : l;
        }

        if ((int32_t)f >= frame_offset)
        {
            const size_t dst = (size_t)((int32_t)f - frame_offset);
            left[dst] = l;
            if (right)
                right[dst] = r;
        }

        if (stretchPhase)
        {
            stretchEnv = std::max(0.0f, stretchEnv - stretchEnvDec);
            if (loopPhase == 1)
                loopLevelComp += loopLevelCompInc;
            else if (loopPhase == 2)
                loopLevelComp -= loopLevelCompInc;
        }

        if (loopPhase <= 1)
        {
            ++samplePos;
            if (samplePos >= segmentLoop.end)
            {
                stretchPhase = true;
                loopPhase = 2;
                if (samplePos > 0)
                    --samplePos;
                if (samplePos <= segmentLoop.start)
                    loopPhase = 1;
            }
        }
        else
        {
            if (samplePos > 0)
                --samplePos;
            if (samplePos <= segmentLoop.start)
                loopPhase = 1;
        }
    }

    if (frames_out)
        *frames_out = needed;

    return VL_OK;
}

/* -----------------------------------------------------------------------
   Write: assembly from audio slices
   ----------------------------------------------------------------------- */

VLError vl_set_info(VLFile file, const VLFileInfo* info)
{
    if (!file)
        return VL_ERROR_INVALID_HANDLE;

    if (!info)
        return VL_ERROR_INVALID_ARG;

    if (!file->impl.slices.empty() || !file->impl.pcm.empty())
        return VL_ERROR_ALREADY_HAS_DATA;

    if (info->channels != 1 && info->channels != 2)
        return VL_ERROR_INVALID_ARG;

    if (info->sample_rate < 8000 || info->sample_rate > 192000)
        return VL_ERROR_INVALID_SAMPLE_RATE;

    if (info->tempo <= 0)
        return VL_ERROR_INVALID_TEMPO;

    file->impl.info = *info;
    file->impl.info.channels = info->channels;
    file->impl.info.sample_rate = info->sample_rate;
    file->impl.info.bit_depth = storageBitDepth(info->bit_depth);
    file->impl.processingGain = (uint16_t)std::clamp(info->processing_gain > 0 ? info->processing_gain : 1000, 0, 1000);
    file->impl.transientEnabled = info->transient_enabled != 0;
    file->impl.transientAttack = (uint16_t)std::clamp(info->transient_attack, 0, 1023);
    file->impl.transientDecay = (uint16_t)std::clamp(info->transient_decay > 0 ? info->transient_decay : 1023, 0, 1023);
    file->impl.transientStretch = (uint16_t)std::clamp(info->transient_stretch, 0, 100);
    file->impl.silenceSelected = info->silence_selected != 0;

    return VL_OK;
}

VLError vl_set_creator_info(VLFile file, const VLCreatorInfo* info)
{
    if (!file)
        return VL_ERROR_INVALID_HANDLE;

    if (!info)
        return VL_ERROR_INVALID_ARG;

    if (!file->impl.slices.empty() || !file->impl.pcm.empty())
        return VL_ERROR_ALREADY_HAS_DATA;

    file->impl.creator = *info;
    file->impl.creator.name[sizeof(file->impl.creator.name) - 1] = '\0';
    file->impl.creator.copyright[sizeof(file->impl.creator.copyright) - 1] = '\0';
    file->impl.creator.url[sizeof(file->impl.creator.url) - 1] = '\0';
    file->impl.creator.email[sizeof(file->impl.creator.email) - 1] = '\0';
    file->impl.creator.free_text[sizeof(file->impl.creator.free_text) - 1] = '\0';

    return VL_OK;
}

int32_t vl_add_slice(VLFile file, int32_t ppq_pos, const float* left, const float* right, int32_t frames)
{
    if (!file)
        return (int32_t)VL_ERROR_INVALID_HANDLE;

    if (ppq_pos < 0)
        return (int32_t)VL_ERROR_INVALID_ARG;

    const uint32_t start = calcSampleStartFromPPQ(file->impl, (uint32_t)ppq_pos);
    return addSliceAtSample(file, start, ppq_pos, left, right, frames);
}

VLError vl_remove_slice(VLFile file, int32_t index)
{
    if (!file)
        return VL_ERROR_INVALID_HANDLE;

    if (index < 0 || (size_t)index >= file->impl.slices.size())
        return VL_ERROR_INVALID_SLICE;

    file->impl.slices.erase(file->impl.slices.begin() + index);
    file->impl.info.slice_count = (int32_t)file->impl.slices.size();

    return VL_OK;
}

VLError vl_save(VLFile file, const char* path)
{
    if (!file)
        return VL_ERROR_INVALID_HANDLE;

    if (!path)
        return VL_ERROR_INVALID_ARG;

    size_t size = 0;
    VLError e = vl_save_to_memory(file, nullptr, &size);
    if (e != VL_OK)
        return e;

    std::vector<uint8_t> buf(size);
    e = vl_save_to_memory(file, buf.data(), &size);
    if (e != VL_OK)
        return e;

    std::ofstream out(path, std::ios::binary);
    if (!out)
        return VL_ERROR_INVALID_ARG;

    out.write((const char*)buf.data(), (std::streamsize)size);
    return out ? VL_OK : VL_ERROR_FILE_CORRUPT;
}

VLError vl_save_to_memory(VLFile file, void* buf, size_t* size_out)
{
    if (!file)
        return VL_ERROR_INVALID_HANDLE;

    if (!size_out)
        return VL_ERROR_INVALID_ARG;

    if (file->impl.pcm.empty())
        return VL_ERROR_INVALID_ARG;

    std::vector<uint8_t> encoded = buildREX2File(file->impl);

    if (!buf)
    {
        *size_out = encoded.size();
        return VL_OK;
    }

    if (*size_out < encoded.size())
    {
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

const char* vl_error_string(VLError err)
{
    switch (static_cast<int32_t>(err))
    {
        case static_cast<int32_t>(VL_OK): return "OK";
        case static_cast<int32_t>(VL_ERROR_INVALID_HANDLE): return "invalid handle";
        case static_cast<int32_t>(VL_ERROR_INVALID_ARG): return "invalid argument";
        case static_cast<int32_t>(VL_ERROR_FILE_NOT_FOUND): return "file not found";
        case static_cast<int32_t>(VL_ERROR_FILE_CORRUPT): return "file corrupt or unsupported format";
        case static_cast<int32_t>(VL_ERROR_OUT_OF_MEMORY): return "out of memory";
        case static_cast<int32_t>(VL_ERROR_INVALID_SLICE): return "invalid slice index";
        case static_cast<int32_t>(VL_ERROR_INVALID_SAMPLE_RATE): return "invalid sample rate";
        case static_cast<int32_t>(VL_ERROR_BUFFER_TOO_SMALL): return "buffer too small";
        case static_cast<int32_t>(VL_ERROR_NO_CREATOR_INFO): return "no creator info available";
        case static_cast<int32_t>(VL_ERROR_NOT_IMPLEMENTED): return "not implemented";
        case static_cast<int32_t>(VL_ERROR_ALREADY_HAS_DATA): return "already has data";
        case static_cast<int32_t>(VL_ERROR_FILE_TOO_NEW): return "file too new";
        case static_cast<int32_t>(VL_ERROR_ZERO_LOOP_LENGTH): return "zero loop length";
        case static_cast<int32_t>(VL_ERROR_INVALID_SIZE): return "invalid size";
        case static_cast<int32_t>(VL_ERROR_INVALID_TEMPO): return "invalid tempo";
        default: return "unknown error";
    }
}

const char* vl_version_string(void)
{
    return "0.2.0";
}
