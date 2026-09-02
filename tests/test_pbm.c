#include "test_util.h"
#include "pbm.h"
#include <string.h>

int main(void) {
    int w = 0, h = 0;
    size_t off = 0;

    const char* simple = "P4\n400 240\n";
    CHECK_EQ_INT(pbm_parse_header((const uint8_t*)simple, strlen(simple), &w, &h, &off), 0);
    CHECK_EQ_INT(w, 400);
    CHECK_EQ_INT(h, 240);
    CHECK_EQ_INT(off, strlen(simple));

    const char* commented = "P4\n# made by tool\n400\n# again\n 240\n";
    CHECK_EQ_INT(pbm_parse_header((const uint8_t*)commented, strlen(commented), &w, &h, &off), 0);
    CHECK_EQ_INT(w, 400);
    CHECK_EQ_INT(h, 240);
    CHECK_EQ_INT(off, strlen(commented));

    const char* ascii = "P1\n400 240\n";
    CHECK_EQ_INT(pbm_parse_header((const uint8_t*)ascii, strlen(ascii), &w, &h, &off), -1);

    const char* truncated = "P4\n400";
    CHECK_EQ_INT(pbm_parse_header((const uint8_t*)truncated, strlen(truncated), &w, &h, &off), -1);

    const char* junk = "P4\nabc 240\n";
    CHECK_EQ_INT(pbm_parse_header((const uint8_t*)junk, strlen(junk), &w, &h, &off), -1);
    TEST_MAIN_END();
}
