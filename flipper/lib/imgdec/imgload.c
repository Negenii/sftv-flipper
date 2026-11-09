#include "imgload.h"
#include <stdlib.h>
#include <string.h>
#include "tjpgd.h"
#include "uzlib.h"

/* ---------------------------------------------------------------------------------------------
 * Sink: source rows (8-bit luma) -> fit-scaled, centred, Bayer-dithered 400x240 bits.
 * Columns: each target column averages its span of source columns (or repeats one when
 * enlarging). Rows: source rows that land on the same target row are averaged; target rows a
 * source row skips (enlarging) repeat it. Only one target row is ever held.
 * ------------------------------------------------------------------------------------------- */
typedef struct {
    int w, h; /* source */
    int tw, th, ox, oy; /* target rectangle inside 400x240 */
    int y; /* next source row */
    int acc_ty; /* target row being accumulated */
    int n; /* source rows in it */
    uint32_t* acc; /* tw sums */
    uint8_t* out;
    const ImgIo* io;
    int last_pct;
} Sink;

static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static bool sink_init(Sink* s, int w, int h, uint8_t* out, const ImgIo* io) {
    memset(s, 0, sizeof *s);
    /* 32-bit maths throughout (the Flipper firmware exports no 64-bit division): sides up to
     * 65535 keep every product below 2^31 */
    if(w <= 0 || h <= 0 || w > 65535 || h > 65535) return false;
    s->w = w;
    s->h = h;
    if((int32_t)w * IMG_H > (int32_t)h * IMG_W) { /* wider than 5:3 */
        s->tw = IMG_W;
        s->th = (int)(((int32_t)h * IMG_W + w / 2) / w);
    } else {
        s->th = IMG_H;
        s->tw = (int)(((int32_t)w * IMG_H + h / 2) / h);
    }
    if(s->tw < 1) s->tw = 1;
    if(s->th < 1) s->th = 1;
    s->ox = (IMG_W - s->tw) / 2;
    s->oy = (IMG_H - s->th) / 2;
    s->acc = calloc((size_t)s->tw, sizeof(uint32_t));
    s->out = out;
    s->io = io;
    s->last_pct = -1;
    memset(out, 0, IMG_W * IMG_H / 8); /* white */
    return s->acc != NULL;
}

static void sink_emit(Sink* s, int ty) {
    if(ty < 0 || ty >= s->th || s->n == 0) return;
    int Y = s->oy + ty;
    uint8_t* row = s->out + Y * (IMG_W / 8);
    for(int tx = 0; tx < s->tw; tx++) {
        int X = s->ox + tx;
        uint32_t g = s->acc[tx] / (uint32_t)s->n;
        /* black when luma < (bayer + 0.5) / 16 * 255, as the web's ditherRGBA */
        if(g * 32 < (uint32_t)(2 * bayer[Y & 3][X & 3] + 1) * 255) row[X >> 3] |= (uint8_t)(0x80 >> (X & 7));
    }
}

static void sink_row(Sink* s, const uint8_t* g) {
    if(s->y >= s->h) return;
    int ty = (int)((int32_t)s->y * s->th / s->h);
    if(s->n > 0 && ty != s->acc_ty) {
        for(int r = s->acc_ty; r < ty; r++) sink_emit(s, r); /* enlarging fills skipped rows */
        memset(s->acc, 0, sizeof(uint32_t) * (size_t)s->tw);
        s->n = 0;
    }
    s->acc_ty = ty;
    for(int tx = 0; tx < s->tw; tx++) {
        int x0 = (int)((int32_t)tx * s->w / s->tw);
        int x1 = (int)((int32_t)(tx + 1) * s->w / s->tw);
        if(x1 <= x0) x1 = x0 + 1;
        if(x1 > s->w) x1 = s->w;
        uint32_t sum = 0;
        for(int x = x0; x < x1; x++) sum += g[x];
        s->acc[tx] += sum / (uint32_t)(x1 - x0);
    }
    s->n++;
    s->y++;
    int pct = (int)((int32_t)s->y * 100 / s->h);
    if(pct != s->last_pct && s->io->progress) {
        s->last_pct = pct;
        s->io->progress(s->io->progress_ctx, pct);
    }
}

static void sink_finish(Sink* s) {
    if(s->n > 0)
        for(int r = s->acc_ty; r < s->th; r++) sink_emit(s, r);
    free(s->acc);
    s->acc = NULL;
}

static uint8_t luma(int r, int g, int b, int a) {
    int l = (299 * r + 587 * g + 114 * b) / 1000;
    return (uint8_t)((l * a + 255 * (255 - a)) / 255); /* alpha over white */
}

static void set_err(char* err, size_t n, const char* msg) {
    if(err && n) {
        strncpy(err, msg, n - 1);
        err[n - 1] = 0;
    }
}

static bool read_exact(const ImgIo* io, void* buf, int n) {
    uint8_t* p = buf;
    while(n > 0) {
        int r = io->read(io->ctx, p, n);
        if(r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

/* ------------------------------------------------- JPEG (TJpgDec) --------------------------- */
typedef struct {
    const ImgIo* io;
    Sink sink;
    uint8_t* strip; /* ws x mh luma */
    int ws, hs, mh, strip_top, strip_rows;
} JpgCtx;

static size_t jpg_in(JDEC* jd, uint8_t* buf, size_t n) {
    JpgCtx* c = jd->device;
    if(buf) {
        int r = c->io->read(c->io->ctx, buf, (int)n);
        return r > 0 ? (size_t)r : 0;
    }
    uint8_t tmp[64]; /* skip */
    size_t done = 0;
    while(done < n) {
        int k = (int)(n - done < sizeof tmp ? n - done : sizeof tmp);
        int r = c->io->read(c->io->ctx, tmp, k);
        if(r <= 0) break;
        done += (size_t)r;
    }
    return done;
}

static void jpg_flush(JpgCtx* c) {
    for(int r = 0; r < c->strip_rows && c->strip_top + r < c->hs; r++) sink_row(&c->sink, c->strip + r * c->ws);
    c->strip_rows = 0;
}

static int jpg_out(JDEC* jd, void* bitmap, JRECT* rect) {
    JpgCtx* c = jd->device;
    if(rect->top != c->strip_top) {
        jpg_flush(c);
        c->strip_top = rect->top;
    }
    const uint8_t* src = bitmap;
    int w = rect->right - rect->left + 1;
    for(int y = rect->top; y <= rect->bottom; y++) {
        int r = y - c->strip_top;
        if(r < 0 || r >= c->mh) continue;
        if(r + 1 > c->strip_rows) c->strip_rows = r + 1;
        for(int x = 0; x < w; x++) {
            int X = rect->left + x;
            if(X < c->ws) c->strip[r * c->ws + X] = src[(y - rect->top) * w + x];
        }
    }
    return 1;
}

static bool load_jpeg(const ImgIo* io, uint8_t* out, char* err, size_t en) {
    JpgCtx c;
    memset(&c, 0, sizeof c);
    c.io = io;
    JDEC jd;
    size_t pool_size = 4096;
    void* pool = malloc(pool_size);
    if(!pool) { set_err(err, en, "out of memory"); return false; }
    JRESULT r = jd_prepare(&jd, jpg_in, pool, pool_size, &c);
    if(r != JDR_OK) {
        free(pool);
        set_err(err, en, r == JDR_FMT3 ? "unsupported JPEG (progressive?)" : "not a readable JPEG");
        return false;
    }
    /* largest built-in downscale (1/2^s) that still leaves at least the target size */
    int tw, th;
    {
        int w = jd.width, h = jd.height;
        if((int32_t)w * IMG_H > (int32_t)h * IMG_W) { tw = IMG_W; th = (int)(((int32_t)h * IMG_W + w / 2) / w); }
        else { th = IMG_H; tw = (int)(((int32_t)w * IMG_H + h / 2) / h); }
    }
    int s = 3;
    while(s > 0 && ((int)(jd.width >> s) < tw || (int)(jd.height >> s) < th)) s--;
    c.ws = (int)(jd.width >> s); /* the decoder's own scaled size (a partial last pixel is dropped) */
    c.hs = (int)(jd.height >> s);
    if(c.ws < 1) c.ws = 1;
    if(c.hs < 1) c.hs = 1;
    c.mh = (jd.msy * 8) >> s;
    if(c.mh < 1) c.mh = 1;
    c.strip = malloc((size_t)c.ws * (size_t)c.mh);
    if(c.strip) memset(c.strip, 255, (size_t)c.ws * (size_t)c.mh);
    if(!c.strip || !sink_init(&c.sink, c.ws, c.hs, out, io)) {
        free(c.strip);
        free(pool);
        set_err(err, en, "out of memory");
        return false;
    }
    c.strip_top = 0;
    r = jd_decomp(&jd, jpg_out, (uint8_t)s);
    if(r == JDR_OK) jpg_flush(&c);
    sink_finish(&c.sink);
    free(c.strip);
    free(pool);
    if(r != JDR_OK) { set_err(err, en, r == JDR_FMT3 ? "unsupported JPEG (progressive?)" : "JPEG data error"); return false; }
    return true;
}

/* ------------------------------------------------- PNG (uzlib) ------------------------------ */
typedef struct {
    struct uzlib_uncomp d; /* first: the read callback casts back to the context */
    const ImgIo* io;
    uint32_t left; /* bytes left in the current IDAT chunk */
    bool eof;
    uint8_t buf[512];
} PngIn;

static int png_read_cb(struct uzlib_uncomp* d) {
    PngIn* p = (PngIn*)d;
    while(p->left == 0) {
        if(p->eof) return -1;
        uint8_t h[12]; /* previous chunk's CRC + next chunk's length and type */
        if(!read_exact(p->io, h, 12)) { p->eof = true; return -1; }
        if(memcmp(h + 8, "IDAT", 4) != 0) { p->eof = true; return -1; }
        p->left = be32(h + 4);
    }
    int n = p->left < sizeof p->buf ? (int)p->left : (int)sizeof p->buf;
    int r = p->io->read(p->io->ctx, p->buf, n);
    if(r <= 0) { p->eof = true; return -1; }
    p->left -= (uint32_t)r;
    d->source = p->buf + 1;
    d->source_limit = p->buf + r;
    return p->buf[0];
}

static uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

static bool load_png(const ImgIo* io, uint8_t* out, char* err, size_t en) {
    uint8_t sig[8], h[8];
    if(!read_exact(io, sig, 8) || memcmp(sig, "\x89PNG\r\n\x1a\n", 8) != 0) { set_err(err, en, "not a PNG"); return false; }
    uint32_t w = 0, hh = 0;
    int depth = 0, ctype = 0;
    uint8_t pal[256][4];
    memset(pal, 0, sizeof pal);
    for(int i = 0; i < 256; i++) pal[i][3] = 255;
    for(;;) { /* header chunks up to the first IDAT */
        if(!read_exact(io, h, 8)) { set_err(err, en, "PNG ends early"); return false; }
        uint32_t len = be32(h);
        if(memcmp(h + 4, "IHDR", 4) == 0) {
            uint8_t d[13];
            if(len != 13 || !read_exact(io, d, 13)) { set_err(err, en, "bad PNG header"); return false; }
            w = be32(d);
            hh = be32(d + 4);
            depth = d[8];
            ctype = d[9];
            if(d[12] != 0) { set_err(err, en, "interlaced PNG not supported"); return false; }
            len = 0;
        } else if(memcmp(h + 4, "PLTE", 4) == 0) {
            for(uint32_t i = 0; i < len / 3 && i < 256; i++) {
                uint8_t rgb[3];
                if(!read_exact(io, rgb, 3)) return false;
                pal[i][0] = rgb[0]; pal[i][1] = rgb[1]; pal[i][2] = rgb[2];
            }
            len -= (len / 3 < 256 ? len / 3 : 256) * 3;
        } else if(memcmp(h + 4, "tRNS", 4) == 0 && ctype == 3) {
            for(uint32_t i = 0; i < len && i < 256; i++) {
                uint8_t a;
                if(!read_exact(io, &a, 1)) return false;
                pal[i][3] = a;
            }
            len -= len < 256 ? len : 256;
        } else if(memcmp(h + 4, "IDAT", 4) == 0) {
            if(w == 0) { set_err(err, en, "bad PNG"); return false; }
            /* inflate from here */
            int chans = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
            if(!chans || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)) { set_err(err, en, "unsupported PNG type"); return false; }
            size_t bpp_bits = (size_t)chans * (size_t)depth;
            size_t rowbytes = ((size_t)w * bpp_bits + 7) / 8;
            if(rowbytes > 16384) { set_err(err, en, "PNG too wide"); return false; }
            int bpp = (int)((bpp_bits + 7) / 8);
            PngIn* pin = calloc(1, sizeof(PngIn));
            uint8_t* dict = malloc(32768);
            uint8_t* cur = malloc(rowbytes + 1);
            uint8_t* prev = calloc(rowbytes + 1, 1);
            uint8_t* gray = malloc(w);
            Sink sink;
            bool ok = pin && dict && cur && prev && gray && sink_init(&sink, (int)w, (int)hh, out, io);
            if(!ok) {
                free(pin); free(dict); free(cur); free(prev); free(gray);
                set_err(err, en, "out of memory (image too big)");
                return false;
            }
            pin->io = io;
            pin->left = len;
            uzlib_init();
            uzlib_uncompress_init(&pin->d, dict, 32768);
            pin->d.source = NULL;
            pin->d.source_limit = NULL;
            pin->d.source_read_cb = png_read_cb;
            if(uzlib_zlib_parse_header(&pin->d) < 0) ok = false;
            for(uint32_t y = 0; ok && y < hh; y++) {
                pin->d.dest_start = pin->d.dest = cur;
                pin->d.dest_limit = cur + rowbytes + 1;
                int res = uzlib_uncompress(&pin->d);
                if(res < 0 || pin->d.dest != pin->d.dest_limit) { ok = false; break; }
                uint8_t f = cur[0];
                uint8_t* r = cur + 1;
                uint8_t* p = prev + 1;
                for(size_t i = 0; i < rowbytes; i++) {
                    int a = i >= (size_t)bpp ? r[i - bpp] : 0, b = p[i], c = i >= (size_t)bpp ? p[i - bpp] : 0;
                    switch(f) {
                    case 1: r[i] = (uint8_t)(r[i] + a); break;
                    case 2: r[i] = (uint8_t)(r[i] + b); break;
                    case 3: r[i] = (uint8_t)(r[i] + ((a + b) >> 1)); break;
                    case 4: r[i] = (uint8_t)(r[i] + paeth(a, b, c)); break;
                    default: break;
                    }
                }
                for(uint32_t x = 0; x < w; x++) {
                    int R, G, B, A = 255;
                    if(depth == 16) {
                        const uint8_t* q = r + (size_t)x * chans * 2; /* high bytes */
                        if(ctype == 0) R = G = B = q[0];
                        else if(ctype == 4) { R = G = B = q[0]; A = q[2]; }
                        else if(ctype == 2) { R = q[0]; G = q[2]; B = q[4]; }
                        else { R = q[0]; G = q[2]; B = q[4]; A = q[6]; }
                    } else if(depth == 8) {
                        const uint8_t* q = r + (size_t)x * chans;
                        if(ctype == 0) R = G = B = q[0];
                        else if(ctype == 3) { R = pal[q[0]][0]; G = pal[q[0]][1]; B = pal[q[0]][2]; A = pal[q[0]][3]; }
                        else if(ctype == 4) { R = G = B = q[0]; A = q[1]; }
                        else if(ctype == 2) { R = q[0]; G = q[1]; B = q[2]; }
                        else { R = q[0]; G = q[1]; B = q[2]; A = q[3]; }
                    } else { /* 1/2/4-bit gray or palette */
                        size_t bit = (size_t)x * depth;
                        int v = (r[bit >> 3] >> (8 - depth - (int)(bit & 7))) & ((1 << depth) - 1);
                        if(ctype == 3) { R = pal[v][0]; G = pal[v][1]; B = pal[v][2]; A = pal[v][3]; }
                        else R = G = B = v * 255 / ((1 << depth) - 1);
                    }
                    gray[x] = luma(R, G, B, A);
                }
                sink_row(&sink, gray);
                uint8_t* t = prev; prev = cur; cur = t; /* this row is the next one's "previous" */
            }
            sink_finish(&sink);
            free(pin); free(dict); free(cur); free(prev); free(gray);
            if(!ok) set_err(err, en, "PNG data error");
            return ok;
        }
        /* skip the rest of this chunk and its CRC */
        uint8_t tmp[64];
        uint32_t skip = len + 4;
        while(skip) {
            int k = skip < sizeof tmp ? (int)skip : (int)sizeof tmp;
            if(!read_exact(io, tmp, k)) { set_err(err, en, "PNG ends early"); return false; }
            skip -= (uint32_t)k;
        }
    }
}

/* ------------------------------------------------- BMP -------------------------------------- */
static bool load_bmp(const ImgIo* io, uint8_t* out, char* err, size_t en) {
    uint8_t h[54];
    if(!read_exact(io, h, 54) || h[0] != 'B' || h[1] != 'M') { set_err(err, en, "not a BMP"); return false; }
    uint32_t off = le32(h + 10), hsz = le32(h + 14);
    int32_t w = (int32_t)le32(h + 18), hh = (int32_t)le32(h + 22);
    int bits = h[28] | h[29] << 8;
    uint32_t comp = le32(h + 30);
    if(comp != 0 && !(comp == 3 && (bits == 32 || bits == 16))) { set_err(err, en, "compressed BMP not supported"); return false; }
    if(bits != 1 && bits != 4 && bits != 8 && bits != 24 && bits != 32) { set_err(err, en, "unsupported BMP depth"); return false; }
    bool top_down = hh < 0;
    if(hh < 0) hh = -hh;
    if(w <= 0 || hh == 0) { set_err(err, en, "bad BMP"); return false; }
    uint8_t pal[256][4];
    memset(pal, 0, sizeof pal);
    if(bits <= 8) {
        if(!io->seek(io->ctx, 14 + hsz)) return false;
        int n = 1 << bits;
        for(int i = 0; i < n; i++) if(!read_exact(io, pal[i], 4)) return false; /* B G R x */
    }
    size_t stride = (((size_t)w * bits + 31) / 32) * 4;
    if(stride > 16384) { set_err(err, en, "BMP too wide"); return false; }
    uint8_t* row = malloc(stride);
    uint8_t* gray = malloc((size_t)w);
    Sink sink;
    if(!row || !gray || !sink_init(&sink, w, hh, out, io)) { free(row); free(gray); set_err(err, en, "out of memory"); return false; }
    bool ok = true;
    for(int32_t y = 0; y < hh && ok; y++) { /* top row first: bottom-up files are read backwards */
        int32_t fy = top_down ? y : hh - 1 - y;
        if(!io->seek(io->ctx, off + (uint32_t)fy * (uint32_t)stride) || !read_exact(io, row, (int)stride)) { ok = false; break; }
        for(int32_t x = 0; x < w; x++) {
            int R, G, B;
            if(bits == 24) { B = row[x * 3]; G = row[x * 3 + 1]; R = row[x * 3 + 2]; }
            else if(bits == 32) { B = row[x * 4]; G = row[x * 4 + 1]; R = row[x * 4 + 2]; }
            else {
                size_t bit = (size_t)x * bits;
                int v = (row[bit >> 3] >> (8 - bits - (int)(bit & 7))) & ((1 << bits) - 1);
                B = pal[v][0]; G = pal[v][1]; R = pal[v][2];
            }
            gray[x] = luma(R, G, B, 255);
        }
        sink_row(&sink, gray);
    }
    sink_finish(&sink);
    free(row);
    free(gray);
    if(!ok) set_err(err, en, "BMP ends early");
    return ok;
}

/* ------------------------------------------------- PBM (P4) --------------------------------- */
static bool pbm_token(const ImgIo* io, int* v) {
    uint8_t c;
    *v = 0;
    do { if(!read_exact(io, &c, 1)) return false; if(c == '#') while(c != '\n') if(!read_exact(io, &c, 1)) return false; } while(c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '#');
    int n = 0;
    while(c >= '0' && c <= '9') { n = n * 10 + (c - '0'); if(!read_exact(io, &c, 1)) break; }
    *v = n;
    return true;
}

static bool load_pbm_any(const ImgIo* io, uint8_t* out, char* err, size_t en) {
    uint8_t m[2];
    int w, hh;
    if(!read_exact(io, m, 2) || m[0] != 'P' || m[1] != '4' || !pbm_token(io, &w) || !pbm_token(io, &hh) || w <= 0 || hh <= 0) { set_err(err, en, "not a P4 PBM"); return false; }
    size_t stride = ((size_t)w + 7) / 8;
    uint8_t* row = malloc(stride);
    uint8_t* gray = malloc((size_t)w);
    Sink sink;
    if(!row || !gray || !sink_init(&sink, w, hh, out, io)) { free(row); free(gray); set_err(err, en, "out of memory"); return false; }
    bool ok = true;
    for(int y = 0; y < hh && ok; y++) {
        if(!read_exact(io, row, (int)stride)) { ok = false; break; }
        for(int x = 0; x < w; x++) gray[x] = ((row[x >> 3] >> (7 - (x & 7))) & 1) ? 0 : 255;
        sink_row(&sink, gray);
    }
    sink_finish(&sink);
    free(row);
    free(gray);
    if(!ok) set_err(err, en, "PBM ends early");
    return ok;
}

bool img_from_bits(const uint8_t* bits, int w, int h, int stride, bool lsb_first, uint8_t* out) {
    static const ImgIo no_io = {0};
    uint8_t* gray = malloc((size_t)w);
    Sink sink;
    if(!gray || !sink_init(&sink, w, h, out, &no_io)) { free(gray); return false; }
    for(int y = 0; y < h; y++) {
        const uint8_t* row = bits + (size_t)y * (size_t)stride;
        for(int x = 0; x < w; x++) {
            int b = lsb_first ? (row[x >> 3] >> (x & 7)) & 1 : (row[x >> 3] >> (7 - (x & 7))) & 1;
            gray[x] = b ? 0 : 255;
        }
        sink_row(&sink, gray);
    }
    sink_finish(&sink);
    free(gray);
    return true;
}

bool img_load(const ImgIo* io, const char* filename, uint8_t* out, char* err, size_t err_size) {
    uint8_t magic[8] = {0};
    if(!io->seek(io->ctx, 0) || !read_exact(io, magic, 8) || !io->seek(io->ctx, 0)) { set_err(err, err_size, "can't read file"); return false; }
    (void)filename; /* the content decides, not the extension */
    if(magic[0] == 0xFF && magic[1] == 0xD8) return load_jpeg(io, out, err, err_size);
    if(memcmp(magic, "\x89PNG", 4) == 0) return load_png(io, out, err, err_size);
    if(magic[0] == 'B' && magic[1] == 'M') return load_bmp(io, out, err, err_size);
    if(magic[0] == 'P' && magic[1] == '4') return load_pbm_any(io, out, err, err_size);
    set_err(err, err_size, "unknown image format");
    return false;
}
