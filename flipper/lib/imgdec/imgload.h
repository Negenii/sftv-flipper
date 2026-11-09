#pragma once
/* Any picture file -> the 400x240 1-bit SFTV image (PBM bit order, 1 = black), streamed: the
 * source is decoded row by row (JPEG: one MCU strip at a time), shrunk or enlarged to fit
 * 400x240 with its aspect kept (white margins), turned to luma (alpha over white) and Bayer-
 * dithered like the web transmitter. Peak RAM: JPEG ~20 KB, PNG 32 KB window + two rows.
 * Formats: baseline JPEG, PNG (non-interlaced, any bit depth/colour type), BMP (1/4/8/24/32-bit
 * uncompressed), PBM (P4). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMG_W 400
#define IMG_H 240

typedef struct {
    void* ctx;
    int (*read)(void* ctx, uint8_t* buf, int n); /* bytes read, 0 at end, <0 error */
    bool (*seek)(void* ctx, uint32_t offset); /* absolute */
    uint32_t size; /* file size in bytes */
    void (*progress)(void* ctx, int percent); /* may be NULL */
    void* progress_ctx;
} ImgIo;

/* out: IMG_W * IMG_H / 8 bytes. Returns true on success; on failure `err` (if not NULL) gets a
 * short reason ("progressive JPEG", "too wide", ...). */
bool img_load(const ImgIo* io, const char* filename, uint8_t* out, char* err, size_t err_size);

/* A 1-bit raster already in memory (e.g. a Flipper animation frame after compress_icon_decode:
 * rows of `stride` bytes, bit 1 = black, `lsb_first` = leftmost pixel in bit 0 as in XBM). Fitted
 * and dithered like any other source (pure black/white stays crisp). */
bool img_from_bits(const uint8_t* bits, int w, int h, int stride, bool lsb_first, uint8_t* out);
