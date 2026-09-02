#include "test_util.h"
#include "sstv_modem_ref.h"
#include "sstv_frame.h"
#include <stdlib.h>
#include <string.h>

int main(void) {
    sstv_mod m;
    sstv_mod_init(&m, 44100.0f, 10000);
    CHECK(m.f0 == (float)SSTV_F0_HZ);
    CHECK(m.f1 == (float)SSTV_F1_HZ);
    CHECK(m.fsync == (float)SSTV_FSYNC_HZ);

    /* 1 ms at 44.1 kHz is 44.1 samples: ten tones must total 441 samples. */
    int16_t buf[1024];
    int total = 0;
    for(int i = 0; i < 10; i++) {
        int n = sstv_mod_tone(&m, 2000.0f, 1000, buf, 1024);
        CHECK(n == 44 || n == 45);
        total += n;
    }
    CHECK_EQ_INT(total, 441);
    CHECK_EQ_INT(m.samples_out, 441);

    /* Square wave: only +A and -A, roughly half of each over whole cycles. */
    sstv_mod_init(&m, 44100.0f, 10000);
    int n = sstv_mod_tone(&m, 2000.0f, 10000, buf, 1024); /* 20 cycles */
    CHECK_EQ_INT(n, 441);
    int pos = 0;
    for(int i = 0; i < n; i++) {
        CHECK(buf[i] == 10000 || buf[i] == -10000);
        if(buf[i] > 0) pos++;
    }
    CHECK(pos > 200 && pos < 241);

    /* Too small a buffer is an error and does not advance time. */
    sstv_mod_init(&m, 44100.0f, 10000);
    CHECK_EQ_INT(sstv_mod_tone(&m, 2000.0f, 1000000, buf, 1024), -1);
    CHECK_EQ_INT(m.samples_out, 0);

    /* One frame: 5 ms sync + 416 ms of bits = 421 ms = 18566 samples. */
    uint8_t pixels[SSTV_LINE_BYTES], frame[SSTV_FRAME_BYTES];
    memset(pixels, 0xA5, sizeof pixels);
    sstv_frame_pack(7, pixels, frame);
    int16_t* big = malloc(sizeof(int16_t) * 20000);
    sstv_mod_init(&m, 44100.0f, 10000);
    CHECK_EQ_INT(sstv_mod_frame(&m, frame, big, 20000), 18566);
    free(big);

    /* Full image capacity: 1 s + 240 * 0.421 s = 102.04 s. */
    int cap = sstv_mod_image_capacity(44100.0f);
    CHECK(cap >= 4499964 && cap <= 4500100);
    uint8_t* image = calloc(SSTV_IMAGE_BYTES, 1);
    int16_t* all = malloc(sizeof(int16_t) * (size_t)cap);
    sstv_mod_init(&m, 44100.0f, 10000);
    int img_n = sstv_mod_image(&m, image, all, cap);
    CHECK(img_n >= 4499960 && img_n <= cap);
    free(all);
    free(image);
    TEST_MAIN_END();
}
