#pragma once

/* Image geometry: Playdate screen, 1 bit per pixel, PBM P4 packing (bit 1 = black). */
#define SSTV_WIDTH 400
#define SSTV_HEIGHT 240
#define SSTV_LINE_BYTES 50 /* ceil(400 / 8) */
#define SSTV_IMAGE_BYTES (SSTV_LINE_BYTES * SSTV_HEIGHT) /* 12000 */

/* Line frame: line number (8) + pixels (400) + CRC-8 (8). */
#define SSTV_FRAME_BYTES (1 + SSTV_LINE_BYTES + 1) /* 52 */
#define SSTV_FRAME_BITS (SSTV_FRAME_BYTES * 8) /* 416 */

/* Tones, Hz. Square waves. */
#define SSTV_F0_HZ 2000 /* bit 0 */
#define SSTV_F1_HZ 3000 /* bit 1 */
#define SSTV_FSYNC_HZ 4000 /* line sync */

/* Timing, microseconds. */
#define SSTV_BIT_US 1000
#define SSTV_SYNC_US 5000
#define SSTV_LEADER_US 1000000 /* leader tone at SSTV_F1_HZ before line 0 */
