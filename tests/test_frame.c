#include "test_util.h"
#include "sstv_frame.h"
#include <string.h>

int main(void) {
    uint8_t pixels[SSTV_LINE_BYTES];
    for(int i = 0; i < SSTV_LINE_BYTES; i++) pixels[i] = (uint8_t)(i * 37 + 11);

    uint8_t frame[SSTV_FRAME_BYTES];
    sstv_frame_pack(123, pixels, frame);
    CHECK_EQ_INT(frame[0], 123);
    CHECK(memcmp(frame + 1, pixels, SSTV_LINE_BYTES) == 0);

    uint8_t line_no = 0, out[SSTV_LINE_BYTES];
    CHECK(sstv_frame_unpack(frame, &line_no, out));
    CHECK_EQ_INT(line_no, 123);
    CHECK(memcmp(out, pixels, SSTV_LINE_BYTES) == 0);

    /* bit accessor: MSB first. 123 = 0b01111011 */
    CHECK_EQ_INT(sstv_frame_bit(frame, 0), 0);
    CHECK_EQ_INT(sstv_frame_bit(frame, 1), 1);
    CHECK_EQ_INT(sstv_frame_bit(frame, 7), 1);
    CHECK_EQ_INT(sstv_frame_bit(frame, 8), (pixels[0] >> 7) & 1);
    CHECK_EQ_INT(sstv_frame_bit(frame, 415), frame[51] & 1);

    /* a flipped pixel bit is detected */
    frame[20] ^= 0x10;
    CHECK(!sstv_frame_unpack(frame, &line_no, out));
    CHECK_EQ_INT(line_no, 123); /* outputs filled regardless */

    CHECK_EQ_INT(SSTV_FRAME_BITS, 8 * SSTV_FRAME_BYTES);
    CHECK_EQ_INT(SSTV_IMAGE_BYTES, SSTV_LINE_BYTES * SSTV_HEIGHT);
    TEST_MAIN_END();
}
