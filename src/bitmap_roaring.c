/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Roaring encoding for strings used as bitmaps.
 *
 * A string holding a bitmap (built with SETBIT, BITFIELD or BITOP) can be
 * stored as a compressed Roaring bitmap (https://roaringbitmap.org) instead of
 * an array of bytes. Bit i of the string, that is bit 7 - (i % 8) of byte
 * i / 8, is the value i of the Roaring bitmap. We also keep the length of the
 * string in bytes: it is part of the value and can extend past the last set
 * bit.
 *
 * The encoding is transparent. Bitmap commands, SETRANGE and APPEND work on
 * the Roaring bitmap directly, and other commands get the plain bytes (see
 * getDecodedObject()), so every command returns exactly what it returns with
 * the plain encoding.
 *
 * Roaring bitmaps split the 32-bit values into chunks of 2^16, each stored in
 * a container: a sorted array of up to 4096 16-bit values, a bitset of 2^16
 * bits, or a list of runs. To build a Roaring bitmap from bytes, we write the
 * documented portable serialization format of CRoaring
 * (https://github.com/RoaringBitmap/RoaringFormatSpec): a bitset container
 * holds the same bits as 8 KB of the string, with the bits of each byte in
 * reverse order. To get bytes back, we read the bitmap as 64-bit words with
 * roaring_bitmap_word_range_to_bitset(). */

#include "server.h"
#include "bitmap_roaring.h"

/* The vendored header does not build cleanly with our warning flags. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wstatic-in-inline"
#endif
#include "roaring.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

typedef struct bitmapRoaring {
    roaring_bitmap_t *rb;
    size_t len; /* Length of the string in bytes. */
} bitmapRoaring;

#define CHUNK_BYTES 8192 /* Bytes of the string covered by a container. */
#define ARRAY_MAX_CARD 4096
#define COOKIE_NO_RUN 12346
#define MAX_WORDS ((uint64_t)1 << 26) /* 64-bit words covering the 2^32 values. */
#define WINDOW_WORDS 512              /* Words read at once. */
#define SMALL_WRITE_BYTES 64          /* Writes of up to this many bytes set bits one by one. */
#define SEGMENT_CHUNKS 128            /* Chunks rebuilt at once by larger writes. */

static inline bitmapRoaring *getBitmap(const robj *o) {
    serverAssert(o->type == OBJ_STRING && o->encoding == OBJ_ENCODING_ROARING);
    return objectGetVal(o);
}

/* ----------------------------- Memory ------------------------------------ */

static void *roaringMalloc(size_t size) {
    return zmalloc(size);
}

static void *roaringRealloc(void *ptr, size_t size) {
    return zrealloc(ptr, size);
}

static void *roaringCalloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    return zcalloc(count * size);
}

static void roaringFree(void *ptr) {
    zfree(ptr);
}

static void *roaringAlignedMalloc(size_t alignment, size_t size) {
    serverAssert(alignment <= CACHE_LINE_SIZE);
    return zmalloc_cache_aligned(size);
}

/* Route the allocations of CRoaring through zmalloc so they count as used
 * memory. */
void bitmapRoaringInit(void) {
    roaring_memory_t hooks = {
        .malloc = roaringMalloc,
        .realloc = roaringRealloc,
        .calloc = roaringCalloc,
        .free = roaringFree,
        .aligned_malloc = roaringAlignedMalloc,
        .aligned_free = roaringFree,
    };
    roaring_init_memory_hook(hooks);
}

/* ----------------------------- Helpers ----------------------------------- */

static inline unsigned char reverseBits(unsigned char b) {
    b = (unsigned char)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (unsigned char)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = (unsigned char)((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

static inline void writeLE16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static inline void writeLE32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* Reverses the order of the bits in each byte of a word. */
static inline uint64_t reverseBitsInBytes(uint64_t w) {
    w = (w >> 1 & 0x5555555555555555ULL) | (w & 0x5555555555555555ULL) << 1;
    w = (w >> 2 & 0x3333333333333333ULL) | (w & 0x3333333333333333ULL) << 2;
    w = (w >> 4 & 0x0F0F0F0F0F0F0F0FULL) | (w & 0x0F0F0F0F0F0F0F0FULL) << 4;
    return w;
}

static robj *createObjectFromRoaring(roaring_bitmap_t *rb, size_t len) {
    bitmapRoaring *bm = zmalloc(sizeof(*bm));
    bm->rb = rb;
    bm->len = len;
    robj *o = createObject(OBJ_STRING, bm);
    objectSetEncoding(o, OBJ_ENCODING_ROARING);
    return o;
}

/* --------------------------- Conversions --------------------------------- */

/* Builds a Roaring bitmap holding the bits of the 'len' bytes at 'p', taken as
 * the bytes of the string from byte first_chunk * CHUNK_BYTES. */
static roaring_bitmap_t *roaringFromBytes(const unsigned char *p, size_t len, size_t first_chunk) {
    serverAssert(first_chunk * CHUNK_BYTES + len <= BITMAP_ROARING_MAX_BYTES);
    size_t nchunks = (len + CHUNK_BYTES - 1) / CHUNK_BYTES;
    uint32_t *cards = zmalloc(sizeof(uint32_t) * (nchunks ? nchunks : 1));
    size_t ncontainers = 0, payload = 0;
    for (size_t c = 0; c < nchunks; c++) {
        size_t bytes = len - c * CHUNK_BYTES < CHUNK_BYTES ? len - c * CHUNK_BYTES : CHUNK_BYTES;
        cards[c] = (uint32_t)serverPopcount((void *)(p + c * CHUNK_BYTES), (long)bytes);
        if (cards[c] == 0) continue;
        ncontainers++;
        payload += cards[c] <= ARRAY_MAX_CARD ? 2 * (size_t)cards[c] : CHUNK_BYTES;
    }
    if (ncontainers == 0) {
        zfree(cards);
        return roaring_bitmap_create();
    }

    /* Cookie, number of containers, then for each container its key and
     * cardinality minus one, then its offset, then the containers. */
    size_t header = 8 + 8 * ncontainers;
    size_t total = header + payload;
    unsigned char *buf = zmalloc(total);
    writeLE32(buf, COOKIE_NO_RUN);
    writeLE32(buf + 4, (uint32_t)ncontainers);
    unsigned char *keys = buf + 8, *offsets = buf + 8 + 4 * ncontainers, *out = buf + header;
    for (size_t c = 0; c < nchunks; c++) {
        uint32_t card = cards[c];
        if (card == 0) continue;
        writeLE16(keys, (uint16_t)(first_chunk + c));
        writeLE16(keys + 2, (uint16_t)(card - 1));
        keys += 4;
        writeLE32(offsets, (uint32_t)(out - buf));
        offsets += 4;
        const unsigned char *chunk = p + c * CHUNK_BYTES;
        size_t bytes = len - c * CHUNK_BYTES < CHUNK_BYTES ? len - c * CHUNK_BYTES : CHUNK_BYTES;
        if (card <= ARRAY_MAX_CARD) {
            for (size_t b = 0; b < bytes; b++) {
                unsigned int v = reverseBits(chunk[b]);
                while (v) {
                    writeLE16(out, (uint16_t)(b * 8 + __builtin_ctz(v)));
                    out += 2;
                    v &= v - 1;
                }
            }
        } else {
            for (size_t b = 0; b < bytes; b++) out[b] = reverseBits(chunk[b]);
            memset(out + bytes, 0, CHUNK_BYTES - bytes);
            out += CHUNK_BYTES;
        }
    }
    serverAssert((size_t)(out - buf) == total);
    zfree(cards);

    roaring_bitmap_t *rb = roaring_bitmap_portable_deserialize_safe((const char *)buf, total);
    zfree(buf);
    serverAssert(rb != NULL);
    roaring_bitmap_run_optimize(rb);
    roaring_bitmap_shrink_to_fit(rb);
    return rb;
}

/* Writes into dst[0..count) the bytes start..start+count-1 of the string
 * held by 'rb'. Bytes past the 2^32 bits of the encoding are zero. */
static void roaringToBytes(const roaring_bitmap_t *rb, size_t start, size_t count, unsigned char *dst) {
    uint64_t words[WINDOW_WORDS];
    size_t done = 0;
    while (done < count) {
        size_t byte = start + done, want = count - done;
        uint64_t first_word = byte / 8;
        if (first_word >= MAX_WORDS) {
            memset(dst + done, 0, want);
            return;
        }
        size_t skip = byte % 8; /* Bytes of the first word before 'byte'. */
        uint64_t n = (skip + want + 7) / 8;
        if (n > WINDOW_WORDS) n = WINDOW_WORDS;
        if (first_word + n > MAX_WORDS) n = MAX_WORDS - first_word;
        roaring_bitmap_word_range_to_bitset(rb, (uint32_t)first_word, (uint32_t)n, words);
        size_t take = n * 8 - skip < want ? n * 8 - skip : want;
        /* Byte b of the words holds the values 8 * b to 8 * b + 7, the first
         * one in its least significant bit. */
        for (uint64_t k = 0; k < n; k++)
            if (words[k]) words[k] = reverseBitsInBytes(words[k]);
#if BYTE_ORDER == LITTLE_ENDIAN
        if (take >= 64) {
            memcpy(dst + done, (unsigned char *)words + skip, take);
        } else
#endif
        {
            /* A loop is faster than memcpy() for the few bytes of BITFIELD. */
            for (size_t b = 0; b < take; b++)
                dst[done + b] = (unsigned char)(words[(skip + b) / 8] >> (8 * ((skip + b) % 8)));
        }
        done += take;
    }
}

/* ----------------------------- Objects ----------------------------------- */

/* Creates a string of 'len' zero bytes with the Roaring encoding. */
robj *createBitmapRoaringObject(size_t len) {
    serverAssert(len <= BITMAP_ROARING_MAX_BYTES);
    return createObjectFromRoaring(roaring_bitmap_create(), len);
}

/* Returns a Roaring-encoded copy of the string 'o' (any encoding), or NULL if
 * it is too long for the encoding. */
robj *bitmapRoaringObjectFromString(robj *o) {
    serverAssert(o->type == OBJ_STRING);
    if (o->encoding == OBJ_ENCODING_ROARING) return bitmapRoaringDupObject(o);
    if (stringObjectLen(o) > BITMAP_ROARING_MAX_BYTES) return NULL;
    robj *decoded = getDecodedObject(o);
    sds s = objectGetVal(decoded);
    robj *r = createObjectFromRoaring(roaringFromBytes((unsigned char *)s, sdslen(s), 0), sdslen(s));
    decrRefCount(decoded);
    return r;
}

robj *bitmapRoaringDupObject(const robj *o) {
    const bitmapRoaring *bm = getBitmap(o);
    return createObjectFromRoaring(roaring_bitmap_copy(bm->rb), bm->len);
}

void bitmapRoaringFreeObject(robj *o) {
    bitmapRoaring *bm = getBitmap(o);
    roaring_bitmap_free(bm->rb);
    zfree(bm);
}

/* Returns the bytes of the string. */
sds bitmapRoaringToSds(const robj *o) {
    const bitmapRoaring *bm = getBitmap(o);
    sds s = sdsnewlen(SDS_NOINIT, bm->len);
    roaringToBytes(bm->rb, 0, bm->len, (unsigned char *)s);
    return s;
}

/* Returns 'count' bytes of the string from byte 'start', which must be in
 * the string. */
sds bitmapRoaringGetRange(const robj *o, size_t start, size_t count) {
    const bitmapRoaring *bm = getBitmap(o);
    serverAssert(start + count <= bm->len);
    sds s = sdsnewlen(SDS_NOINIT, count);
    roaringToBytes(bm->rb, start, count, (unsigned char *)s);
    return s;
}

size_t bitmapRoaringLen(const robj *o) {
    return getBitmap(o)->len;
}

/* Approximate memory used by the bitmap. */
size_t bitmapRoaringAllocSize(const robj *o) {
    const bitmapRoaring *bm = getBitmap(o);
    roaring_statistics_t stats;
    roaring_bitmap_statistics(bm->rb, &stats);
    const roaring_array_t *ra = &bm->rb->high_low_container;
    return sizeof(*bm) + sizeof(*bm->rb) +
           (size_t)ra->allocation_size * (sizeof(void *) + sizeof(uint16_t) + sizeof(uint8_t)) +
           (size_t)stats.n_bytes_array_containers + stats.n_bytes_run_containers + stats.n_bytes_bitset_containers;
}

/* Number of allocations to release, for lazy freeing. */
size_t bitmapRoaringFreeEffort(const robj *o) {
    return (size_t)getBitmap(o)->rb->high_low_container.size + 1;
}

/* ---------------------------- Bit access --------------------------------- */

/* Makes the string at least 'len' bytes long, padding with zero bytes. */
void bitmapRoaringGrow(robj *o, size_t len) {
    bitmapRoaring *bm = getBitmap(o);
    serverAssert(len <= BITMAP_ROARING_MAX_BYTES);
    if (len > bm->len) bm->len = len;
}

int bitmapRoaringGetBit(const robj *o, uint64_t offset) {
    const bitmapRoaring *bm = getBitmap(o);
    if (offset > UINT32_MAX) return 0;
    return roaring_bitmap_contains(bm->rb, (uint32_t)offset);
}

/* Sets or clears a bit within the string. Returns its previous value. */
int bitmapRoaringSetBit(robj *o, uint64_t offset, int on) {
    bitmapRoaring *bm = getBitmap(o);
    serverAssert(offset >> 3 < bm->len);
    if (on) return !roaring_bitmap_add_checked(bm->rb, (uint32_t)offset);
    return roaring_bitmap_remove_checked(bm->rb, (uint32_t)offset);
}

/* Number of set bits from offset 'first' to 'last', inclusive. */
uint64_t bitmapRoaringCount(const robj *o, uint64_t first, uint64_t last) {
    const bitmapRoaring *bm = getBitmap(o);
    if (first > last || first > UINT32_MAX) return 0;
    if (first == 0 && bm->len && last >= (uint64_t)bm->len * 8 - 1) return roaring_bitmap_get_cardinality(bm->rb);
    if (last > UINT32_MAX) last = UINT32_MAX;
    return roaring_bitmap_range_cardinality(bm->rb, first, last + 1);
}

/* Offset of the first bit set to 'bit' from 'first' to 'last' (inclusive), or
 * -1 if there is none. */
long long bitmapRoaringFirst(const robj *o, int bit, uint64_t first, uint64_t last) {
    const bitmapRoaring *bm = getBitmap(o);
    if (first > last) return -1;
    if (bit) {
        if (first > UINT32_MAX) return -1;
        roaring_uint32_iterator_t it;
        roaring_iterator_init(bm->rb, &it);
        if (!roaring_uint32_iterator_move_equalorlarger(&it, (uint32_t)first)) return -1;
        return it.current_value <= last ? (long long)it.current_value : -1;
    }
    if (first > UINT32_MAX || !roaring_bitmap_contains(bm->rb, (uint32_t)first)) return (long long)first;
    /* Most runs of set bits are short: we read the words that follow
     * 'first', then for a long run we grow a window known to be set, and
     * finally bisect. */
    uint64_t end = (last > UINT32_MAX ? UINT32_MAX : last) + 1; /* Search up to 'end' (excluded). */
    uint64_t next = first;                                      /* The bits before 'next' are set. */
    uint64_t words[16];
    for (uint64_t n = 1; n <= 16 && next < end; n *= 16) {
        uint64_t w = next / 64;
        if (w + n > MAX_WORDS) n = MAX_WORDS - w;
        roaring_bitmap_word_range_to_bitset(bm->rb, (uint32_t)w, (uint32_t)n, words);
        words[0] |= ((uint64_t)1 << (next % 64)) - 1; /* The bits before 'next'. */
        for (uint64_t k = 0; k < n; k++) {
            if (~words[k]) {
                uint64_t pos = (w + k) * 64 + (uint64_t)__builtin_ctzll(~words[k]);
                return pos <= last ? (long long)pos : -1;
            }
        }
        next = (w + n) * 64;
    }
    uint64_t step = 1024;
    while (next < end) {
        uint64_t stop = next + step < end ? next + step : end;
        if (!roaring_bitmap_contains_range(bm->rb, next, stop)) {
            /* A clear bit is in [next, stop). */
            while (stop - next > 1) {
                uint64_t mid = next + (stop - next) / 2;
                if (roaring_bitmap_contains_range(bm->rb, next, mid))
                    next = mid;
                else
                    stop = mid;
            }
            return (long long)next;
        }
        next = stop;
        step *= 2;
    }
    return next <= last ? (long long)next : -1;
}

/* Reads 'count' bytes of the string from byte 'start'. Bytes past the end of
 * the string read as zero. */
void bitmapRoaringReadBytes(const robj *o, size_t start, size_t count, unsigned char *dst) {
    roaringToBytes(getBitmap(o)->rb, start, count, dst);
}

/* Overwrites 'count' bytes of the string from byte 'start', which must be
 * within the string. */
void bitmapRoaringWriteBytes(robj *o, size_t start, size_t count, const unsigned char *src) {
    bitmapRoaring *bm = getBitmap(o);
    serverAssert(start + count <= bm->len);
    if (count == 0) return;

    /* A few bytes, as written by BITFIELD: we clear their bits and add the
     * new ones. */
    if (count <= SMALL_WRITE_BYTES) {
        uint32_t values[SMALL_WRITE_BYTES * 8];
        size_t n = 0;
        uint64_t first = (uint64_t)start * 8;
        roaring_bitmap_remove_range(bm->rb, first, first + (uint64_t)count * 8);
        for (size_t i = 0; i < count; i++) {
            unsigned int v = reverseBits(src[i]);
            while (v) {
                values[n++] = (uint32_t)(first + i * 8 + __builtin_ctz(v));
                v &= v - 1;
            }
        }
        roaring_bitmap_add_many(bm->rb, n, values);
        return;
    }

    /* Otherwise we rebuild the chunks covered by the bytes, a segment of up to
     * SEGMENT_CHUNKS chunks at a time. The bytes of the first and last chunks
     * that are not overwritten come from the bitmap. */
    size_t end = start + count;
    size_t chunk = start / CHUNK_BYTES, last_chunk = (end - 1) / CHUNK_BYTES;
    unsigned char *buf = NULL;
    while (chunk <= last_chunk) {
        size_t nchunks = last_chunk - chunk + 1 < SEGMENT_CHUNKS ? last_chunk - chunk + 1 : SEGMENT_CHUNKS;
        size_t lo = chunk * CHUNK_BYTES, hi = (chunk + nchunks) * CHUNK_BYTES;
        const unsigned char *bytes;
        if (lo >= start && hi <= end) {
            bytes = src + (lo - start);
        } else {
            if (buf == NULL) buf = zmalloc(SEGMENT_CHUNKS * CHUNK_BYTES);
            size_t from = lo > start ? lo : start, to = hi < end ? hi : end;
            if (from > lo) roaringToBytes(bm->rb, lo, from - lo, buf);
            memcpy(buf + (from - lo), src + (from - start), to - from);
            if (hi > to) roaringToBytes(bm->rb, to, hi - to, buf + (to - lo));
            bytes = buf;
        }
        roaring_bitmap_t *part = roaringFromBytes(bytes, hi - lo, chunk);
        roaring_bitmap_remove_range(bm->rb, (uint64_t)lo * 8, (uint64_t)hi * 8);
        roaring_bitmap_or_inplace(bm->rb, part);
        roaring_bitmap_free(part);
        chunk += nchunks;
    }
    zfree(buf);
}

/* ------------------------------- BITOP ----------------------------------- */

static roaring_bitmap_t *orOrXor(int op, const roaring_bitmap_t *a, const roaring_bitmap_t *b) {
    return op == BITOP_OR ? roaring_bitmap_or(a, b) : roaring_bitmap_xor(a, b);
}

/* OR or XOR of 'n' bitmaps. We do not use roaring_bitmap_or_many(): it turns
 * sparse containers into bitsets until the end, which is slow with many
 * containers. When the result likely keeps sparse (array) containers, we
 * combine pairs into new bitmaps, like a tree, which keeps the arrays small.
 * Otherwise, we combine the inputs one after the other in place. */
static roaring_bitmap_t *unionOrXor(int op, const roaring_bitmap_t **inputs, unsigned long n) {
    if (n == 1) return roaring_bitmap_copy(inputs[0]);
    uint64_t total = 0;
    int32_t containers = 1;
    for (unsigned long j = 0; j < n; j++) {
        total += roaring_bitmap_get_cardinality(inputs[j]);
        if (inputs[j]->high_low_container.size > containers) containers = inputs[j]->high_low_container.size;
    }
    if (n == 2 || total / (uint64_t)containers > ARRAY_MAX_CARD) {
        roaring_bitmap_t *res = orOrXor(op, inputs[0], inputs[1]);
        for (unsigned long j = 2; j < n; j++) {
            if (op == BITOP_OR)
                roaring_bitmap_or_inplace(res, inputs[j]);
            else
                roaring_bitmap_xor_inplace(res, inputs[j]);
        }
        return res;
    }
    roaring_bitmap_t **level = zmalloc(sizeof(*level) * ((n + 1) / 2));
    unsigned long count = 0;
    for (unsigned long j = 0; j < n; j += 2)
        level[count++] = j + 1 < n ? orOrXor(op, inputs[j], inputs[j + 1]) : roaring_bitmap_copy(inputs[j]);
    while (count > 1) {
        unsigned long next = 0;
        for (unsigned long j = 0; j < count; j += 2) {
            if (j + 1 < count) {
                roaring_bitmap_t *t = orOrXor(op, level[j], level[j + 1]);
                roaring_bitmap_free(level[j]);
                roaring_bitmap_free(level[j + 1]);
                level[next++] = t;
            } else {
                level[next++] = level[j];
            }
        }
        count = next;
    }
    roaring_bitmap_t *res = level[0];
    zfree(level);
    return res;
}

robj *bitmapRoaringBitop(int op, robj **objects, unsigned long numkeys, size_t maxlen) {
    serverAssert(maxlen <= BITMAP_ROARING_MAX_BYTES);
    const roaring_bitmap_t **inputs = zmalloc(sizeof(*inputs) * numkeys);
    roaring_bitmap_t **owned = zcalloc(sizeof(*owned) * numkeys);
    size_t *lens = zmalloc(sizeof(*lens) * numkeys);
    int any_empty = 0;
    for (unsigned long j = 0; j < numkeys; j++) {
        robj *o = objects[j];
        if (o == NULL) {
            owned[j] = roaring_bitmap_create();
            inputs[j] = owned[j];
            lens[j] = 0;
        } else if (o->encoding == OBJ_ENCODING_ROARING) {
            inputs[j] = getBitmap(o)->rb;
            lens[j] = getBitmap(o)->len;
        } else {
            robj *decoded = getDecodedObject(o);
            sds s = objectGetVal(decoded);
            owned[j] = roaringFromBytes((unsigned char *)s, sdslen(s), 0);
            inputs[j] = owned[j];
            lens[j] = sdslen(s);
            decrRefCount(decoded);
        }
        if (roaring_bitmap_is_empty(inputs[j])) any_empty = 1;
    }

    roaring_bitmap_t *res;
    switch (op) {
    case BITOP_AND:
        if (any_empty) {
            res = roaring_bitmap_create();
        } else if (numkeys == 1) {
            res = roaring_bitmap_copy(inputs[0]);
        } else {
            res = roaring_bitmap_and(inputs[0], inputs[1]);
            for (unsigned long j = 2; j < numkeys && !roaring_bitmap_is_empty(res); j++)
                roaring_bitmap_and_inplace(res, inputs[j]);
        }
        break;
    case BITOP_OR:
    case BITOP_XOR: res = unionOrXor(op, inputs, numkeys); break;
    case BITOP_NOT: res = roaring_bitmap_flip(inputs[0], 0, (uint64_t)lens[0] * 8); break;
    default: serverPanic("Unknown BITOP operation");
    }

    for (unsigned long j = 0; j < numkeys; j++)
        if (owned[j]) roaring_bitmap_free(owned[j]);
    zfree(owned);
    zfree(inputs);
    zfree(lens);
    return createObjectFromRoaring(res, maxlen);
}
