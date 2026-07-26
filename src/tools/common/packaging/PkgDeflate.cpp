//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/PkgDeflate.cpp
// Purpose: Self-contained DEFLATE compression and decompression. Ported from
//          src/runtime/io/rt_compress.c with GC dependencies removed.
//
// Key invariants:
//   - All memory management uses malloc/free internally, results returned
//     as std::vector<uint8_t>.
//   - Thread-safe: no global mutable state except lazily-initialized fixed
//     Huffman trees (built once, read-only after init).
//
// Ownership/Lifetime:
//   - Internal buffers are freed before return or on exception.
//   - Returned vectors own their memory.
//
// Links: src/runtime/io/rt_compress.c (original), PkgDeflate.hpp (API)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements self-contained raw DEFLATE compression and decompression.
/// @details Provides bounded LSB-first bit streams, canonical Huffman decoding,
///          LZ77 matching, stored/fixed block compression, and all RFC 1951 block decoders.

#include "PkgDeflate.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>

namespace zanna::pkg {

//=============================================================================
// Constants
//=============================================================================

static constexpr int kMinLevel = 1;
static constexpr int kMaxLevel = 9;

static constexpr int kWindowSize = 32768;
static constexpr int kWindowMask = 0x7FFF;
static constexpr int kMaxMatchLen = 258;
static constexpr int kMinMatchLen = 3;
static constexpr int kMaxDistance = 32768;

static constexpr int kMaxBits = 15;
static constexpr int kMaxLitCodes = 286;
static constexpr int kMaxDistCodes = 30;
static constexpr int kMaxCodeLenCodes = 19;

static constexpr int kFixedLitCodes = 288;
static constexpr int kFixedDistCodes = 32;

//=============================================================================
// Bit Stream Reader (for decompression)
//=============================================================================

/// @brief LSB-first bit stream reader over a borrowed byte buffer (decompression).
struct BitReader {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint32_t buffer;
    int bitsInBuf;

    /// @brief Point the reader at `d[0..l)` without copying.
    /// @param d Borrowed compressed-data buffer.
    /// @param l Buffer length in bytes.
    void init(const uint8_t *d, size_t l) {
        data = d;
        len = l;
        pos = 0;
        buffer = 0;
        bitsInBuf = 0;
    }

    /// @brief Refill the bit buffer until at least `n` bits are available. Returns false at EOF.
    /// @param n Required number of buffered bits.
    /// @return Whether at least `n` bits are available.
    bool fill(int n) {
        while (bitsInBuf < n) {
            if (pos >= len)
                return false;
            buffer |= static_cast<uint32_t>(data[pos++]) << bitsInBuf;
            bitsInBuf += 8;
        }
        return true;
    }

    /// @brief Consume and return the next `n` bits (LSB first). Throws `DeflateError` at EOF.
    /// @param n Number of low-order bits to consume.
    /// @return Decoded unsigned value.
    /// @throws DeflateError If the input ends before `n` bits are available.
    uint32_t read(int n) {
        if (!fill(n))
            throw DeflateError("inflate: unexpected end of data");
        uint32_t val = buffer & ((1U << n) - 1);
        buffer >>= n;
        bitsInBuf -= n;
        return val;
    }

    /// @brief Return the next `n` bits without consuming them from the buffer.
    /// @param n Number of low-order bits to inspect.
    /// @return Decoded unsigned value without advancing the stream.
    /// @throws DeflateError If the input ends before `n` bits are available.
    uint32_t peek(int n) {
        if (!fill(n))
            throw DeflateError("inflate: unexpected end of data");
        return buffer & ((1U << n) - 1);
    }

    /// @brief Discard `n` bits already peeked from the buffer.
    /// @param n Number of buffered bits to discard.
    /// @throws DeflateError If fewer than `n` bits are buffered.
    void consume(int n) {
        if (bitsInBuf < n)
            throw DeflateError("inflate: unexpected end of data");
        buffer >>= n;
        bitsInBuf -= n;
    }

    /// @brief Discard any partial byte, aligning the stream to the next byte boundary.
    /// Required before reading stored-block length fields (RFC 1951 §3.2.4).
    void align() {
        buffer = 0;
        bitsInBuf = 0;
    }

    /// @brief Return true if there are more bits to read (either buffered or in the byte array).
    bool hasData() const {
        return pos < len || bitsInBuf > 0;
    }
};

//=============================================================================
// Bit Stream Writer (for compression)
//=============================================================================

/// @brief LSB-first bit stream writer backed by a growable malloc buffer (compression).
struct BitWriter {
    uint8_t *data = nullptr;
    size_t capacity = 0;
    size_t len = 0;
    uint32_t buffer = 0;
    int bitsInBuf = 0;

    /// @brief Construct an empty writer with no allocated backing storage.
    BitWriter() = default;

    /// @brief Release the writer's caller-inaccessible backing allocation.
    ~BitWriter() {
        std::free(data);
    }

    /// @brief Disable copying because the writer uniquely owns @ref data.
    BitWriter(const BitWriter &) = delete;
    /// @brief Disable copy assignment because the writer uniquely owns @ref data.
    /// @return This writer; the operation is unavailable.
    BitWriter &operator=(const BitWriter &) = delete;

    /// @brief Allocate the initial backing buffer (minimum 256 bytes). Throws on OOM.
    /// @param initialCap Requested initial capacity, capped at one MiB.
    /// @throws DeflateError If size arithmetic overflows or allocation fails.
    void init(size_t initialCap) {
        constexpr size_t kMaxInitialReserve = 1024u * 1024u;
        if (initialCap > std::numeric_limits<size_t>::max() - 256u)
            throw DeflateError("deflate: output buffer size overflow");
        initialCap = std::min(initialCap, kMaxInitialReserve);
        capacity = initialCap > 256 ? initialCap : 256;
        data = static_cast<uint8_t *>(std::malloc(capacity));
        if (!data)
            throw DeflateError("deflate: memory allocation failed");
        len = 0;
        buffer = 0;
        bitsInBuf = 0;
    }

    /// @brief Grow the backing buffer to accommodate at least `need` more bytes.
    /// @param need Additional writable bytes required.
    /// @throws DeflateError If capacity arithmetic overflows or allocation fails.
    void ensure(size_t need) {
        if (need > std::numeric_limits<size_t>::max() - len)
            throw DeflateError("deflate: output buffer size overflow");
        const size_t required = len + need;
        if (required > capacity) {
            size_t newCap = capacity * 2;
            if (newCap < capacity)
                throw DeflateError("deflate: output buffer size overflow");
            if (newCap < required) {
                if (required > std::numeric_limits<size_t>::max() - 256u)
                    throw DeflateError("deflate: output buffer size overflow");
                newCap = required + 256u;
            }
            auto *newData = static_cast<uint8_t *>(std::realloc(data, newCap));
            if (!newData)
                throw DeflateError("deflate: out of memory");
            data = newData;
            capacity = newCap;
        }
    }

    /// @brief Write `n` bits of `val` (LSB first) to the output stream, flushing complete bytes.
    /// @param val Value whose low `n` bits are emitted.
    /// @param n Number of bits to emit.
    /// @throws DeflateError If output allocation fails.
    void write(uint32_t val, int n) {
        buffer |= val << bitsInBuf;
        bitsInBuf += n;
        while (bitsInBuf >= 8) {
            ensure(1);
            data[len++] = buffer & 0xFF;
            buffer >>= 8;
            bitsInBuf -= 8;
        }
    }

    /// @brief Flush any partial byte in the bit buffer as a zero-padded byte.
    void flush() {
        if (bitsInBuf > 0) {
            ensure(1);
            data[len++] = buffer & 0xFF;
            buffer = 0;
            bitsInBuf = 0;
        }
    }

    /// @brief Copy `srcLen` raw bytes directly to the output buffer, bypassing bit-packing.
    /// Used for stored-block payload bytes that do not need bit-packing.
    /// @param src Source byte buffer.
    /// @param srcLen Number of bytes to append.
    /// @throws std::runtime_error If a non-empty source is null.
    /// @throws DeflateError If output growth fails.
    void writeBytes(const uint8_t *src, size_t srcLen) {
        if (srcLen == 0)
            return;
        if (src == nullptr)
            throw std::runtime_error("deflate: null data pointer for non-empty stored block");
        ensure(srcLen);
        std::memcpy(data + len, src, srcLen);
        len += srcLen;
    }

    /// @brief Release the backing buffer without moving it to a vector.
    /// Called on error paths where the partial result must be discarded.
    void free() {
        std::free(data);
        data = nullptr;
        capacity = 0;
        len = 0;
    }

    /// @brief Move contents to vector and free internal buffer.
    std::vector<uint8_t> toVector() {
        std::vector<uint8_t> result(data, data + len);
        free();
        return result;
    }
};

//=============================================================================
// Huffman Tree
//=============================================================================

/// @brief Canonical Huffman decode table built from per-symbol code lengths.
struct HuffmanTree {
    int maxCode;
    uint16_t *symbols;
    int tableBits;
    size_t tableSize;

    /// @brief Construct an empty Huffman table with no symbol allocation.
    HuffmanTree() : maxCode(0), symbols(nullptr), tableBits(0), tableSize(0) {}

    /// @brief Release the canonical-symbol lookup table.
    ~HuffmanTree() {
        std::free(symbols);
    }

    /// @brief Disable copying because the tree uniquely owns @ref symbols.
    HuffmanTree(const HuffmanTree &) = delete;
    /// @brief Disable copy assignment because the tree uniquely owns @ref symbols.
    /// @return This tree; the operation is unavailable.
    HuffmanTree &operator=(const HuffmanTree &) = delete;

    /// @brief Build a flat lookup table from canonical Huffman code lengths.
    /// Each `symbols[]` entry is encoded as `(len << 12) | symbol`. Only codes up to
    /// `tableBits` in length have direct table entries (RFC 1951 caps all codes at 15 bits).
    /// Returns false if the length array is invalid or empty.
    /// @param lengths Per-symbol code lengths.
    /// @param numCodes Number of symbols represented by `lengths`.
    /// @return Whether a non-empty, non-oversubscribed decode table was built.
    bool build(const uint8_t *lengths, int numCodes) {
        int blCount[kMaxBits + 1] = {};
        int nonZero = 0;
        for (int i = 0; i < numCodes; i++) {
            if (lengths[i] > kMaxBits)
                return false;
            blCount[lengths[i]]++;
            if (lengths[i] != 0)
                ++nonZero;
        }
        if (nonZero == 0)
            return false;
        blCount[0] = 0;

        uint16_t nextCode[kMaxBits + 1];
        uint16_t code = 0;
        int left = 1;
        for (int bits = 1; bits <= kMaxBits; bits++) {
            left <<= 1;
            left -= blCount[bits];
            if (left < 0)
                return false;
            code = static_cast<uint16_t>((code + blCount[bits - 1]) << 1);
            nextCode[bits] = code;
        }

        int maxLen = 0;
        for (int i = 0; i < numCodes; i++) {
            if (lengths[i] > maxLen)
                maxLen = lengths[i];
        }
        if (maxLen == 0)
            maxLen = 1;

        tableBits = maxLen;
        tableSize = static_cast<size_t>(1) << tableBits;
        maxCode = numCodes;

        symbols = static_cast<uint16_t *>(std::calloc(tableSize + numCodes * 2, sizeof(uint16_t)));
        if (!symbols)
            return false;

        for (int i = 0; i < numCodes; i++) {
            if (lengths[i] == 0)
                continue;

            uint16_t symCode = nextCode[lengths[i]]++;
            int len = lengths[i];

            if (len <= tableBits) {
                uint16_t revCode = 0;
                for (int b = 0; b < len; b++) {
                    if (symCode & (1 << b))
                        revCode |= 1 << (len - 1 - b);
                }
                int fill = 1 << (tableBits - len);
                for (int j = 0; j < fill; j++) {
                    int idx = revCode | (j << len);
                    symbols[idx] = static_cast<uint16_t>((len << 12) | i);
                }
            }
        }
        return true;
    }

    /// @brief Decode one symbol from `br` by peeking `tableBits` bits and doing a direct lookup.
    /// Returns -1 if the bit stream is exhausted or the code does not appear in the table.
    /// @param br Bit reader to consume on success.
    /// @return Decoded symbol index, or -1 for invalid/truncated input.
    int decode(BitReader &br) const {
        const bool haveFullTable = br.fill(tableBits);
        if (!haveFullTable && br.bitsInBuf == 0)
            return -1;
        uint32_t bits = br.buffer & ((static_cast<uint32_t>(1) << tableBits) - 1);
        uint16_t entry = symbols[bits];
        if (entry == 0)
            return -1;
        int len = entry >> 12;
        int symbol = entry & 0xFFF;
        if (len == 0 || len > br.bitsInBuf)
            return -1;
        br.consume(len);
        return symbol;
    }
};

//=============================================================================
// Length and Distance Tables
//=============================================================================

static const int kLengthExtraBits[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                         2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

static const int kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};

static const int kDistExtraBits[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static const int kDistBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                  33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                  1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};

static const int kCodeLengthOrder[kMaxCodeLenCodes] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

//=============================================================================
// Fixed Huffman Trees (lazy init, thread-safe enough for our use)
//=============================================================================

static HuffmanTree *sFixedLitTree = nullptr;
static HuffmanTree *sFixedDistTree = nullptr;
static std::once_flag sFixedTreesFlag;

/// @brief Initialize the RFC 1951 §3.2.6 fixed Huffman trees exactly once (thread-safe via
/// `std::call_once`). The trees are heap-allocated and intentionally never freed — read-only after
/// initialization.
static void initFixedTrees() {
    /// @brief Construct and publish the process-wide fixed Huffman trees.
    /// @details This callback executes at most once through `std::call_once`.
    std::call_once(sFixedTreesFlag, []() {
        // Literal/length code lengths (RFC 1951 section 3.2.6)
        auto lit = std::make_unique<HuffmanTree>();
        uint8_t litLengths[kFixedLitCodes];
        for (int i = 0; i <= 143; i++)
            litLengths[i] = 8;
        for (int i = 144; i <= 255; i++)
            litLengths[i] = 9;
        for (int i = 256; i <= 279; i++)
            litLengths[i] = 7;
        for (int i = 280; i <= 287; i++)
            litLengths[i] = 8;
        lit->build(litLengths, kFixedLitCodes);

        // Distance code lengths (all 5 bits)
        auto dist = std::make_unique<HuffmanTree>();
        uint8_t distLengths[kFixedDistCodes];
        for (int i = 0; i < kFixedDistCodes; i++)
            distLengths[i] = 5;
        dist->build(distLengths, kFixedDistCodes);

        sFixedLitTree = lit.release();
        sFixedDistTree = dist.release();
    });
}

//=============================================================================
// Output Buffer (for decompression)
//=============================================================================

static constexpr size_t kInflateMaxOutput = 256u * 1024u * 1024u;

/// @brief Growable decompression output buffer with a hard size cap (anti-zip-bomb).
struct OutputBuffer {
    uint8_t *data;
    size_t len;
    size_t capacity;
    size_t maxOutput;

    /// @brief Allocate the initial decompression buffer, capped at `maxOutputBytes`.
    /// @param initialCap Requested starting capacity.
    /// @param maxOutputBytes Hard upper bound on decompressed output.
    /// @throws DeflateError If allocation fails.
    void init(size_t initialCap, size_t maxOutputBytes) {
        maxOutput = maxOutputBytes;
        if (initialCap > maxOutput)
            initialCap = maxOutput;
        capacity = initialCap > 256 ? initialCap : 256;
        data = static_cast<uint8_t *>(std::malloc(capacity));
        if (!data)
            throw DeflateError("inflate: memory allocation failed");
        len = 0;
    }

    /// @brief Grow the decompression buffer; throws `DeflateError` if the output size limit is
    /// reached.
    /// @param need Additional output bytes required.
    /// @throws DeflateError If the limit would be exceeded or allocation fails.
    void ensure(size_t need) {
        if (need > maxOutput || len > maxOutput - need) {
            const size_t maxMB = maxOutput / (1024u * 1024u);
            throw DeflateError("inflate: output exceeds " + std::to_string(maxMB) + " MB limit");
        }
        if (len + need > capacity) {
            size_t newCap = capacity * 2;
            if (newCap < len + need)
                newCap = len + need + 256;
            if (newCap > maxOutput)
                newCap = maxOutput;
            auto *newData = static_cast<uint8_t *>(std::realloc(data, newCap));
            if (!newData)
                throw DeflateError("inflate: out of memory");
            data = newData;
            capacity = newCap;
        }
    }

    /// @brief Append a single literal byte to the decompressed output buffer.
    /// @param b Literal byte to append.
    /// @throws DeflateError If the output limit or allocation prevents growth.
    void putByte(uint8_t b) {
        ensure(1);
        data[len++] = b;
    }

    /// @brief Expand an LZ77 back-reference by copying `length` bytes from `distance` bytes back.
    /// Copies byte-by-byte because src and dst may overlap (e.g. RLE runs).
    /// @param distance Backward distance from the current output end.
    /// @param length Number of bytes to reproduce.
    /// @throws DeflateError If the output limit or allocation prevents growth.
    void copyBack(int distance, int length) {
        ensure(length);
        size_t src = len - distance;
        for (int i = 0; i < length; i++)
            data[len++] = data[src++];
    }

    /// @brief Move decompressed output to a vector and free the internal buffer.
    std::vector<uint8_t> toVector() {
        std::vector<uint8_t> result(data, data + len);
        std::free(data);
        data = nullptr;
        return result;
    }

    /// @brief Free the internal buffer — called on error paths where output is discarded.
    void free() {
        std::free(data);
        data = nullptr;
    }
};

//=============================================================================
// DEFLATE Decompression
//=============================================================================

/// @brief Decompress a DEFLATE stored (BTYPE=00) block.
/// Aligns to the next byte boundary, validates LEN+NLEN one's-complement, then
/// copies LEN raw bytes from the bit stream to `out`.
/// @param br Input bit reader positioned after the block header.
/// @param out Bounded output buffer to extend.
/// @return `true` when lengths and payload are complete and valid.
static bool inflateStored(BitReader &br, OutputBuffer &out) {
    br.align();
    if (br.pos + 4 > br.len)
        return false;

    uint16_t blockLen = br.data[br.pos] | (br.data[br.pos + 1] << 8);
    uint16_t nlen = br.data[br.pos + 2] | (br.data[br.pos + 3] << 8);
    br.pos += 4;

    if ((blockLen ^ nlen) != 0xFFFF)
        return false;
    if (br.pos + blockLen > br.len)
        return false;

    out.ensure(blockLen);
    std::memcpy(out.data + out.len, br.data + br.pos, blockLen);
    out.len += blockLen;
    br.pos += blockLen;
    return true;
}

/// @brief Decompress a DEFLATE Huffman-coded block (BTYPE=01 or BTYPE=02).
/// Symbol 256 is end-of-block; symbols 257-285 are length codes followed by a distance code.
/// @param br Input bit reader positioned at the first symbol.
/// @param out Bounded output buffer to extend.
/// @param litTree Literal/length decode table.
/// @param distTree Distance decode table.
/// @return `true` when a valid end-of-block symbol is reached.
static bool inflateHuffman(BitReader &br,
                           OutputBuffer &out,
                           const HuffmanTree &litTree,
                           const HuffmanTree &distTree) {
    while (true) {
        int sym = litTree.decode(br);
        if (sym < 0)
            return false;

        if (sym < 256) {
            out.putByte(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else if (sym <= 285) {
            int lenIdx = sym - 257;
            int length = kLengthBase[lenIdx];
            int extra = kLengthExtraBits[lenIdx];
            if (extra > 0)
                length += br.read(extra);

            int distSym = distTree.decode(br);
            if (distSym < 0 || distSym >= 30)
                return false;

            int distance = kDistBase[distSym];
            extra = kDistExtraBits[distSym];
            if (extra > 0)
                distance += br.read(extra);

            if (distance > static_cast<int>(out.len))
                return false;

            out.copyBack(distance, length);
        } else {
            return false;
        }
    }
}

/// @brief Decompress a DEFLATE dynamic Huffman block (BTYPE=10).
/// Reads HLIT/HDIST/HCLEN, builds the code-length tree, decodes literal/length and distance
/// tables (with repeat codes 16/17/18 per RFC 1951 §3.2.7), then calls `inflateHuffman`.
/// @param br Input bit reader positioned after the block header.
/// @param out Bounded output buffer to extend.
/// @return `true` when the dynamic tables and encoded payload are valid.
static bool inflateDynamic(BitReader &br, OutputBuffer &out) {
    int hlit = br.read(5) + 257;
    int hdist = br.read(5) + 1;
    int hclen = br.read(4) + 4;

    if (hlit > kMaxLitCodes || hdist > kMaxDistCodes)
        return false;

    uint8_t clLengths[kMaxCodeLenCodes] = {};
    for (int i = 0; i < hclen; i++)
        clLengths[kCodeLengthOrder[i]] = static_cast<uint8_t>(br.read(3));

    HuffmanTree clTree;
    if (!clTree.build(clLengths, kMaxCodeLenCodes))
        return false;

    uint8_t lengths[kMaxLitCodes + kMaxDistCodes] = {};
    int totalCodes = hlit + hdist;
    int i = 0;

    while (i < totalCodes) {
        int sym = clTree.decode(br);
        if (sym < 0)
            return false;

        if (sym < 16) {
            lengths[i++] = static_cast<uint8_t>(sym);
        } else if (sym == 16) {
            if (i == 0)
                return false;
            int repeat = br.read(2) + 3;
            if (i + repeat > totalCodes)
                return false;
            uint8_t prev = lengths[i - 1];
            while (repeat-- > 0 && i < totalCodes)
                lengths[i++] = prev;
        } else if (sym == 17) {
            int repeat = br.read(3) + 3;
            if (i + repeat > totalCodes)
                return false;
            while (repeat-- > 0 && i < totalCodes)
                lengths[i++] = 0;
        } else if (sym == 18) {
            int repeat = br.read(7) + 11;
            if (i + repeat > totalCodes)
                return false;
            while (repeat-- > 0 && i < totalCodes)
                lengths[i++] = 0;
        } else {
            return false;
        }
    }

    if (lengths[256] == 0)
        return false;

    bool hasLengthCode = false;
    for (int code = 257; code <= 285 && code < hlit; ++code) {
        if (lengths[code] != 0) {
            hasLengthCode = true;
            break;
        }
    }
    bool hasDistanceCode = false;
    for (int code = 0; code < hdist; ++code) {
        if (lengths[hlit + code] != 0) {
            hasDistanceCode = true;
            break;
        }
    }
    if (hasLengthCode && !hasDistanceCode)
        return false;

    HuffmanTree litTree, distTree;
    if (!litTree.build(lengths, hlit))
        return false;
    if (!hasDistanceCode)
        return inflateHuffman(br, out, litTree, *sFixedDistTree);
    if (!distTree.build(lengths + hlit, hdist))
        return false;

    return inflateHuffman(br, out, litTree, distTree);
}

/// @brief Top-level DEFLATE decompression loop: reads BFINAL/BTYPE block headers and
/// dispatches to `inflateStored`, `inflateHuffman` (fixed), or `inflateDynamic`.
/// Throws `DeflateError` on malformed input or output size violation.
/// @param data Borrowed raw DEFLATE bytes.
/// @param len Compressed input length.
/// @param maxOutputBytes Maximum allowed decompressed length.
/// @return Decompressed bytes.
/// @throws DeflateError If input is malformed/truncated, has trailing data, exceeds
///         the output limit, or allocation fails.
static std::vector<uint8_t> inflateData(const uint8_t *data, size_t len, size_t maxOutputBytes) {
    initFixedTrees();

    BitReader br;
    br.init(data, len);

    OutputBuffer out;
    const size_t initialCap =
        len > std::numeric_limits<size_t>::max() / 4 ? maxOutputBytes : len * 4;
    out.init(initialCap, maxOutputBytes);

    bool lastBlock = false;
    while (!lastBlock) {
        if (!br.hasData()) {
            out.free();
            throw DeflateError("inflate: unexpected end of data");
        }

        lastBlock = br.read(1);
        int blockType = br.read(2);

        bool ok = false;
        switch (blockType) {
            case 0:
                ok = inflateStored(br, out);
                break;
            case 1:
                ok = inflateHuffman(br, out, *sFixedLitTree, *sFixedDistTree);
                break;
            case 2:
                ok = inflateDynamic(br, out);
                break;
            default:
                out.free();
                throw DeflateError("inflate: invalid block type");
        }

        if (!ok) {
            out.free();
            throw DeflateError("inflate: invalid compressed data");
        }
    }

    if ((br.bitsInBuf > 0 && br.buffer != 0) || br.pos != br.len) {
        out.free();
        throw DeflateError("inflate: trailing data after final block");
    }

    return out.toVector();
}

//=============================================================================
// DEFLATE Compression
//=============================================================================

/// @brief LZ77 match accelerator: a 3-byte rolling hash plus sliding-window chains.
struct LZ77State {
    int *head = nullptr;
    int *prev = nullptr;

    /// @brief Construct an empty match accelerator with no chain storage.
    LZ77State() = default;

    /// @brief Release the hash-head and predecessor arrays.
    ~LZ77State() {
        std::free(head);
        std::free(prev);
    }

    /// @brief Disable copying because the state uniquely owns both arrays.
    LZ77State(const LZ77State &) = delete;
    /// @brief Disable copy assignment because the state uniquely owns both arrays.
    /// @return This state; the operation is unavailable.
    LZ77State &operator=(const LZ77State &) = delete;

    static constexpr int kHashBits = 15;
    static constexpr int kHashSize = 1 << kHashBits;
    static constexpr int kHashMask = kHashSize - 1;
    static constexpr int kNil = -1;

    /// @brief Allocate `head[]` and `prev[]` arrays and initialize all entries to `kNil`.
    /// `head` maps a 3-byte rolling hash to the most-recent position; `prev` is the sliding-window
    /// chain.
    void init() {
        head = static_cast<int *>(std::malloc(kHashSize * sizeof(int)));
        prev = static_cast<int *>(std::malloc(kWindowSize * sizeof(int)));
        if (!head || !prev) {
            std::free(head);
            std::free(prev);
            head = nullptr;
            prev = nullptr;
            throw DeflateError("deflate: memory allocation failed");
        }
        for (int i = 0; i < kHashSize; i++)
            head[i] = kNil;
        for (int i = 0; i < kWindowSize; i++)
            prev[i] = kNil;
    }

    /// @brief Release `head` and `prev` arrays. Called on error paths; the destructor also frees
    /// them.
    void free() {
        std::free(head);
        std::free(prev);
    }

    /// @brief Compute a 3-byte rolling hash for `d[0..2]` — collisions are harmless
    /// (just longer chains), but poor distribution reduces compression ratio.
    /// @param d Address of at least three input bytes.
    /// @return Hash bucket index.
    static int computeHash(const uint8_t *d) {
        return ((d[0] << 10) ^ (d[1] << 5) ^ d[2]) & kHashMask;
    }

    /// @brief Walk the hash chain for `data+pos` and return the longest match in the sliding
    /// window. Sets `*matchDist` to the distance of the best match. Returns 0 if no match meets
    /// `kMinMatchLen`. `maxChain` limits chain traversal depth to bound CPU cost.
    /// @param data Complete uncompressed input.
    /// @param pos Current input position.
    /// @param len Total input length.
    /// @param maxChain Maximum candidate positions to inspect.
    /// @param matchDist Output destination for the winning backward distance.
    /// @return Match length from 3 through 258, or zero.
    int findMatch(const uint8_t *data, size_t pos, size_t len, int maxChain, int *matchDist) const {
        if (pos + kMinMatchLen > len)
            return 0;

        int hash = computeHash(data + pos);
        int chainPos = head[hash];
        int bestLen = kMinMatchLen - 1;
        int bestDist = 0;

        int limit = static_cast<int>(pos) - kMaxDistance;
        if (limit < 0)
            limit = 0;

        while (chainPos >= limit && maxChain-- > 0) {
            int matchLen = 0;
            size_t a = pos, b = chainPos;
            size_t maxLen = len - pos;
            if (maxLen > kMaxMatchLen)
                maxLen = kMaxMatchLen;

            while (matchLen < static_cast<int>(maxLen) && data[a] == data[b]) {
                matchLen++;
                a++;
                b++;
            }

            if (matchLen > bestLen) {
                bestLen = matchLen;
                bestDist = static_cast<int>(pos - chainPos);
                if (bestLen >= kMaxMatchLen)
                    break;
            }

            chainPos = prev[chainPos & kWindowMask];
        }

        *matchDist = bestDist;
        return bestLen >= kMinMatchLen ? bestLen : 0;
    }

    /// @brief Insert `pos` into the hash chain for `data[pos..pos+3)`.
    /// Must be called for every byte position the compressor advances past.
    /// @param data Complete uncompressed input.
    /// @param pos Position with at least three accessible bytes.
    void updateHash(const uint8_t *data, size_t pos) {
        int hash = computeHash(data + pos);
        prev[pos & kWindowMask] = head[hash];
        head[hash] = static_cast<int>(pos);
    }
};

/// @brief Map a match length (3–258) to its DEFLATE length code (257–285).
/// Extra bits from `kLengthExtraBits` encode the residual beyond the base value.
/// @param length Match length in the DEFLATE-supported range.
/// @return Literal/length alphabet code.
static int getLengthCode(int length) {
    for (int i = 0; i < 29; i++) {
        if (i == 28)
            return 285;
        if (length < kLengthBase[i + 1])
            return 257 + i;
    }
    return 285;
}

/// @brief Map a match distance (1–32768) to its DEFLATE distance code (0–29).
/// Extra bits from `kDistExtraBits` encode the residual beyond `kDistBase[code]`.
/// @param dist Backward match distance.
/// @return Distance alphabet code.
static int getDistCode(int dist) {
    for (int i = 0; i < 30; i++) {
        if (i == 29 || dist < kDistBase[i + 1])
            return i;
    }
    return 29;
}

/// @brief Write bits in reverse order (LSB first as required by DEFLATE)
/// @param bw Bit writer receiving the code.
/// @param code Canonical MSB-first Huffman code.
/// @param len Code length in bits.
static void writeCode(BitWriter &bw, uint16_t code, int len) {
    uint16_t rev = 0;
    for (int i = 0; i < len; i++) {
        if (code & (1 << i))
            rev |= 1 << (len - 1 - i);
    }
    bw.write(rev, len);
}

/// @brief Compress using stored blocks (no compression)
/// @param bw Bit writer receiving one or more stored blocks.
/// @param data Borrowed uncompressed input.
/// @param len Input length.
static void deflateStored(BitWriter &bw, const uint8_t *data, size_t len) {
    if (len == 0) {
        bw.write(1, 1); // BFINAL = 1
        bw.write(0, 2); // BTYPE = stored
        bw.flush();
        bw.write(0x00, 8);
        bw.write(0x00, 8);
        bw.write(0xFF, 8);
        bw.write(0xFF, 8);
        return;
    }

    size_t pos = 0;
    while (pos < len) {
        size_t blockLen = std::min<size_t>(65535, len - pos);

        bool last = (pos + blockLen >= len);
        bw.write(last ? 1 : 0, 1); // BFINAL
        bw.write(0, 2);            // BTYPE = stored
        bw.flush();

        uint16_t nlen = ~static_cast<uint16_t>(blockLen);
        bw.write(blockLen & 0xFF, 8);
        bw.write((blockLen >> 8) & 0xFF, 8);
        bw.write(nlen & 0xFF, 8);
        bw.write((nlen >> 8) & 0xFF, 8);

        bw.flush();
        bw.writeBytes(data + pos, blockLen);
        pos += blockLen;
    }
}

/// @brief Compress using fixed Huffman codes with LZ77
/// @param bw Bit writer receiving the final fixed-Huffman block.
/// @param data Borrowed uncompressed input.
/// @param len Input length.
/// @param level Compression level controlling hash-chain search depth.
/// @throws DeflateError If LZ77 or output allocation fails.
static void deflateFixed(BitWriter &bw, const uint8_t *data, size_t len, int level) {
    initFixedTrees();

    LZ77State lz;
    lz.init();

    int maxChain = 4 << level;

    bw.write(1, 1); // BFINAL = 1
    bw.write(1, 2); // BTYPE = fixed Huffman

    size_t pos = 0;
    while (pos < len) {
        int matchDist = 0;
        int matchLen = 0;

        if (pos + kMinMatchLen <= len)
            matchLen = lz.findMatch(data, pos, len, maxChain, &matchDist);

        if (matchLen >= kMinMatchLen) {
            int lenCode = getLengthCode(matchLen);
            int lenIdx = lenCode - 257;

            if (lenCode <= 279)
                writeCode(bw, static_cast<uint16_t>(lenCode - 256), 7);
            else
                writeCode(bw, static_cast<uint16_t>(0xC0 + (lenCode - 280)), 8);

            if (kLengthExtraBits[lenIdx] > 0)
                bw.write(matchLen - kLengthBase[lenIdx], kLengthExtraBits[lenIdx]);

            int distCode = getDistCode(matchDist);
            writeCode(bw, static_cast<uint16_t>(distCode), 5);

            if (kDistExtraBits[distCode] > 0)
                bw.write(matchDist - kDistBase[distCode], kDistExtraBits[distCode]);

            for (int i = 0; i < matchLen; i++) {
                if (pos + i + kMinMatchLen <= len)
                    lz.updateHash(data, pos + i);
            }
            pos += matchLen;
        } else {
            uint8_t lit = data[pos];
            if (lit <= 143)
                writeCode(bw, 0x30 + lit, 8);
            else
                writeCode(bw, 0x190 + (lit - 144), 9);

            if (pos + kMinMatchLen <= len)
                lz.updateHash(data, pos);
            pos++;
        }
    }

    // End of block (code 256, 7 bits)
    writeCode(bw, 0, 7);

    // LZ77State destructor handles cleanup — do NOT call lz.free() here
    // (that causes a double-free since the destructor also frees head/prev).
}

/// @brief Top-level compression dispatcher. Clamps level to [1, 9], then selects:
/// stored blocks for tiny/empty inputs or level 1; fixed Huffman+LZ77 for all other cases.
/// @param data Borrowed uncompressed input.
/// @param len Input length.
/// @param level Requested compression level.
/// @return Raw DEFLATE stream bytes.
/// @throws DeflateError If size arithmetic or allocation fails.
static std::vector<uint8_t> deflateData(const uint8_t *data, size_t len, int level) {
    if (level < kMinLevel)
        level = kMinLevel;
    if (level > kMaxLevel)
        level = kMaxLevel;

    BitWriter bw;
    constexpr size_t kMaxInitialReserve = 1024u * 1024u;
    const size_t initialReserve = len <= 64 ? 256u : std::min<size_t>(len, kMaxInitialReserve);
    bw.init(initialReserve);

    if (len > static_cast<size_t>(std::numeric_limits<int>::max())) {
        // The LZ77 accelerator stores window positions in int slots. Very large
        // inputs remain packageable by falling back to standards-compliant
        // stored blocks instead of truncating match positions.
        deflateStored(bw, data, len);
    } else if (len <= 64 || level == 1)
        deflateStored(bw, data, len);
    else
        deflateFixed(bw, data, len, level);

    bw.flush();
    return bw.toVector();
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Compress `data` using raw DEFLATE at the given level.
/// @param data Borrowed uncompressed input.
/// @param len Input length.
/// @param level Compression level, clamped to the supported 1–9 range.
/// @return Raw DEFLATE stream bytes.
/// @throws DeflateError If compression fails.
std::vector<uint8_t> deflate(const uint8_t *data, size_t len, int level) {
    return deflateData(data, len, level);
}

/// @brief Decompress a raw DEFLATE stream with the default 256 MB output limit.
/// @param data Borrowed compressed input.
/// @param len Compressed input length.
/// @return Decompressed bytes.
/// @throws DeflateError If input is invalid/truncated or the output exceeds 256 MiB.
std::vector<uint8_t> inflate(const uint8_t *data, size_t len) {
    return inflateData(data, len, kInflateMaxOutput);
}

/// @brief Decompress a raw DEFLATE stream with an explicit output byte limit.
/// @param data Borrowed compressed input.
/// @param len Compressed input length.
/// @param maxOutputBytes Maximum permitted decompressed length.
/// @return Decompressed bytes.
/// @throws DeflateError If input is invalid/truncated or the limit is exceeded.
std::vector<uint8_t> inflate(const uint8_t *data, size_t len, size_t maxOutputBytes) {
    return inflateData(data, len, maxOutputBytes);
}

} // namespace zanna::pkg
