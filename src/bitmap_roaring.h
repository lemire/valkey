/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VALKEY_BITMAP_ROARING_H
#define VALKEY_BITMAP_ROARING_H

#include <stddef.h>
#include <stdint.h>

#include "sds.h"

/* The Roaring encoding of strings used as bitmaps. See bitmap_roaring.c. */

struct serverObject;

/* Roaring bitmaps hold 32-bit values: a string can use the encoding while its
 * bits fit, so up to 2^29 bytes (512 MB). */
#define BITMAP_ROARING_MAX_BYTES ((size_t)1 << 29)

void bitmapRoaringInit(void);

/* Objects. */
struct serverObject *createBitmapRoaringObject(size_t len);
struct serverObject *bitmapRoaringObjectFromString(struct serverObject *o);
struct serverObject *bitmapRoaringDupObject(const struct serverObject *o);
void bitmapRoaringFreeObject(struct serverObject *o);
sds bitmapRoaringToSds(const struct serverObject *o);
sds bitmapRoaringGetRange(const struct serverObject *o, size_t start, size_t count);
size_t bitmapRoaringLen(const struct serverObject *o);
size_t bitmapRoaringAllocSize(const struct serverObject *o);
size_t bitmapRoaringFreeEffort(const struct serverObject *o);

/* Bit access. Offsets must be below BITMAP_ROARING_MAX_BYTES * 8. */
void bitmapRoaringGrow(struct serverObject *o, size_t len);
int bitmapRoaringGetBit(const struct serverObject *o, uint64_t offset);
int bitmapRoaringSetBit(struct serverObject *o, uint64_t offset, int on);
uint64_t bitmapRoaringCount(const struct serverObject *o, uint64_t first, uint64_t last);
long long bitmapRoaringFirst(const struct serverObject *o, int bit, uint64_t first, uint64_t last);
void bitmapRoaringReadBytes(const struct serverObject *o, size_t start, size_t count, unsigned char *dst);
void bitmapRoaringWriteBytes(struct serverObject *o, size_t start, size_t count, const unsigned char *src);

/* The same values as in bitops.c. */
#define BITOP_AND 0
#define BITOP_OR 1
#define BITOP_XOR 2
#define BITOP_NOT 3

/* BITOP over 'numkeys' strings (NULL for missing keys), any encoding. The
 * result has 'maxlen' bytes, the length of the longest input. */
struct serverObject *bitmapRoaringBitop(int op, struct serverObject **objects, unsigned long numkeys, size_t maxlen);

#endif
