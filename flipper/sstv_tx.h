#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Called from the timer ISR after each line finishes. `done` is true after line 239 of each
 * pass; transmission then continues from line 0 until sstv_tx_stop().
 * Only ISR-safe calls are allowed inside (e.g. furi_message_queue_put with timeout 0). */
typedef void (*SstvTxCallback)(void* ctx, uint16_t line, bool done);

/* image_bits: SSTV_IMAGE_BYTES of P4 raster (1 = black). Copied into pre-packed frames.
 * Acquires the speaker. Returns false if the speaker is busy or already transmitting. */
bool sstv_tx_start(const uint8_t* image_bits, SstvTxCallback cb, void* ctx);

/* Safe to call at any time, including after the transmitter finished on its own. */
void sstv_tx_stop(void);

bool sstv_tx_is_running(void);
