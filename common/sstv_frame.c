#include "sstv_frame.h"
#include "crc8.h"
#include <string.h>

void sstv_frame_pack(uint8_t line_no, const uint8_t* pixels, uint8_t* out) {
    out[0] = line_no;
    memcpy(out + 1, pixels, SSTV_LINE_BYTES);
    out[SSTV_FRAME_BYTES - 1] = crc8_compute(out, 1 + SSTV_LINE_BYTES);
}

bool sstv_frame_unpack(const uint8_t* in, uint8_t* line_no, uint8_t* pixels) {
    *line_no = in[0];
    memcpy(pixels, in + 1, SSTV_LINE_BYTES);
    return crc8_compute(in, 1 + SSTV_LINE_BYTES) == in[SSTV_FRAME_BYTES - 1];
}
