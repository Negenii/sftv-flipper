#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Called from the timer ISR after each physical frame finishes (frame index 0..total-1).
 * `done` is true after the last frame; without loop the transmitter then falls silent
 * (call sstv_tx_stop() to release the speaker). Only ISR-safe calls are allowed inside. */
typedef void (*SstvTxCallback)(void* ctx, uint16_t frame, bool done);

/* image_bits: SSTV_IMAGE_BYTES of P4 raster (1 = black). Packed, RS-coded and interleaved
 * into physical frames. Acquires the speaker. Returns false if the speaker is busy. */
bool sstv_tx_start(const uint8_t* image_bits, const char* name, bool loop, SstvTxCallback cb, void* ctx);

/* Temporary tone test (channel measurements): sync burst + 164 symbols cycling through six
 * values (silence, 2500, 3000, 3500, 4000, 4500 Hz) in a fixed pseudo-random order
 * (SSTV_TEST_* below), repeated until sstv_tx_stop(). The callback fires per frame with
 * done = false. */
#define SSTV_TEST_TONES 6
bool sstv_tx_start_test(SstvTxCallback cb, void* ctx);

/* Image lines completed once `frames_sent` physical frames are out (for progress display). */
int sstv_tx_lines_after(int frames_sent);

/* Whether the current transmission uses the coded (compressed) mode. */
bool sstv_tx_is_coded(void);

/* Safe to call at any time, including after the transmitter finished on its own. */
void sstv_tx_stop(void);

bool sstv_tx_is_running(void);

/* Physical frames in the current transmission (depends on how well the image compresses). */
int sstv_tx_total_frames(void);

/* Symbol currently on the speaker: 0..SSTV_NUM_TONES-1 data (0 = silence), -1 sync, -2 idle. */
int sstv_tx_current_symbol(void);

/* Whether the frame on the speaker is an outer-code parity frame (LED colour). */
bool sstv_tx_current_is_parity(void);
