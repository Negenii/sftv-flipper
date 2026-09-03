#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/elements.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <input/input.h>
#include <furi_hal_light.h>

#include "sstv_params.h"
#include "pbm.h"
#include "sstv_preview.h"
#include "sstv_tx.h"
#include "sstv_tx_icons.h"

#define TAG "SstvTx"
#define SSTV_DATA_DIR EXT_PATH("apps_data/sstv_tx")
#define SSTV_FILE_MAX (SSTV_IMAGE_BYTES + 256)

typedef enum {
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

/* What lies under the sheet: the resting dolphin and a word. */
static void draw_under_layer(Canvas* canvas) {
    canvas_draw_icon(canvas, SHEET_X, SHEET_Y, &I_dolphin_done);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, SHEET_X + 3, SHEET_Y + 12, "Sent!");
}

static void draw_sheet(Canvas* canvas, const uint8_t* preview, int lines_done) {
    int y0 = SHEET_Y - lines_done * SSTV_PREVIEW_H / SSTV_HEIGHT; /* top row of the picture */
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

static void draw_callback(Canvas* canvas, void* ctx) {
    SstvApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    if(app->state != StatePreview) draw_under_layer(canvas);
    if(app->state != StateDone) draw_sheet(canvas, app->preview, app->state == StatePreview ? 0 : app->lines_done);
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

static void show_error(DialogsApp* dialogs, const char* text) {
    DialogMessage* msg = dialog_message_alloc();
    dialog_message_set_header(msg, "SSTV TX", 64, 2, AlignCenter, AlignTop);
    dialog_message_set_text(msg, text, 64, 32, AlignCenter, AlignCenter);
    dialog_message_set_buttons(msg, NULL, "OK", NULL);
    dialog_message_show(dialogs, msg);
    dialog_message_free(msg);
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
    set_state(app, StatePreview);
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
            if(ev.input.type != InputTypeShort) break;
            if(ev.input.key == InputKeyBack) {
                if(app->state == StateTransmit || app->state == StateDone) {
                    sstv_tx_stop();
                    set_state(app, StatePreview);
                } else {
                    leave = true;
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

int32_t sstv_app_main(void* p) {
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

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, SSTV_DATA_DIR);
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);

    while(true) {
        DialogsFileBrowserOptions opts;
        dialog_file_browser_set_basic_options(&opts, ".pbm", NULL);
        opts.base_path = SSTV_DATA_DIR;
        furi_string_set(app->path, SSTV_DATA_DIR);
        if(!dialog_file_browser_show(dialogs, app->path, app->path, &opts)) break;
        if(!load_pbm(storage, furi_string_get_cstr(app->path), app->image)) {
            show_error(dialogs, "Need a P4 .pbm\nexactly 400x240");
            continue;
        }
        sstv_preview_make(app->image, app->preview);
        {
            /* file name without directory, sent to the receiver in the meta block */
            const char* full = furi_string_get_cstr(app->path);
            const char* base = strrchr(full, '/');
            base = base ? base + 1 : full;
            strncpy(app->name, base, sizeof(app->name) - 1);
            app->name[sizeof(app->name) - 1] = 0;
            char* dot = strrchr(app->name, '.');
            if(dot && dot != app->name) *dot = 0; /* extension is implied (.pbm) */
        }
        run_viewport(app);
    }

    furi_record_close(RECORD_DIALOGS);
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
