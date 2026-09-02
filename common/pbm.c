#include "pbm.h"

static int is_space(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

/* Skips whitespace and '#' comments. Returns new position, or len if exhausted. */
static size_t skip_ws(const uint8_t* d, size_t len, size_t p) {
    while(p < len) {
        if(is_space(d[p])) {
            p++;
        } else if(d[p] == '#') {
            while(p < len && d[p] != '\n') p++;
        } else {
            break;
        }
    }
    return p;
}

static int read_int(const uint8_t* d, size_t len, size_t* p, int* out) {
    size_t q = *p;
    if(q >= len || d[q] < '0' || d[q] > '9') return -1;
    int v = 0;
    while(q < len && d[q] >= '0' && d[q] <= '9') {
        v = v * 10 + (d[q] - '0');
        if(v > 100000) return -1;
        q++;
    }
    *p = q;
    *out = v;
    return 0;
}

int pbm_parse_header(const uint8_t* data, size_t len, int* width, int* height, size_t* data_offset) {
    if(len < 2 || data[0] != 'P' || data[1] != '4') return -1;
    size_t p = 2;
    p = skip_ws(data, len, p);
    if(read_int(data, len, &p, width) != 0) return -1;
    p = skip_ws(data, len, p);
    if(read_int(data, len, &p, height) != 0) return -1;
    /* Exactly one whitespace character separates the header from the raster. */
    if(p >= len || !is_space(data[p])) return -1;
    p++;
    *data_offset = p;
    return 0;
}
