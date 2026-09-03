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
#include "sstv_tx.h"

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
    EvLedTick,
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
    AppState state;
    uint16_t frames_sent;
    FuriTimer* led_timer; /* 100 ms tick while transmitting: blue blink, violet on sync */
    uint8_t led_phase;
    uint8_t led_violet_ticks; /* ticks left of the violet sync flash */
} SstvApp;

/* Blue blinks while data symbols go out; every frame boundary (= sync burst) flashes violet. */
static void led_apply(SstvApp* app) {
    if(app->state != StateTransmit) {
        furi_hal_light_set(LightRed | LightGreen | LightBlue, 0);
        return;
    }
    if(app->led_violet_ticks > 0) {
        furi_hal_light_set(LightRed, 0x60);
        furi_hal_light_set(LightGreen, 0);
        furi_hal_light_set(LightBlue, 0xFF);
    } else {
        furi_hal_light_set(LightRed, 0);
        furi_hal_light_set(LightGreen, 0);
        furi_hal_light_set(LightBlue, (app->led_phase & 1) ? 0xFF : 0x18);
    }
}

static void led_timer_callback(void* ctx) {
    SstvApp* app = ctx;
    AppEvent ev = {.type = EvLedTick};
    furi_message_queue_put(app->queue, &ev, 0);
}

/* ---- drawing ---- */

static void draw_preview(Canvas* canvas, const uint8_t* image) {
    for(int y = 0; y < 64; y++) {
        int sy = y * SSTV_HEIGHT / 64;
        const uint8_t* row = image + sy * SSTV_LINE_BYTES;
        for(int x = 0; x < 128; x++) {
            int sx = x * SSTV_WIDTH / 128;
            if(row[sx >> 3] & (0x80 >> (sx & 7))) canvas_draw_dot(canvas, x, y);
        }
    }
}

static void draw_callback(Canvas* canvas, void* ctx) {
    SstvApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    draw_preview(canvas, app->image);

    /* status strip */
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 0, 53, 128, 11);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_line(canvas, 0, 53, 127, 53);
    canvas_set_font(canvas, FontSecondary);

    char buf[32];
    switch(app->state) {
    case StatePreview:
        canvas_draw_str(canvas, 2, 62, "OK: send   Back: files");
        break;
    case StateTransmit:
        snprintf(buf, sizeof buf, "TX %u/%u", app->frames_sent, (unsigned)SSTV_NUM_SEGS);
        canvas_draw_str(canvas, 2, 62, buf);
        elements_progress_bar(canvas, 62, 55, 64, (float)app->frames_sent / SSTV_NUM_SEGS);
        break;
    case StateDone:
        canvas_draw_str(canvas, 2, 62, "Done.  OK: again   Back");
        break;
    }
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
    if(s == StatePreview) app->frames_sent = 0;
    furi_mutex_release(app->mutex);
    if(s == StateTransmit) {
        app->led_phase = 0;
        app->led_violet_ticks = 1;
        furi_timer_start(app->led_timer, furi_ms_to_ticks(100));
    } else {
        furi_timer_stop(app->led_timer);
    }
    led_apply(app);
    view_port_update(app->vp);
}

static void start_tx(SstvApp* app) {
    if(sstv_tx_start(app->image, false, tx_callback, app)) {
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
            furi_mutex_release(app->mutex);
            app->led_violet_ticks = 1; /* the next frame's sync is playing now */
            led_apply(app);
            view_port_update(app->vp);
            break;
        case EvLedTick:
            app->led_phase++;
            if(app->led_violet_ticks > 0) app->led_violet_ticks--;
            led_apply(app);
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
    app->led_timer = furi_timer_alloc(led_timer_callback, FuriTimerTypePeriodic, app);
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
        run_viewport(app);
    }

    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    view_port_free(app->vp);
    furi_record_close(RECORD_GUI);
    furi_timer_stop(app->led_timer);
    furi_timer_free(app->led_timer);
    furi_hal_light_set(LightRed | LightGreen | LightBlue, 0);
    free(app->image);
    furi_string_free(app->path);
    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->queue);
    free(app);
    return 0;
}
