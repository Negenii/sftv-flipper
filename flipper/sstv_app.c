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
#include "sftv_tx_icons.h"

#define TAG "SftvTx"
#define SSTV_DATA_DIR EXT_PATH("apps_data/sftv_tx")
#define SSTV_OLD_DATA_DIR EXT_PATH("apps_data/sstv_tx") /* before the rename to SFTV */
#define SSTV_FILE_MAX (SSTV_IMAGE_BYTES + 256)
#define MAX_FILES 24
#define TILES_PER_PAGE 3

typedef enum {
    StateBrowse, /* tile grid of the pictures on the card */
    StatePreview,
    StateTransmit,
    StateDone,
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
    FuriThread* led_thread; /* follows the symbol on the speaker while transmitting */
    volatile bool led_run;
} SstvApp;

/* The LED shows the data as it plays: off for the silence symbol, blue for tones (brighter
 * for higher tones), violet during the sync burst. Polled every 2 ms from its own thread
 * because the LED driver sits on I2C and cannot be touched from the timer ISR. */
static int32_t led_thread_fn(void* ctx) {
    SstvApp* app = ctx;
    int last = -99;
    while(app->led_run) {
        int sym = sstv_tx_current_symbol();
        if(sym != last) {
            last = sym;
            if(sym == -1) { /* sync */
                furi_hal_light_set(LightRed, 0xFF);
                furi_hal_light_set(LightGreen, 0);
                furi_hal_light_set(LightBlue, 0);
            } else if(sym <= 0) { /* silence or idle */
                furi_hal_light_set(LightRed | LightGreen | LightBlue, 0);
            } else {
                static const uint8_t level[SSTV_NUM_TONES] = {0, 0x40, 0x90, 0xFF};
                furi_hal_light_set(LightRed, 0);
                furi_hal_light_set(LightGreen, 0);
                furi_hal_light_set(LightBlue, level[sym]);
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

/* Top row of the picture. While the intro frames go out (START, previews, schedule) the sheet
 * moves from its resting place up to the top edge; then each transmitted line pushes it out. */
static int intro_frames(const SstvApp* app) {
    return SSTV_ILEAVE + (app->coded ? SSTV_INTRO_FRAMES_CODED : SSTV_INTRO_FRAMES);
}

static int sheet_top(const SstvApp* app) {
    if(app->state == StatePreview) return SHEET_Y;
    int intro = intro_frames(app);
    if(app->frames_sent < intro) return SHEET_Y - SHEET_Y * app->frames_sent / intro;
    return -(app->lines_done * SSTV_PREVIEW_H / SSTV_HEIGHT);
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
    case StateBrowse:
        break;
    case StatePreview:
        canvas_draw_str(canvas, PANEL_X, 9, "Ready");
        hint(canvas, &I_Ok_btn_9x9, 9, "send", 52);
        hint(canvas, &I_back_10px, 10, "files", 63);
        break;
    case StateTransmit: {
        canvas_draw_str(canvas, PANEL_X, 9, app->coded ? "TX zip" : "TX raw");
        float p = app->frames_total ? (float)app->frames_sent / app->frames_total : 0.0f;
        elements_progress_bar(canvas, PANEL_X, 12, w, p);
        snprintf(buf, sizeof buf, "%u/%u", app->lines_done, SSTV_HEIGHT);
        canvas_draw_str(canvas, PANEL_X, 32, buf);
        uint32_t left_s = (uint32_t)(app->frames_total - app->frames_sent) * (SSTV_PHYS_US / 1000) / 1000;
        snprintf(buf, sizeof buf, "~%lu:%02lu", (unsigned long)(left_s / 60), (unsigned long)(left_s % 60));
        canvas_draw_str(canvas, PANEL_X, 42, buf);
        hint(canvas, &I_back_10px, 10, "stop", 63);
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

static void draw_callback(Canvas* canvas, void* ctx) {
    SstvApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    if(app->state == StateBrowse) {
        draw_browse(canvas, app);
        furi_mutex_release(app->mutex);
        return;
    }
    /* the layer underneath appears once the sheet is flush with the top edge */
    if(app->state == StateDone || (app->state == StateTransmit && app->frames_sent >= intro_frames(app))) draw_under_layer(canvas);
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
    if(s == StateTransmit) {
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
        app->lines_done = 0;
        set_state(app, StateTransmit);
    } else {
        FURI_LOG_E(TAG, "speaker busy");
    }
}

static void run_viewport(SstvApp* app) {
    set_state(app, StateBrowse);
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
            if(ev.input.type != InputTypeShort && ev.input.type != InputTypeRepeat) break;
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
                if(ev.input.key == InputKeyBack && ev.input.type == InputTypeShort) leave = true;
                if(ev.input.key == InputKeyOk && ev.input.type == InputTypeShort && open_file(app)) set_state(app, StatePreview);
                view_port_update(app->vp);
                break;
            }
            if(ev.input.type != InputTypeShort) break;
            if(ev.input.key == InputKeyBack) {
                if(app->state == StateTransmit || app->state == StateDone) {
                    sstv_tx_stop();
                    set_state(app, StatePreview);
                } else {
                    set_state(app, StateBrowse);
                }
            } else if(ev.input.key == InputKeyOk) {
                if(app->state == StatePreview || app->state == StateDone) start_tx(app);
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
