#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Called from the timer ISR after each physical frame finishes (frame index 0..SSTV_NUM_SEGS-1).
 * `done` is true after the last frame; without loop the transmitter then falls silent
 * (call sstv_tx_stop() to release the speaker). Only ISR-safe calls are allowed inside. */
typedef void (*SstvTxCallback)(void* ctx, uint16_t frame, bool done);

/* image_bits: SSTV_IMAGE_BYTES of P4 raster (1 = black). Packed, RS-coded and interleaved
 * into physical frames. Acquires the speaker. Returns false if the speaker is busy. */
bool sstv_tx_start(const uint8_t* image_bits, bool loop, SstvTxCallback cb, void* ctx);

/* Safe to call at any time, including after the transmitter finished on its own. */
void sstv_tx_stop(void);

bool sstv_tx_is_running(void);
