#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include <input/input.h>
#include <furi_hal_light.h>

#include "sstv_params.h"
#include "pbm.h"
#include "sstv_preview.h"
#include "sstv_tx.h"
#include <gui/icon_animation.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include "sftv_tx_icons.h"
#include "sstv_notes_presets.h"

#define TAG "SftvTx"
#define SSTV_DATA_DIR EXT_PATH("apps_data/sftv_tx")
#define SSTV_OLD_DATA_DIR EXT_PATH("apps_data/sstv_tx") /* before the rename to SFTV */
#define SSTV_FILE_MAX (SSTV_IMAGE_BYTES + 256)
#define MAX_FILES 24
#define TILES_PER_PAGE 3

typedef enum {
    StateMenu, /* top menu: Send image / Send note */
    StateBrowse, /* tile grid of the pictures on the card */
    StateNoteList, /* pick a preset note or start a new one */
    StateNotePreview, /* the note text, ready to send */
    StatePreview,
    StateTransmit,
    StateDone,
    StateTest, /* temporary tone test (hold Up in the browser), Back stops */
} AppState;

typedef enum {
    EvInput,
    EvFrame,
    EvTxDone,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
    uint16_t line;
} AppEvent;

typedef struct {
    Gui* gui;
    ViewPort* vp;
    FuriMessageQueue* queue;
    FuriMutex* mutex;
    FuriString* path;
    uint8_t* image; /* SSTV_IMAGE_BYTES */
    uint8_t preview[SSTV_PREVIEW_BYTES]; /* 80x48 sheet drawn on screen, same dither the receiver gets */
    AppState state;
    uint16_t frames_sent;
    uint16_t frames_total;
    uint16_t lines_done; /* image lines already transmitted (progress) */
    bool coded;
    char name[32]; /* file name without directory, sent in the meta block */
    /* picture browser */
    Storage* storage;
    char files[MAX_FILES][32];
    uint8_t thumbs[MAX_FILES][SSTV_TINY_BYTES]; /* 40x20 tiles */
    int file_count;
    int file_index;
    IconAnimation* scan_anim; /* animated logo (image scanning in) in the Send-image tile */
    int menu_index; /* top menu: 0 image, 1 note */
    int note_index; /* note list cursor: 0 = New note, 1.. = presets */
    bool is_note; /* the current transmission is a text note */
    char note_text[SSTV_NOTE_MAX_CHARS + 1];
    char note_name[SSTV_NAME_MAX + 1];
    ViewDispatcher* kbd_vd; /* temporary dispatcher used only for the note keyboard */
    FuriThread* led_thread; /* follows the symbol on the speaker while transmitting */
    volatile bool led_run;
} SstvApp;

/* The LED shows the data as it plays: off for the silence symbol, blue for tones (brighter
 * for higher tones, green instead of blue during outer-code parity frames), red during the sync burst. Polled every 2 ms from its own thread
 * because the LED driver sits on I2C and cannot be touched from the timer ISR. */
static int32_t led_thread_fn(void* ctx) {
    SstvApp* app = ctx;
    int last = -99;
    bool last_parity = false;
    int ui_tick = 0;
    while(app->led_run) {
        if(++ui_tick >= 100) { /* ~200 ms: keep the panel fresh even during the paused hold */
            ui_tick = 0;
            view_port_update(app->vp);
        }
        int sym = sstv_tx_current_symbol();
        bool parity = sstv_tx_current_is_parity(); /* parity frames glow green instead of blue */
        if(sym != last || parity != last_parity) {
            last = sym;
            last_parity = parity;
            if(sym == -1) { /* sync */
                furi_hal_light_set(LightRed, 0xFF);
                furi_hal_light_set(LightGreen, 0);
                furi_hal_light_set(LightBlue, 0);
            } else if(sym <= 0) { /* silence or idle */
                furi_hal_light_set(LightRed | LightGreen | LightBlue, 0);
            } else {
                static const uint8_t level[SSTV_NUM_TONES] = {0, 0x40, 0x90, 0xFF};
                uint8_t v = sym < SSTV_NUM_TONES ? level[sym] : 0xFF;
                furi_hal_light_set(LightRed, 0);
                furi_hal_light_set(LightGreen, parity ? v : 0);
                furi_hal_light_set(LightBlue, parity ? 0 : v);
            }
        }
        furi_delay_ms(2);
    }
    furi_hal_light_set(LightRed | LightGreen | LightBlue, 0);
    return 0;
}

static void led_start(SstvApp* app) {
    if(app->led_run) return;
    app->led_run = true;
    app->led_thread = furi_thread_alloc_ex("SstvLed", 1024, led_thread_fn, app);
    furi_thread_start(app->led_thread);
}

static void led_stop(SstvApp* app) {
    if(!app->led_run) return;
    app->led_run = false;
    furi_thread_join(app->led_thread);
    furi_thread_free(app->led_thread);
    app->led_thread = NULL;
}

/* ---- drawing ---- */

/* Layout: the picture is an 80x48 sheet with a frame on the left; a panel on the right shows
 * state, frame count and a vertical progress bar. As lines go out the sheet slides up and
 * leaves through the top edge, as if fed into a shredder, uncovering a dolphin resting under
 * a palm (the Playdate shows the same sheet coming in from its bottom edge). */
#define SHEET_X 3
#define SHEET_Y 8
#define SHEET_BOTTOM 63
#define PANEL_X 88

static void hline_clipped(Canvas* canvas, int x0, int x1, int y) {
    if(y < 0 || y > SHEET_BOTTOM) return;
    canvas_draw_line(canvas, x0, y, x1, y);
}

static void vline_clipped(Canvas* canvas, int x, int y0, int y1) {
    if(y0 < 0) y0 = 0;
    if(y1 > SHEET_BOTTOM) y1 = SHEET_BOTTOM;
    if(y0 > y1) return;
    canvas_draw_line(canvas, x, y0, x, y1);
}

/* What lies under the sheet: the resting dolphin low on the screen and a speech bubble above. */
static void draw_under_layer(Canvas* canvas) {
    canvas_draw_icon(canvas, SHEET_X, 64 - SSTV_PREVIEW_H, &I_dolphin_done);
    canvas_set_font(canvas, FontSecondary);
    const char* text = "Sent!";
    int tw = canvas_string_width(canvas, text);
    int bx = SHEET_X + 6, by = 1, bw = tw + 8, bh = 12;
    canvas_draw_rframe(canvas, bx, by, bw, bh, 3);
    canvas_draw_str(canvas, bx + 4, by + 9, text);
    /* tail towards the dolphin, who lies to the right */
    int tx = bx + bw - 6;
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_line(canvas, tx - 4, by + bh - 1, tx, by + bh - 1);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_line(canvas, tx - 5, by + bh - 1, tx + 2, by + bh + 3);
    canvas_draw_line(canvas, tx + 1, by + bh - 1, tx + 2, by + bh + 3);
}

/* Top row of the picture. The sheet slides out across the whole transmission, so progress
 * counts every frame (START, tiny, preview, schedule, picture data and outer-code parity), not
 * only the 240 image lines. Resting at SHEET_Y, fully out at -SSTV_PREVIEW_H. */
static int sheet_top(const SstvApp* app) {
    if(app->state == StatePreview || app->frames_total == 0) return SHEET_Y;
    int travel = SHEET_Y + SSTV_PREVIEW_H;
    int prog = (int)((uint32_t)app->frames_sent * (uint32_t)travel / app->frames_total);
    return SHEET_Y - prog;
}

static void draw_sheet(Canvas* canvas, const uint8_t* preview, int y0) {
    /* paper: white box (clipped) so it covers the layer underneath */
    int top = y0 < 0 ? 0 : y0, bottom = y0 + SSTV_PREVIEW_H - 1;
    if(bottom > SHEET_BOTTOM) bottom = SHEET_BOTTOM;
    if(bottom >= top) {
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, SHEET_X, top, SSTV_PREVIEW_W, bottom - top + 1);
        canvas_set_color(canvas, ColorBlack);
    }
    for(int py = 0; py < SSTV_PREVIEW_H; py++) {
        int y = y0 + py;
        if(y < 0 || y > SHEET_BOTTOM) continue;
        const uint8_t* row = preview + py * SSTV_PREVIEW_LINE_BYTES;
        for(int px = 0; px < SSTV_PREVIEW_W; px++) {
            if(row[px >> 3] & (0x80 >> (px & 7))) canvas_draw_dot(canvas, SHEET_X + px, y);
        }
    }
    /* frame one pixel outside the picture */
    hline_clipped(canvas, SHEET_X - 1, SHEET_X + SSTV_PREVIEW_W, y0 - 1);
    hline_clipped(canvas, SHEET_X - 1, SHEET_X + SSTV_PREVIEW_W, y0 + SSTV_PREVIEW_H);
    vline_clipped(canvas, SHEET_X - 1, y0 - 1, y0 + SSTV_PREVIEW_H);
    vline_clipped(canvas, SHEET_X + SSTV_PREVIEW_W, y0 - 1, y0 + SSTV_PREVIEW_H);
}

/* Button hint: icon plus a word, baseline at `y`. */
static void hint(Canvas* canvas, const Icon* icon, int icon_h, const char* text, int y) {
    canvas_draw_icon(canvas, PANEL_X, y - icon_h + 1, icon);
    canvas_draw_str(canvas, PANEL_X + 13, y, text);
}

static void draw_panel(Canvas* canvas, SstvApp* app) {
    const int w = 128 - PANEL_X; /* 40 px */
    char buf[16];
    canvas_draw_line(canvas, PANEL_X - 3, 0, PANEL_X - 3, 63);
    canvas_set_font(canvas, FontSecondary);
    switch(app->state) {
    case StateMenu:
    case StateBrowse:
    case StateNoteList:
    case StateNotePreview:
    case StateTest:
        break;
    case StatePreview:
        canvas_draw_str(canvas, PANEL_X, 9, "Ready");
        hint(canvas, &I_Ok_btn_9x9, 9, "send", 52);
        hint(canvas, &I_back_10px, 10, "files", 63);
        break;
    case StateTransmit: {
        bool paused = sstv_tx_is_paused();
        canvas_draw_str(canvas, PANEL_X, 9, paused ? "Paused" : app->is_note ? "TX note" : app->coded ? "TX zip" : "TX raw");
        float p = app->frames_total ? (float)app->frames_sent / app->frames_total : 0.0f;
        elements_progress_bar(canvas, PANEL_X, 12, w, p);
        if(app->frames_total > 200) {
            /* tenths matter only on long passes (big uncompressed image); on a short one
             * each frame is ~1 %, so the decimal is noise */
            int pct10 = (int)((uint32_t)app->frames_sent * 1000u / app->frames_total);
            snprintf(buf, sizeof buf, "%d.%d%%", pct10 / 10, pct10 % 10);
        } else {
            int pct = app->frames_total ? (int)((uint32_t)app->frames_sent * 100u / app->frames_total) : 0;
            snprintf(buf, sizeof buf, "%d%%", pct);
        }
        canvas_draw_str(canvas, PANEL_X, 32, buf);
        uint32_t left_s = (uint32_t)(app->frames_total - app->frames_sent) * (SSTV_PHYS_US / 1000) / 1000;
        snprintf(buf, sizeof buf, "~%lu:%02lu", (unsigned long)(left_s / 60), (unsigned long)(left_s % 60));
        canvas_draw_str(canvas, PANEL_X, 42, buf);
        if(paused) {
            hint(canvas, &I_Ok_btn_9x9, 9, "resume", 52);
            hint(canvas, &I_back_10px, 10, "stop", 63);
        } else {
            hint(canvas, &I_back_10px, 10, "pause", 63);
        }
        break;
    }
    case StateDone: {
        canvas_draw_str(canvas, PANEL_X, 9, "Done");
        elements_progress_bar(canvas, PANEL_X, 12, w, 1.0f);
        uint32_t total_s = (uint32_t)app->frames_total * (SSTV_PHYS_US / 1000) / 1000;
        snprintf(buf, sizeof buf, "%lu:%02lu", (unsigned long)(total_s / 60), (unsigned long)(total_s % 60));
        canvas_draw_str(canvas, PANEL_X, 32, buf);
        hint(canvas, &I_Ok_btn_9x9, 9, "again", 52);
        hint(canvas, &I_back_10px, 10, "files", 63);
        break;
    }
    }
}

/* Fits `name` into `max_w` pixels, ending in ".." when cut. */
static void fit_name(Canvas* canvas, char* out, size_t cap, const char* name, int max_w) {
    strncpy(out, name, cap - 1);
    out[cap - 1] = 0;
    char* dot = strrchr(out, '.');
    if(dot && dot != out) *dot = 0;
    if(out[0] >= 'a' && out[0] <= 'z') out[0] -= 32; /* show every name capitalised */
    if((int)canvas_string_width(canvas, out) <= max_w) return;
    size_t n = strlen(out);
    while(n > 1) {
        n--;
        out[n] = 0;
        strcat(out, "..");
        if((int)canvas_string_width(canvas, out) <= max_w) return;
        out[n] = 0;
    }
}

/* Tile grid: three 40x20 thumbnails per page, name under each, cursor as a double frame. */
static void scan_anim_cb(IconAnimation* instance, void* ctx) {
    UNUSED(instance);
    SstvApp* app = ctx;
    view_port_update(app->vp);
}

static void draw_browse(Canvas* canvas, SstvApp* app) {
    char buf[32];
    /* "TX" pill, then the title */
    canvas_set_font(canvas, FontSecondary);
    int pw = canvas_string_width(canvas, "TX") + 6;
    canvas_draw_rbox(canvas, 2, 1, pw, 11, 3);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_str(canvas, 5, 10, "TX");
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2 + pw + 4, 10, "Slow Flipper TV");
    if(app->file_count == 0) {
        canvas_draw_str(canvas, 2, 30, "No pictures. Put 400x240");
        canvas_draw_str(canvas, 2, 40, "P4 .pbm files into");
        canvas_draw_str(canvas, 2, 50, "apps_data/sftv_tx");
        return;
    }
    int page = app->file_index / TILES_PER_PAGE * TILES_PER_PAGE;
    for(int k = 0; k < TILES_PER_PAGE; k++) {
        int idx = page + k;
        if(idx >= app->file_count) break;
        /* pictures at x = 1, 44, 87, each framed: two empty pixels between the frames;
         * the cursor thickens its frame inwards */
        int x = 1 + k * 43, y = 17;
        const uint8_t* t = app->thumbs[idx];
        for(int py = 0; py < SSTV_TINY_H; py++)
            for(int px = 0; px < SSTV_TINY_W; px++)
                if(t[py * SSTV_TINY_LINE_BYTES + (px >> 3)] & (0x80 >> (px & 7))) canvas_draw_dot(canvas, x + px, y + py);
        canvas_draw_frame(canvas, x - 1, y - 1, SSTV_TINY_W + 2, SSTV_TINY_H + 2);
        if(idx == app->file_index) canvas_draw_frame(canvas, x, y, SSTV_TINY_W, SSTV_TINY_H);
        fit_name(canvas, buf, sizeof buf, app->files[idx], SSTV_TINY_W + 2);
        canvas_draw_str(canvas, x + (SSTV_TINY_W - (int)canvas_string_width(canvas, buf)) / 2, y + SSTV_TINY_H + 12, buf);
    }
    canvas_draw_icon(canvas, 2, 54, &I_Ok_btn_9x9);
    canvas_draw_str(canvas, 14, 62, "open");
    canvas_draw_icon(canvas, 44, 53, &I_back_10px);
    canvas_draw_str(canvas, 56, 62, "exit");
    snprintf(buf, sizeof buf, "%d/%d", app->file_index + 1, app->file_count);
    canvas_draw_str(canvas, 126 - canvas_string_width(canvas, buf), 62, buf);
}

/* ---- top menu ---- */
static void draw_menu(Canvas* canvas, SstvApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Slow Flipper TV");
    canvas_set_font(canvas, FontSecondary);
    struct { const char* label; } rows[2] = {{"Send image"}, {"Send note"}};
    for(int i = 0; i < 2; i++) {
        int y = 20 + i * 20; /* row box top */
        if(i == app->menu_index) {
            canvas_draw_rbox(canvas, 2, y, 124, 18, 3);
            canvas_set_color(canvas, ColorWhite);
        }
        if(i == 0 && app->scan_anim)
            canvas_draw_icon_animation(canvas, 7, y + 3, app->scan_anim);
        else
            canvas_draw_str(canvas, 8, y + 13, "T"); /* note marker */
        canvas_draw_str(canvas, 26, y + 13, rows[i].label);
        canvas_set_color(canvas, ColorBlack);
    }
    canvas_draw_icon(canvas, 2, 54, &I_Ok_btn_9x9);
    canvas_draw_str(canvas, 14, 62, "open");
    canvas_draw_icon(canvas, 44, 53, &I_back_10px);
    canvas_draw_str(canvas, 56, 62, "exit");
}

/* ---- note list (New note + presets) ---- */
#define NOTE_LIST_LEN (1 + SSTV_NOTE_PRESET_COUNT)
static void note_list_label(int idx, char* out, size_t cap) {
    if(idx == 0) {
        strncpy(out, "New note...", cap - 1);
        out[cap - 1] = 0;
    } else {
        strncpy(out, SSTV_NOTE_PRESETS[idx - 1], cap - 1);
        out[cap - 1] = 0;
    }
}

static void draw_note_list(Canvas* canvas, SstvApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Send note");
    canvas_set_font(canvas, FontSecondary);
    const int rows = 4, rh = 11;
    int first = app->note_index - app->note_index % rows;
    for(int r = 0; r < rows; r++) {
        int idx = first + r;
        if(idx >= NOTE_LIST_LEN) break;
        int y = 16 + r * rh; /* box top */
        char label[40];
        note_list_label(idx, label, sizeof label);
        /* fit to width */
        while(canvas_string_width(canvas, label) > 108 && strlen(label) > 1) label[strlen(label) - 1] = 0;
        if(idx == app->note_index) {
            canvas_draw_rbox(canvas, 2, y, 124, rh, 2);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 6, y + 9, label);
        canvas_set_color(canvas, ColorBlack);
    }
    canvas_draw_icon(canvas, 2, 54, &I_Ok_btn_9x9);
    canvas_draw_str(canvas, 14, 62, app->note_index == 0 ? "type" : "edit");
    canvas_draw_icon(canvas, 44, 53, &I_back_10px);
    canvas_draw_str(canvas, 56, 62, "back");
}

/* Word-wrapped note text in a box; used for the preview. */
static void draw_wrapped(Canvas* canvas, const char* text, int x0, int y0, int w, int line_h, int max_lines) {
    char line[64];
    int ll = 0, lines = 0, y = y0;
    const char* p = text;
    while(*p && lines < max_lines) {
        /* take the next word */
        const char* ws = p;
        while(*ws == ' ') ws++;
        const char* we = ws;
        while(*we && *we != ' ') we++;
        int wlen = (int)(we - ws);
        char cand[64];
        int cl = ll;
        if(cl > 0) cand[cl++] = ' ';
        for(int i = 0; i < wlen && cl < (int)sizeof cand - 1; i++) cand[cl++] = ws[i];
        cand[cl] = 0;
        if(canvas_string_width(canvas, cand) <= w || ll == 0) {
            memcpy(line, cand, cl + 1);
            ll = cl;
        } else {
            line[ll] = 0;
            canvas_draw_str(canvas, x0, y, line);
            y += line_h;
            lines++;
            ll = 0;
            if(wlen < (int)sizeof line) {
                memcpy(line, ws, wlen);
                ll = wlen;
                line[ll] = 0;
            }
        }
        p = we;
    }
    if(ll > 0 && lines < max_lines) {
        line[ll] = 0;
        canvas_draw_str(canvas, x0, y, line);
    }
}

static void draw_note_preview(Canvas* canvas, SstvApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Note");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_frame(canvas, 1, 13, 126, 38);
    draw_wrapped(canvas, app->note_text, 4, 22, 120, 9, 4);
    canvas_draw_icon(canvas, 2, 54, &I_Ok_btn_9x9);
    canvas_draw_str(canvas, 14, 62, "send");
    canvas_draw_icon(canvas, 44, 53, &I_back_10px);
    canvas_draw_str(canvas, 56, 62, "back");
}

static void draw_callback(Canvas* canvas, void* ctx) {
    SstvApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    if(app->state == StateMenu) {
        draw_menu(canvas, app);
        furi_mutex_release(app->mutex);
        return;
    }
    if(app->state == StateBrowse) {
        draw_browse(canvas, app);
        furi_mutex_release(app->mutex);
        return;
    }
    if(app->state == StateNoteList) {
        draw_note_list(canvas, app);
        furi_mutex_release(app->mutex);
        return;
    }
    if(app->state == StateNotePreview) {
        draw_note_preview(canvas, app);
        furi_mutex_release(app->mutex);
        return;
    }
    if((app->state == StateTransmit || app->state == StateDone) && app->is_note) {
        /* a note has no picture sheet: show the text in a paper box, panel on the right */
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_frame(canvas, 1, 2, PANEL_X - 5, 50);
        draw_wrapped(canvas, app->note_text, 4, 11, PANEL_X - 11, 9, 5);
        draw_panel(canvas, app);
        furi_mutex_release(app->mutex);
        return;
    }
    if(app->state == StateTest) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, "Tone test");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 26, "sync + 164 x 4 ms symbols:");
        canvas_draw_str(canvas, 2, 36, "silence 2500 3000 3500");
        canvas_draw_str(canvas, 2, 46, "4000 4500 Hz, looping");
        hint(canvas, &I_back_10px, 10, "stop", 63);
        furi_mutex_release(app->mutex);
        return;
    }
    /* the layer underneath appears once the sheet is flush with the top edge */
    if(app->state == StateDone || (app->state == StateTransmit && sheet_top(app) <= 0)) draw_under_layer(canvas);
    if(app->state != StateDone) draw_sheet(canvas, app->preview, sheet_top(app));
    draw_panel(canvas, app);
    furi_mutex_release(app->mutex);
}

/* ---- events ---- */

static void input_callback(InputEvent* input, void* ctx) {
    SstvApp* app = ctx;
    AppEvent ev = {.type = EvInput, .input = *input};
    furi_message_queue_put(app->queue, &ev, FuriWaitForever);
}

/* ISR context */
static void tx_callback(void* ctx, uint16_t line, bool done) {
    SstvApp* app = ctx;
    AppEvent ev = {.type = done ? EvTxDone : EvFrame, .line = line};
    furi_message_queue_put(app->queue, &ev, 0);
}

/* ---- file loading ---- */

static bool load_pbm(Storage* storage, const char* path, uint8_t* image) {
    bool ok = false;
    File* file = storage_file_alloc(storage);
    uint8_t* buf = malloc(SSTV_FILE_MAX);
    do {
        if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) break;
        uint64_t size = storage_file_size(file);
        if(size > SSTV_FILE_MAX) break;
        size_t got = storage_file_read(file, buf, (uint16_t)size);
        if(got != size) break;
        int w, h;
        size_t off;
        if(pbm_parse_header(buf, got, &w, &h, &off) != 0) break;
        if(w != SSTV_WIDTH || h != SSTV_HEIGHT) break;
        if(got < off + SSTV_IMAGE_BYTES) break;
        memcpy(image, buf + off, SSTV_IMAGE_BYTES);
        ok = true;
    } while(false);
    storage_file_close(file);
    storage_file_free(file);
    free(buf);
    return ok;
}

/* Lists the .pbm files in the data directory and builds a 40x20 tile for each. */
static void scan_files(SstvApp* app) {
    app->file_count = 0;
    File* dir = storage_file_alloc(app->storage);
    if(storage_dir_open(dir, SSTV_DATA_DIR)) {
        FileInfo info;
        char name[64];
        while(app->file_count < MAX_FILES && storage_dir_read(dir, &info, name, sizeof name)) {
            if(info.flags & FSF_DIRECTORY) continue;
            size_t n = strlen(name);
            if(n < 5 || strcasecmp(name + n - 4, ".pbm") != 0) continue;
            strncpy(app->files[app->file_count], name, sizeof(app->files[0]) - 1);
            app->files[app->file_count][sizeof(app->files[0]) - 1] = 0;
            app->file_count++;
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    /* sort by name so the order is stable */
    for(int i = 1; i < app->file_count; i++)
        for(int j = i; j > 0 && strcmp(app->files[j - 1], app->files[j]) > 0; j--) {
            char tmp[32];
            memcpy(tmp, app->files[j], 32);
            memcpy(app->files[j], app->files[j - 1], 32);
            memcpy(app->files[j - 1], tmp, 32);
        }
    FuriString* path = furi_string_alloc();
    for(int i = 0; i < app->file_count; i++) {
        furi_string_printf(path, "%s/%s", SSTV_DATA_DIR, app->files[i]);
        if(load_pbm(app->storage, furi_string_get_cstr(path), app->image)) {
            sstv_preview_make_tiny(app->image, app->thumbs[i]);
        } else {
            memset(app->thumbs[i], 0x55, SSTV_TINY_BYTES); /* unreadable: grey tile */
        }
    }
    furi_string_free(path);
    if(app->file_index >= app->file_count) app->file_index = 0;
}

/* Loads the selected picture for the preview screen. */
static bool open_file(SstvApp* app) {
    if(app->file_index < 0 || app->file_index >= app->file_count) return false;
    furi_string_printf(app->path, "%s/%s", SSTV_DATA_DIR, app->files[app->file_index]);
    if(!load_pbm(app->storage, furi_string_get_cstr(app->path), app->image)) return false;
    sstv_preview_make(app->image, app->preview);
    /* file name without directory or extension, sent to the receiver in the meta block */
    strncpy(app->name, app->files[app->file_index], sizeof(app->name) - 1);
    app->name[sizeof(app->name) - 1] = 0;
    char* dot = strrchr(app->name, '.');
    if(dot && dot != app->name) *dot = 0;
    if(app->name[0] >= 'a' && app->name[0] <= 'z') app->name[0] -= 32; /* capitalised on the receiver too */
    return true;
}

/* ---- main viewport loop; returns when the user leaves to the file browser ---- */

static void set_state(SstvApp* app, AppState s) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->state = s;
    if(s == StatePreview) {
        app->frames_sent = 0;
        app->lines_done = 0;
    }
    furi_mutex_release(app->mutex);
    if(s == StateTransmit || s == StateTest) {
        led_start(app);
    } else {
        led_stop(app);
    }
    view_port_update(app->vp);
}

static void start_tx(SstvApp* app) {
    if(sstv_tx_start(app->image, app->name, false, tx_callback, app)) {
        app->frames_total = (uint16_t)sstv_tx_total_frames();
        app->coded = sstv_tx_is_coded();
        app->is_note = false;
        app->lines_done = 0;
        set_state(app, StateTransmit);
    } else {
        FURI_LOG_E(TAG, "speaker busy");
    }
}

/* Derives the meta name (gallery label) from the first line of the note, capitalised. */
static void note_make_name(SstvApp* app) {
    int n = 0;
    for(const char* t = app->note_text; *t && *t != '\n' && n < SSTV_NAME_MAX; t++) app->note_name[n++] = *t;
    app->note_name[n] = 0;
    if(n == 0) strcpy(app->note_name, "Note");
    if(app->note_name[0] >= 'a' && app->note_name[0] <= 'z') app->note_name[0] -= 32;
}

static void start_tx_note(SstvApp* app) {
    note_make_name(app);
    if(sstv_tx_start_note(app->note_text, app->note_name, tx_callback, app)) {
        app->frames_total = (uint16_t)sstv_tx_total_frames();
        app->coded = false;
        app->is_note = true;
        app->lines_done = 0;
        set_state(app, StateTransmit);
    } else {
        FURI_LOG_E(TAG, "speaker busy");
    }
}

/* Note keyboard on a temporary ViewDispatcher (the app itself runs a raw ViewPort, so the
 * standard text_input view is hosted only for the duration of the edit). Returns true if the
 * user confirmed. note_text is edited in place (prefilled for a preset). */
static bool g_note_kbd_ok;
static void note_kbd_result(void* ctx) {
    SstvApp* app = ctx;
    g_note_kbd_ok = true;
    view_dispatcher_stop(app->kbd_vd);
}
static bool note_kbd_back(void* ctx) {
    SstvApp* app = ctx;
    g_note_kbd_ok = false;
    view_dispatcher_stop(app->kbd_vd);
    return true;
}
static bool edit_note(SstvApp* app) {
    g_note_kbd_ok = false;
    gui_remove_view_port(app->gui, app->vp);
    ViewDispatcher* vd = view_dispatcher_alloc();
    app->kbd_vd = vd;
    TextInput* ti = text_input_alloc();
    text_input_set_header_text(ti, "Note (ASCII, max 200)");
    text_input_set_result_callback(
        ti, note_kbd_result, app, app->note_text, SSTV_NOTE_MAX_CHARS + 1, false);
    view_dispatcher_set_event_callback_context(vd, app);
    view_dispatcher_set_navigation_event_callback(vd, note_kbd_back);
    view_dispatcher_add_view(vd, 0, text_input_get_view(ti));
    view_dispatcher_attach_to_gui(vd, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(vd, 0);
    view_dispatcher_run(vd);
    view_dispatcher_remove_view(vd, 0);
    text_input_free(ti);
    view_dispatcher_free(vd);
    app->kbd_vd = NULL;
    gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);
    return g_note_kbd_ok;
}

static void start_test(SstvApp* app) {
    if(sstv_tx_start_test(tx_callback, app)) {
        set_state(app, StateTest);
    } else {
        FURI_LOG_E(TAG, "speaker busy");
    }
}

static void run_viewport(SstvApp* app) {
    set_state(app, StateMenu);
    gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);

    bool leave = false;
    while(!leave) {
        AppEvent ev;
        if(furi_message_queue_get(app->queue, &ev, FuriWaitForever) != FuriStatusOk) continue;
        switch(ev.type) {
        case EvFrame:
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->frames_sent = ev.line + 1;
            app->lines_done = (uint16_t)sstv_tx_lines_after(app->frames_sent);
            furi_mutex_release(app->mutex);
            view_port_update(app->vp);
            break;
        case EvTxDone:
            sstv_tx_stop();
            set_state(app, StateDone);
            break;
        case EvInput:
            if(app->state == StateBrowse && ev.input.type == InputTypeLong && ev.input.key == InputKeyUp) {
                start_test(app);
                break;
            }
            if(ev.input.type != InputTypeShort && ev.input.type != InputTypeRepeat) break;
            if(app->state == StateMenu) {
                if(ev.input.key == InputKeyUp || ev.input.key == InputKeyDown) app->menu_index ^= 1;
                if(ev.input.key == InputKeyOk && ev.input.type == InputTypeShort) {
                    if(app->menu_index == 0) {
                        set_state(app, StateBrowse);
                    } else {
                        app->note_index = 0;
                        set_state(app, StateNoteList);
                    }
                }
                if(ev.input.key == InputKeyBack && ev.input.type == InputTypeShort) leave = true;
                view_port_update(app->vp);
                break;
            }
            if(app->state == StateBrowse) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                int n = app->file_count;
                if(n > 0) {
                    if(ev.input.key == InputKeyRight) app->file_index = (app->file_index + 1) % n;
                    if(ev.input.key == InputKeyLeft) app->file_index = (app->file_index + n - 1) % n;
                    if(ev.input.key == InputKeyDown) app->file_index = (app->file_index + TILES_PER_PAGE) % n;
                    if(ev.input.key == InputKeyUp) app->file_index = (app->file_index + n - TILES_PER_PAGE % n) % n;
                }
                furi_mutex_release(app->mutex);
                if(ev.input.key == InputKeyBack && ev.input.type == InputTypeShort) set_state(app, StateMenu);
                if(ev.input.key == InputKeyOk && ev.input.type == InputTypeShort && open_file(app)) set_state(app, StatePreview);
                view_port_update(app->vp);
                break;
            }
            if(app->state == StateNoteList) {
                if(ev.input.key == InputKeyDown) app->note_index = (app->note_index + 1) % NOTE_LIST_LEN;
                if(ev.input.key == InputKeyUp) app->note_index = (app->note_index + NOTE_LIST_LEN - 1) % NOTE_LIST_LEN;
                if(ev.input.key == InputKeyBack && ev.input.type == InputTypeShort) set_state(app, StateMenu);
                if(ev.input.key == InputKeyOk && ev.input.type == InputTypeShort) {
                    if(app->note_index == 0) {
                        app->note_text[0] = 0;
                    } else {
                        strncpy(app->note_text, SSTV_NOTE_PRESETS[app->note_index - 1], SSTV_NOTE_MAX_CHARS);
                        app->note_text[SSTV_NOTE_MAX_CHARS] = 0;
                    }
                    set_state(app, edit_note(app) ? StateNotePreview : StateNoteList);
                }
                view_port_update(app->vp);
                break;
            }
            if(app->state == StateNotePreview) {
                if(ev.input.key == InputKeyBack && ev.input.type == InputTypeShort) set_state(app, StateNoteList);
                if(ev.input.key == InputKeyOk && ev.input.type == InputTypeShort) start_tx_note(app);
                view_port_update(app->vp);
                break;
            }
            if(ev.input.type != InputTypeShort) break;
            if(ev.input.key == InputKeyBack) {
                if(app->state == StateTest) {
                    sstv_tx_stop();
                    set_state(app, StateBrowse);
                } else if(app->state == StateTransmit) {
                    /* first Back pauses; while paused, Back stops */
                    if(sstv_tx_is_paused()) {
                        sstv_tx_stop();
                        set_state(app, app->is_note ? StateNotePreview : StatePreview);
                    } else {
                        sstv_tx_pause();
                        view_port_update(app->vp);
                    }
                } else if(app->state == StateDone) {
                    sstv_tx_stop();
                    set_state(app, app->is_note ? StateNotePreview : StatePreview);
                } else {
                    set_state(app, StateMenu);
                }
            } else if(ev.input.key == InputKeyOk) {
                if(app->state == StateTransmit && sstv_tx_is_paused()) {
                    sstv_tx_resume();
                    view_port_update(app->vp);
                } else if(app->state == StateDone) {
                    if(app->is_note) start_tx_note(app);
                    else start_tx(app);
                } else if(app->state == StatePreview) {
                    start_tx(app);
                }
            }
            break;
        }
    }
    sstv_tx_stop();
    gui_remove_view_port(app->gui, app->vp);
}

int32_t sftv_app_main(void* p) {
    UNUSED(p);
    SstvApp* app = malloc(sizeof(SstvApp));
    memset(app, 0, sizeof *app);
    app->queue = furi_message_queue_alloc(16, sizeof(AppEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->path = furi_string_alloc();
    app->image = malloc(SSTV_IMAGE_BYTES);
    app->gui = furi_record_open(RECORD_GUI);
    app->vp = view_port_alloc();
    view_port_draw_callback_set(app->vp, draw_callback, app);
    view_port_input_callback_set(app->vp, input_callback, app);
    app->scan_anim = icon_animation_alloc(&A_scan);
    icon_animation_set_update_callback(app->scan_anim, scan_anim_cb, app);
    icon_animation_start(app->scan_anim);

    app->storage = furi_record_open(RECORD_STORAGE);
    /* the app used to be called SSTV: carry the pictures over */
    FileInfo info;
    if(storage_common_stat(app->storage, SSTV_OLD_DATA_DIR, &info) == FSE_OK &&
       storage_common_stat(app->storage, SSTV_DATA_DIR, &info) != FSE_OK) {
        storage_common_rename(app->storage, SSTV_OLD_DATA_DIR, SSTV_DATA_DIR);
    }
    storage_simply_mkdir(app->storage, SSTV_DATA_DIR);
    scan_files(app);
    run_viewport(app);

    furi_record_close(RECORD_STORAGE);
    icon_animation_stop(app->scan_anim);
    icon_animation_free(app->scan_anim);
    view_port_free(app->vp);
    furi_record_close(RECORD_GUI);
    led_stop(app);
    furi_hal_light_set(LightRed | LightGreen | LightBlue, 0);
    free(app->image);
    furi_string_free(app->path);
    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->queue);
    free(app);
    return 0;
}
