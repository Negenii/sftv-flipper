#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parses a binary PBM ("P4") header. On success returns 0, sets width/height and the
 * byte offset of the first pixel row. Returns -1 for anything else (P1, truncated, junk). */
int pbm_parse_header(const uint8_t* data, size_t len, int* width, int* height, size_t* data_offset);

#ifdef __cplusplus
}
#endif
