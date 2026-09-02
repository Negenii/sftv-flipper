#include "test_util.h"
#include "crc8.h"
#include <string.h>

int main(void) {
    const char* check = "123456789";
    CHECK_EQ_INT(crc8_compute((const uint8_t*)check, strlen(check)), 0xF4);
    CHECK_EQ_INT(crc8_compute(NULL, 0), 0x00);
    uint8_t one[1] = {0x01};
    CHECK_EQ_INT(crc8_compute(one, 1), 0x07);
    uint8_t a[3] = {1, 2, 3}, b[3] = {1, 2, 2};
    CHECK(crc8_compute(a, 3) != crc8_compute(b, 3));
    TEST_MAIN_END();
}
