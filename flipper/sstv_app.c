#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include <input/input.h>
#include <furi_hal_light.h>
#include <furi_hal_speaker.h>
#include <notification/notification_messages.h>

#include "sstv_params.h"
#include "pbm.h"
#include "sstv_preview.h"
#include "sstv_tx.h"
#include <gui/icon_animation.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include "sftv_tx_icons.h"
#include "sstv_notes_presets.h"
#include "sstv_font_cyr.h"
#include <dialogs/dialogs.h>
#include "imgload.h"
#include <toolbox/compress.h>
#include "sstv_anim.h"
#include "build_info.h"
#include "sstv_frame.h"

#define TAG "SftvTx"
#define SSTV_DATA_DIR EXT_PATH("apps_data/sftv_tx")
#define MAX_USER_NOTES 16
#define UN_MAX 1000 /* saved notes keep up to this many bytes each (16 of them stay in RAM) */
#define UN_STRIDE (UN_MAX + 1)
#define NOTES_PATH SSTV_DATA_DIR "/notes.txt"
#define SSTV_OLD_DATA_DIR EXT_PATH("apps_data/sstv_tx") /* before the rename to SFTV */
#define SSTV_FILE_MAX (SSTV_IMAGE_BYTES + 256)
#define TOAST_MS 2500 /* the "Deleted" notice over the grid */
#define TOAST_FRAME_MS 90 /* its trash-can animation, played once */
#define MAX_FILES 160 /* names only; tiles are made for the page on screen */
#define TILES_PER_PAGE 3

typedef enum {
    StateMenu, /* top menu: Send image / Send note */
    StateBrowse, /* tile grid of the pictures on the card */
    StateNoteList, /* pick a preset note or start a new one */
    StateNotePreview, /* the note text, ready to send */
    StatePreview,
    StateTransmit,
    StateDone,
    StateTest, /* temporary tone test (hold Up in the browser) or HF sweep (hold OK), Back stops */
    StateConvert, /* converting a picture picked from the SD card (progress, or the error) */
    StateAnim, /* a whole Flipper animation, packed and playing: OK sends it */
    StateAnimList, /* flipping through the animations on the card, each one playing; OK packs it */
    StateOptions, /* Calibrate, HF sweep, tone test, About */
    StateAbout, /* how to use it, credits */
} AppState;

typedef enum {
    EvInput,
    EvFrame,
    EvTxDone,
    EvAnimTick, /* next animation frame (timer) */
    EvToastTick, /* next frame of the "Deleted" notice (timer) */
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
    uint16_t line;
} AppEvent;

typedef struct {
    Gui* gui;
    NotificationApp* notif;
    bool sweep; /* StateTest is the HF sweep rather than the tone test */
    bool calib; /* StateTest is the calibration loop (from the menu) */
    bool backlight_held;
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
    uint8_t thumbs[TILES_PER_PAGE][SSTV_TINY_BYTES]; /* 40x20 tiles of the page on screen */
    int thumbs_page; /* first grid index those tiles belong to, -1 none */
    int file_count;
    int file_index;
    int browse_sel; /* browse grid cursor: 0 = the "SD card" tile, k = files[k - 1] */
    char last_sd_dir[128]; /* folder of the last SD pick, where the next browse starts */
    int conv_pct; /* StateConvert progress */
    char conv_err[48]; /* StateConvert failure reason, empty while converting */
    IconAnimation* scan_anim; /* animated icon in the Send-image tile */
    IconAnimation* note_anim; /* animated icon in the Send-note tile */
    int menu_anim; /* which tile is currently animating (-1 none) */
    int menu_index; /* top menu: 0 image, 1 note, 2 animation, 3 calibrate */
    IconAnimation* film_anim; /* animated icon in the Animation tile */
    IconAnimation* opts_anim; /* animated icon in the Options tile */
    int opt_index; /* Options list cursor */
    bool al_sent; /* the animation on screen in the list was sent before (it has a shortcut) */
    char last_txt_dir[128]; /* folder of the last .txt picked for a note */
    int note_index; /* note list cursor: 0 = New note, 1.. = presets */
    bool is_note; /* the current transmission is a text note */
    bool pausing; /* Back pressed: PAUSE block is being sent, not yet holding */
    char note_text[SSTV_NOTE_MAX_CHARS + 1];
    char note_name[SSTV_NAME_MAX + 1];
    ViewDispatcher* kbd_vd; /* temporary dispatcher used only for the note keyboard */
    char* user_notes; /* MAX_USER_NOTES * UN_STRIDE saved notes, persisted to notes.txt */
    int user_note_count;
    int editing_user_idx; /* user-note index behind the current preview/edit, -1 = new or preset */
    bool note_del_confirm; /* preview: confirming deletion of a saved note */
    bool pic_del_confirm; /* picture preview: confirming deletion of the .pbm */
    char toast[40]; /* grid notice ("Deleted <name>") shown until toast_until */
    uint32_t toast_until;
    FuriTimer* toast_timer; /* one-shot redraw when the notice expires */
    /* whole animation (StateAnim, and Transmit/Done with is_anim) */
    bool is_anim;
    bool conv_anim; /* StateConvert is packing an animation */
    char anim_dir[128];
    int anim_pick; /* frame picked in the file browser (Left sends just that one) */
    sstv_anim_info anim;
    uint8_t* anim_stream;
    int anim_len;
    uint8_t* anim_frame; /* frame on screen */
    CompressIcon* anim_ci;
    int anim_pos; /* position in anim.order */
    bool anim_read_fail;
    bool anim_shortcut_new; /* a .anim shortcut was written: rescan the grid on the way back */
    /* animation list (StateAnimList): roots are /ext/dolphin and every asset pack's Anims folder;
     * entries are found again by index on each step (no list of names kept in RAM) */
    char (*al_roots)[72];
    int al_root_count[8];
    int al_nroots, al_count, al_index;
    bool anim_from_list; /* StateAnim was opened from the list: Back returns there */
    FuriTimer* anim_timer;
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

/* An animation shortcut in the data folder (see read_shortcut). */
static bool is_anim_file(const char* name) {
    size_t n = strlen(name);
    return n > 5 && strcasecmp(name + n - 5, ".anim") == 0;
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

/* Small rotating-line spinner centred at (cx, cy). */
static void draw_spinner(Canvas* canvas, int cx, int cy) {
    static const int8_t dx[8] = {0, 2, 3, 2, 0, -2, -3, -2};
    static const int8_t dy[8] = {-3, -2, 0, 2, 3, 2, 0, -2};
    int k = (int)((furi_get_tick() / 90) % 8);
    canvas_draw_line(canvas, cx, cy, cx + dx[k], cy + dy[k]);
    canvas_draw_dot(canvas, cx + dx[k], cy + dy[k]);
}

/* Deletion prompt: a title and two button rows (OK delete, Back cancel), vertically centred. */
static void confirm_box(Canvas* canvas, int bx, int bw, const char* title) {
    const int by = 11, bh = 37;
    canvas_set_font(canvas, FontSecondary);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, bx, by, bw, bh);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_frame(canvas, bx, by, bw, bh);
    canvas_draw_str(canvas, bx + 5, by + 11, title);
    canvas_draw_icon(canvas, bx + 5, by + 14, &I_Ok_btn_9x9);
    canvas_draw_str(canvas, bx + 18, by + 22, "delete");
    canvas_draw_icon(canvas, bx + 5, by + 25, &I_back_10px);
    canvas_draw_str(canvas, bx + 18, by + 33, "cancel");
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
    case StateConvert:
    case StateAnim:
    case StateAnimList:
    case StateOptions:
    case StateAbout:
        break;
    case StatePreview:
        canvas_draw_str(canvas, PANEL_X, 9, "Ready");
        /* the 4 px arrow sits in the middle of the 9-10 px icon column */
        canvas_draw_icon(canvas, PANEL_X + 3, 41 - 7 + 1, &I_ButtonLeft_4x7);
        canvas_draw_str(canvas, PANEL_X + 13, 41, "delete");
        hint(canvas, &I_Ok_btn_9x9, 9, "send", 52);
        hint(canvas, &I_back_10px, 10, "files", 63);
        if(app->pic_del_confirm) {
            confirm_box(canvas, 3, PANEL_X - 9, "Delete picture?"); /* over the picture only */
        }
        break;
    case StateTransmit: {
        bool paused = sstv_tx_is_paused();
        if(paused) app->pausing = false;
        bool pausing = app->pausing;
        canvas_draw_str(canvas, PANEL_X, 9, paused ? "Paused" : pausing ? "Pausing" : app->is_note ? "TX note" : app->coded ? "TX zip" : "TX raw");
        float p = app->frames_total ? (float)app->frames_sent / app->frames_total : 0.0f;
        elements_progress_bar(canvas, PANEL_X, 12, w, p);
        if(pausing) {
            draw_spinner(canvas, PANEL_X + 6, 34);
            canvas_draw_str(canvas, PANEL_X + 16, 38, "wait");
            break;
        }
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
        uint32_t left_s = (uint32_t)(app->frames_total - app->frames_sent) * (SSTV_BAND_PHYS_US(sstv_tx_band()) / 1000) / 1000;
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
        uint32_t total_s = (uint32_t)app->frames_total * (SSTV_BAND_PHYS_US(sstv_tx_band()) / 1000) / 1000;
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
    canvas_draw_str(canvas, 2 + pw + 4, 10, "Pictures");
    int total = app->file_count + 1; /* tile 0: the SD card (pictures) or all animations */
    int page = app->browse_sel / TILES_PER_PAGE * TILES_PER_PAGE;
    for(int k = 0; k < TILES_PER_PAGE; k++) {
        int sel = page + k;
        if(sel >= total) break;
        /* pictures at x = 1, 44, 87, each framed: two empty pixels between the frames;
         * the cursor thickens its frame inwards */
        int x = 1 + k * 43, y = 17;
        if(sel == 0) { /* any JPEG / PNG / BMP from the SD card */
            canvas_draw_frame(canvas, x - 1, y - 1, SSTV_TINY_W + 2, SSTV_TINY_H + 2);
            if(app->browse_sel == 0) canvas_draw_frame(canvas, x, y, SSTV_TINY_W, SSTV_TINY_H);
            const char* big = "SD";
            const char* small = "card";
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, x + (SSTV_TINY_W - (int)canvas_string_width(canvas, big)) / 2, y + 14, big);
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, x + (SSTV_TINY_W - (int)canvas_string_width(canvas, small)) / 2, y + SSTV_TINY_H + 12, small);
            continue;
        }
        int idx = sel - 1;
        const uint8_t* t = app->thumbs[k];
        if(app->thumbs_page == page) /* (tiles for another page are still being made) */
            for(int py = 0; py < SSTV_TINY_H; py++)
                for(int px = 0; px < SSTV_TINY_W; px++)
                    if(t[py * SSTV_TINY_LINE_BYTES + (px >> 3)] & (0x80 >> (px & 7))) canvas_draw_dot(canvas, x + px, y + py);
        if(is_anim_file(app->files[idx])) { /* animation shortcut: a play mark in the corner */
            int mx = x + SSTV_TINY_W - 8, my = y + SSTV_TINY_H - 8;
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_box(canvas, mx - 1, my - 1, 8, 8);
            canvas_set_color(canvas, ColorBlack);
            for(int k = 0; k < 3; k++) canvas_draw_line(canvas, mx + 1 + k, my + k, mx + 1 + k, my + 6 - k);
        }
        canvas_draw_frame(canvas, x - 1, y - 1, SSTV_TINY_W + 2, SSTV_TINY_H + 2);
        if(sel == app->browse_sel) canvas_draw_frame(canvas, x, y, SSTV_TINY_W, SSTV_TINY_H);
        fit_name(canvas, buf, sizeof buf, app->files[idx], SSTV_TINY_W + 2);
        canvas_draw_str(canvas, x + (SSTV_TINY_W - (int)canvas_string_width(canvas, buf)) / 2, y + SSTV_TINY_H + 12, buf);
    }
    canvas_draw_icon(canvas, 2, 54, &I_Ok_btn_9x9);
    canvas_draw_str(canvas, 14, 62, "open");
    canvas_draw_icon(canvas, 44, 53, &I_back_10px);
    canvas_draw_str(canvas, 56, 62, "exit");
    snprintf(buf, sizeof buf, "%d/%d", app->browse_sel + 1, total);
    canvas_draw_str(canvas, 126 - canvas_string_width(canvas, buf), 62, buf);
    uint32_t now = furi_get_tick();
    if(app->toast[0] && now < app->toast_until) {
        /* trash can (tools/make_trash_icon.py): lid opens, the sheet drops in, lid shuts;
         * played once, then it stays on the last (closed) frame */
        static const Icon* const trash[] = {&I_trash_0, &I_trash_1, &I_trash_2, &I_trash_3, &I_trash_4, &I_trash_5,
                                            &I_trash_6, &I_trash_7, &I_trash_8, &I_trash_9, &I_trash_10};
        const int nf = (int)COUNT_OF(trash);
        uint32_t t = now - (app->toast_until - furi_ms_to_ticks(TOAST_MS));
        int f = (int)(t / furi_ms_to_ticks(TOAST_FRAME_MS));
        if(f >= nf) f = nf - 1;
        int bw = 112, bx = (128 - bw) / 2, by = 16, bh = 30;
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, bx, by, bw, bh);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_rframe(canvas, bx, by, bw, bh, 3);
        canvas_draw_icon(canvas, bx + 4, by + 3, trash[f]);
        int tx = bx + 31, tw = bx + bw - 4 - tx;
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, tx, by + 13, "Deleted");
        canvas_set_font(canvas, FontSecondary);
        fit_name(canvas, buf, sizeof buf, app->toast, tw);
        canvas_draw_str(canvas, tx, by + 24, buf);
    }
}

static void toast_timer_cb(void* ctx) {
    AppEvent ev = {.type = EvToastTick};
    furi_message_queue_put(((SstvApp*)ctx)->queue, &ev, 0);
}

/* ---- Cyrillic/UTF-8 note text via the built-in u8g2 UTF-8 path ---- */
/* Flipper's canvas_draw_str is u8g2_DrawUTF8 and canvas_string_width is u8g2_GetUTF8Width, so
 * setting a Cyrillic-capable u8g2 font is all that's needed to render Russian. */
static void note_font_begin(Canvas* c) {
    canvas_set_custom_u8g2_font(c, u8g2_font_haxrcorp4089_t_cyrillic);
}
static void note_font_end(Canvas* c) {
    canvas_set_font(c, FontSecondary);
}
/* Word-wrapped UTF-8 with hard break of overlong words. The note font must already be set;
 * baseline of the first line is at y0. */
static void note_draw_wrapped(Canvas* canvas, const char* text, int x0, int y0, int w, int line_h, int max_lines) {
    char line[256];
    int ll = 0, lines = 0, y = y0, last_space = -1;
    const char* p = text;
    for(;;) {
        const char* q = p;
        uint8_t b = (uint8_t)*p;
        if(b == 0 || b == '\n') {
            line[ll] = 0;
            canvas_draw_str(canvas, x0, y, line);
            y += line_h;
            lines++;
            ll = 0;
            last_space = -1;
            if(b == 0 || lines >= max_lines) return;
            p++;
            continue;
        }
        int adv = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1;
        if(ll + adv < (int)sizeof line - 1) {
            if(b == ' ') last_space = ll;
            memcpy(line + ll, q, (size_t)adv);
            ll += adv;
            line[ll] = 0;
        }
        p += adv;
        if((int)canvas_string_width(canvas, line) > w) {
            if(last_space > 0) {
                line[last_space] = 0;
                canvas_draw_str(canvas, x0, y, line);
                int rem = ll - last_space - 1;
                memmove(line, line + last_space + 1, (size_t)rem);
                ll = rem;
                line[ll] = 0;
            } else {
                int back = 1;
                while(back < ll && ((uint8_t)line[ll - back] & 0xC0) == 0x80) back++;
                char carry[8];
                memcpy(carry, line + ll - back, (size_t)back);
                line[ll - back] = 0;
                canvas_draw_str(canvas, x0, y, line);
                memcpy(line, carry, (size_t)back);
                ll = back;
                line[ll] = 0;
            }
            y += line_h;
            lines++;
            last_space = -1;
            if(lines >= max_lines) return;
        }
    }
}

/* ---- top menu ---- */
/* ---- Options and About ---- */
#define OPT_COUNT 2 /* Calibrate, About (the HF sweep and the audible tone test were developer tools) */
/* About is one card: name, version, build and a QR code to the project page (the how-to text
 * lives on GitHub). */

/* Draws `text` word-wrapped to `w` px from baseline y (line height lh), skipping the first
 * `skip` lines and drawing at most `max` (0: measure only). '\n' starts a new paragraph after
 * an empty line. Returns the number of lines the whole text takes. */
static int draw_wrapped(Canvas* canvas, const char* text, int x, int y, int w, int lh, int skip, int max) {
    char line[64];
    int n = 0, len = 0;
    const char* p = text;
#define EMIT()                                                                                    \
    do {                                                                                          \
        line[len] = 0;                                                                            \
        if(n >= skip && n < skip + max) canvas_draw_str(canvas, x, y + (n - skip) * lh, line);   \
        n++;                                                                                      \
        len = 0;                                                                                  \
    } while(0)
    while(*p) {
        if(*p == '\n') {
            EMIT();
            n++; /* the empty line between paragraphs */
            p++;
            continue;
        }
        while(*p == ' ') p++;
        const char* ws = p;
        while(*p && *p != ' ' && *p != '\n') p++;
        int wl = (int)(p - ws);
        if(wl <= 0) continue;
        char cand[64];
        int cl = len ? snprintf(cand, sizeof cand, "%.*s %.*s", len, line, wl, ws) : snprintf(cand, sizeof cand, "%.*s", wl, ws);
        if(len && ((int)canvas_string_width(canvas, cand) > w || cl >= (int)sizeof line)) {
            EMIT();
            cl = snprintf(cand, sizeof cand, "%.*s", wl, ws);
        }
        if(cl > (int)sizeof line - 1) cl = (int)sizeof line - 1;
        memcpy(line, cand, (size_t)cl);
        len = cl;
    }
    if(len) EMIT();
#undef EMIT
    return n;
}

/* Top menu: a 2x2 grid under the title (Animation, Image / Options, Note), an icon beside a
 * label in each tile, the selected one with a thick frame and its icon animating. No button
 * hints: OK opens, Back leaves, as everywhere on the Flipper. */
static void draw_menu(Canvas* canvas, SstvApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Slow Flipper TV");
    canvas_set_font(canvas, FontSecondary);
    /* positions: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right; the long labels get the
     * wider left column */
    IconAnimation* tiles[4] = {app->film_anim, app->scan_anim, app->opts_anim, app->note_anim};
    const Icon* icons[4] = {&A_anim_play, &A_scan, &A_options, &A_note_write};
    const char* labels[4] = {"Animation", "Image", "Options", "Note"};
    if(app->menu_anim != app->menu_index) {
        for(int i = 0; i < 4; i++)
            if(tiles[i]) icon_animation_stop(tiles[i]);
        if(tiles[app->menu_index]) icon_animation_start(tiles[app->menu_index]);
        app->menu_anim = app->menu_index;
    }
    const int th = 22, twl = 70, twr = 52;
    for(int i = 0; i < 4; i++) {
        int tw = (i % 2) ? twr : twl;
        int x = (i % 2) ? 2 + twl + 2 : 2, y = 15 + (i / 2) * (th + 3);
        canvas_draw_rframe(canvas, x, y, tw, th, 4);
        if(i == app->menu_index) {
            canvas_draw_rframe(canvas, x + 1, y + 1, tw - 2, th - 2, 3);
            canvas_draw_rframe(canvas, x + 2, y + 2, tw - 4, th - 4, 3);
        }
        /* icon on the left, the label beside it on the same row */
        int ix = x + 5, iy = y + (th - 12) / 2;
        IconAnimation* anim = tiles[i];
        if(anim && i == app->menu_index) {
            canvas_draw_icon_animation(canvas, ix, iy, anim);
        } else if(anim) { /* idle tile: the finished drawing (last frame), not the blank first one */
            uint32_t frames = icon_get_frame_count(icons[i]);
            canvas_draw_bitmap(canvas, ix, iy, icon_animation_get_width(anim), icon_animation_get_height(anim),
                               icon_get_frame_data(icons[i], frames - 1));
        }
        canvas_draw_str(canvas, ix + 13 + 4, y + th / 2 + 4, labels[i]);
    }
}

/* ---- note list (New note + presets) ---- */
#define NOTE_LIST_LEN(app) (2 + (app)->user_note_count + SSTV_NOTE_PRESET_COUNT) /* New, SD card, saved, presets */

static char* un_ptr(SstvApp* app, int i) {
    return app->user_notes + (size_t)i * UN_STRIDE;
}
static void load_user_notes(SstvApp* app) {
    app->user_note_count = 0;
    File* f = storage_file_alloc(app->storage);
    if(storage_file_open(f, NOTES_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint64_t size = storage_file_size(f);
        if(size > 0 && size < 60000) {
            char* buf = malloc((size_t)size + 1);
            uint16_t got = storage_file_read(f, buf, (uint16_t)size);
            buf[got] = 0;
            char* q = buf;
            while(*q && app->user_note_count < MAX_USER_NOTES) {
                char* nl = strchr(q, '\n');
                if(nl) *nl = 0;
                if(*q) {
                    strncpy(un_ptr(app, app->user_note_count), q, UN_MAX);
                    un_ptr(app, app->user_note_count)[UN_MAX] = 0;
                    app->user_note_count++;
                }
                if(!nl) break;
                q = nl + 1;
            }
            free(buf);
        }
    }
    storage_file_close(f);
    storage_file_free(f);
}
static void save_user_notes(SstvApp* app) {
    File* f = storage_file_alloc(app->storage);
    if(storage_file_open(f, NOTES_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        for(int i = 0; i < app->user_note_count; i++) {
            const char* str = un_ptr(app, i);
            storage_file_write(f, str, (uint16_t)strlen(str));
            storage_file_write(f, "\n", 1);
        }
    }
    storage_file_close(f);
    storage_file_free(f);
}
static int add_user_note(SstvApp* app, const char* text) {
    if(app->user_note_count >= MAX_USER_NOTES) return -1;
    int i = app->user_note_count++;
    strncpy(un_ptr(app, i), text, UN_MAX);
    un_ptr(app, i)[UN_MAX] = 0;
    return i;
}
static void delete_user_note(SstvApp* app, int idx) {
    if(idx < 0 || idx >= app->user_note_count) return;
    for(int i = idx; i + 1 < app->user_note_count; i++)
        memcpy(un_ptr(app, i), un_ptr(app, i + 1), UN_STRIDE);
    app->user_note_count--;
}

static void note_list_label(SstvApp* app, int idx, char* out, size_t cap) {
    const char* src = "New note...";
    if(idx == 1) src = "From SD card (.txt)...";
    else if(idx > 1 && idx <= app->user_note_count + 1) src = un_ptr(app, idx - 2);
    else if(idx > app->user_note_count + 1) src = SSTV_NOTE_PRESETS[idx - 2 - app->user_note_count];
    strncpy(out, src, cap - 1);
    out[cap - 1] = 0;
}

static void draw_note_list(Canvas* canvas, SstvApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Send note");
    canvas_set_font(canvas, FontSecondary);
    const int rows = 3, rh = 11; /* leave room for the bottom nav hints */
    int first = app->note_index - app->note_index % rows;
    for(int r = 0; r < rows; r++) {
        int idx = first + r;
        if(idx >= NOTE_LIST_LEN(app)) break;
        int y = 16 + r * rh; /* box top */
        char label[64];
        note_list_label(app, idx, label, sizeof label);
        note_font_begin(canvas);
        /* fit to width by dropping whole UTF-8 codepoints */
        while(canvas_string_width(canvas, label) > 112 && label[0]) {
            int n = (int)strlen(label), back = 1;
            while(back < n && ((uint8_t)label[n - back] & 0xC0) == 0x80) back++;
            label[n - back] = 0;
        }
        if(idx == app->note_index) {
            canvas_draw_rbox(canvas, 2, y, 124, rh, 2);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 6, y + 9, label);
        canvas_set_color(canvas, ColorBlack);
        note_font_end(canvas);
    }
    canvas_draw_icon(canvas, 2, 54, &I_Ok_btn_9x9);
    canvas_draw_str(canvas, 14, 62, app->note_index == 0 ? "type" : app->note_index == 1 ? "open" : "pick");
    canvas_draw_icon(canvas, 44, 53, &I_back_10px);
    canvas_draw_str(canvas, 56, 62, "back");
}

static void draw_note_preview(Canvas* canvas, SstvApp* app) {
    char cnt[24];
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Note");
    canvas_set_font(canvas, FontSecondary);
    snprintf(cnt, sizeof cnt, "%u/%d", (unsigned)strlen(app->note_text), SSTV_NOTE_MAX_CHARS);
    canvas_draw_str(canvas, 126 - canvas_string_width(canvas, cnt), 10, cnt);
    canvas_draw_frame(canvas, 1, 13, 126, 38);
    note_font_begin(canvas);
    int pfh = (int)canvas_current_font_height(canvas);
    int plh = pfh + 2, pml = (38 - 4) / plh;
    if(pml < 1) pml = 1;
    note_draw_wrapped(canvas, app->note_text, 4, 13 + pfh, 120, plh, pml);
    note_font_end(canvas);
    canvas_draw_icon(canvas, 2, 54, &I_Ok_btn_9x9);
    canvas_draw_str(canvas, 13, 62, "send");
    canvas_draw_str(canvas, 40, 62, "^edit");
    if(app->editing_user_idx >= 0) canvas_draw_str(canvas, 72, 62, "<del");
    canvas_draw_icon(canvas, 116, 53, &I_back_10px);
    if(app->note_del_confirm) {
        confirm_box(canvas, 24, 80, "Delete note?");
    }
}

static void fmt_time(char* buf, size_t cap, uint32_t s) {
    snprintf(buf, cap, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

/* Animation frames fully on the air after `frames_sent` physical frames: data positions sent ->
 * stream bytes -> frames whose coded data ended by then. */
static int anim_frames_sent(const SstvApp* app) {
    int chunks = (app->anim_len + SSTV_NOTE_CHUNK - 1) / SSTV_NOTE_CHUNK;
    int data_frames = SSTV_ILEAVE + (chunks + SSTV_ILEAVE - 1) / SSTV_ILEAVE * SSTV_ILEAVE;
    int pos = sstv_pass_data_before(app->frames_sent, data_frames, SSTV_NOTE_INTRO(data_frames));
    int bytes = (pos - SSTV_ILEAVE) * SSTV_NOTE_CHUNK;
    int n = 0;
    while(n < app->anim.n && app->anim.frame_end[n] <= bytes) n++;
    return n;
}

/* Status bar along the bottom: white under a black rule (it separates the bar from the
 * animation), a 3 px progress bar (1 px margins, corners cut for a rounded look, dotted track)
 * when `progress` >= 0, then three slots. A slot is an optional button glyph from the firmware's
 * set (OK, Back, the arrows), a text, and an optional glyph after it. */
#define ABAR_H 16 /* with progress: rule, gap, 3 px progress, gap, 9 px glyphs */
#define ABAR_H_PLAIN 12 /* without: rule, gap, glyphs */
typedef struct {
    const Icon* icon;
    const char* text;
    const Icon* icon2;
} BarSlot;
#define SLOT(i, t) ((BarSlot){(i), (t), NULL})

static int slot_width(Canvas* canvas, const BarSlot* sl) {
    int w = 0;
    if(sl->icon) w += icon_get_width(sl->icon) + (sl->text || sl->icon2 ? 2 : 0);
    if(sl->text) w += (int)canvas_string_width(canvas, sl->text) + (sl->icon2 ? 2 : 0);
    if(sl->icon2) w += icon_get_width(sl->icon2);
    return w;
}

/* Glyphs are centred on the digits and capitals of FontSecondary at baseline 62, which fill rows
 * 55..61 (checked on a screenshot from the device): a 7 px icon on 55..61, the 9 px OK on 54..62. */
#define BAR_TEXT_MID 58
static void slot_draw(Canvas* canvas, const BarSlot* sl, int x) {
    const int ty = 62; /* text baseline */
    if(sl->icon) {
        canvas_draw_icon(canvas, x, BAR_TEXT_MID - icon_get_height(sl->icon) / 2, sl->icon);
        x += icon_get_width(sl->icon) + 2;
    }
    if(sl->text) {
        canvas_draw_str(canvas, x, ty, sl->text);
        x += (int)canvas_string_width(canvas, sl->text) + 2;
    }
    if(sl->icon2) canvas_draw_icon(canvas, x, BAR_TEXT_MID - icon_get_height(sl->icon2) / 2, sl->icon2);
}

static void anim_bar(Canvas* canvas, BarSlot left, BarSlot mid, BarSlot right, float progress) {
    const int bh = progress >= 0.0f ? ABAR_H : ABAR_H_PLAIN, y = 64 - bh;
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 0, y, 128, bh);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_line(canvas, 0, y, 127, y);
    canvas_set_font(canvas, FontSecondary);
    if(progress >= 0.0f) {
        const int bx = 1, bw = 126, by = y + 2; /* rows by .. by+2 */
        for(int x = bx + 1; x < bx + bw - 1; x += 2) canvas_draw_dot(canvas, x, by + 1); /* track */
        int fill = (int)(progress * (float)bw + 0.5f);
        if(fill > bw) fill = bw;
        if(fill >= 3) {
            canvas_draw_line(canvas, bx + 1, by, bx + fill - 2, by);
            canvas_draw_line(canvas, bx, by + 1, bx + fill - 1, by + 1);
            canvas_draw_line(canvas, bx + 1, by + 2, bx + fill - 2, by + 2);
        } else if(fill > 0) {
            canvas_draw_line(canvas, bx, by + 1, bx + fill - 1, by + 1);
        }
    }
    bool has_l = left.icon || left.text || left.icon2, has_r = right.icon || right.text || right.icon2;
    int wl = has_l ? slot_width(canvas, &left) : 0, wr = has_r ? slot_width(canvas, &right) : 0;
    if(has_l) slot_draw(canvas, &left, 2);
    if(has_r) slot_draw(canvas, &right, 126 - wr);
    if(mid.icon || mid.text) {
        /* the middle goes in the gap between the two sides (centred on the screen when it fits
         * there); its text is cut with ".." when it does not fit */
        int lo = 2 + wl + (has_l ? 5 : 0), hi = 126 - wr - (has_r ? 5 : 0);
        char t[40] = "";
        if(mid.text) {
            strncpy(t, mid.text, sizeof t - 1);
            t[sizeof t - 1] = 0;
        }
        BarSlot m = {mid.icon, mid.text ? t : NULL, mid.icon2};
        int w = slot_width(canvas, &m);
        size_t n = strlen(t);
        while(mid.text && w > hi - lo && n > 1) {
            t[--n] = 0;
            char c[44];
            snprintf(c, sizeof c, "%s..", t);
            BarSlot mc = {mid.icon, c, mid.icon2};
            if(slot_width(canvas, &mc) <= hi - lo) {
                strcpy(t, c);
                w = slot_width(canvas, &m);
                break;
            }
            w = slot_width(canvas, &mc);
        }
        int x = 64 - w / 2;
        if(x < lo) x = lo;
        if(x + w > hi) x = hi - w;
        slot_draw(canvas, &m, x);
    }
}

/* The animation at 1:1 (above the bar when it is short enough, else from the top with the bar
 * over its last rows) and one status bar: preview, sending, paused, done. */
static void draw_anim(Canvas* canvas, SstvApp* app) {
    const sstv_anim_info* a = &app->anim;
    int bh = app->state == StateAnim ? ABAR_H_PLAIN : ABAR_H; /* the preview has no progress bar */
    int ay = a->h <= 64 - bh ? (64 - bh - a->h) / 2 : 0;
    if(app->anim_frame) canvas_draw_xbm(canvas, (128 - a->w) / 2, ay, (size_t)a->w, (size_t)a->h, app->anim_frame);
    char l[24], m[24], r[24];
    uint32_t phys_ms = SSTV_BAND_PHYS_US(sstv_tx_band()) / 1000;
    if(app->state == StateAnim) {
        uint32_t secs = ((uint32_t)sstv_blob_frames(app->anim_len) * phys_ms + SSTV_BAND_PREAMBLE_US(sstv_tx_band()) / 1000) / 1000;
        fmt_time(r, sizeof r, secs);
        BarSlot clock = {&I_clock_7x7, r, NULL};
        if(app->anim_pick >= 0) { /* Left sends just the picked frame */
            snprintf(m, sizeof m, "frame %d", app->anim_pick);
            anim_bar(canvas, SLOT(&I_Ok_btn_9x9, "send"), SLOT(&I_ButtonLeft_4x7, m), clock, -1.0f);
        } else {
            snprintf(m, sizeof m, "%d", a->n);
            BarSlot frames = {NULL, m, &I_frames_7x7};
            anim_bar(canvas, SLOT(&I_Ok_btn_9x9, "send"), frames, clock, -1.0f);
        }
        return;
    }
    if(app->state == StateDone) {
        char tm[12];
        fmt_time(tm, sizeof tm, (uint32_t)app->frames_total * phys_ms / 1000);
        snprintf(m, sizeof m, "%d", a->n);
        BarSlot frames = {NULL, m, &I_frames_7x7};
        anim_bar(canvas, SLOT(&I_clock_7x7, tm), frames, SLOT(&I_Ok_btn_9x9, "again"), 1.0f);
        return;
    }
    /* transmitting */
    bool paused = sstv_tx_is_paused();
    if(paused) app->pausing = false;
    float p = app->frames_total ? (float)app->frames_sent / (float)app->frames_total : 0.0f;
    int pct = (int)(p * 100.0f);
    /* the frame on the air now, counted from 1 like the receiver's (which shows the frame it is
     * drawing): frames already out + 1, all of them once the data is out */
    int on_air = anim_frames_sent(app) + 1;
    if(on_air > a->n) on_air = a->n;
    snprintf(m, sizeof m, "%d/%d", on_air, a->n);
    BarSlot frames = {NULL, m, &I_frames_7x7};
    if(paused) {
        snprintf(l, sizeof l, "|| %d%%", pct);
        anim_bar(canvas, SLOT(NULL, l), SLOT(&I_Ok_btn_9x9, "go"), SLOT(&I_back_10px, "stop"), p);
    } else if(app->pausing) {
        anim_bar(canvas, SLOT(NULL, "Pausing..."), SLOT(NULL, NULL), SLOT(NULL, NULL), p);
    } else {
        char tm[12];
        fmt_time(tm, sizeof tm, (uint32_t)(app->frames_total - app->frames_sent) * phys_ms / 1000);
        snprintf(l, sizeof l, "%d%%", pct);
        snprintf(r, sizeof r, "%s", tm);
        anim_bar(canvas, SLOT(NULL, l), frames, SLOT(&I_clock_7x7, r), p);
    }
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
    if(app->state == StateOptions) {
        static const char* items[OPT_COUNT] = {"Calibrate", "About"};
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "Options");
        canvas_set_font(canvas, FontSecondary);
        for(int i = 0; i < OPT_COUNT; i++) {
            int y = 14 + i * 12;
            if(i == app->opt_index) {
                canvas_draw_rbox(canvas, 1, y, 126, 11, 2);
                canvas_set_color(canvas, ColorWhite);
            }
            canvas_draw_str(canvas, 5, y + 9, items[i]);
            canvas_set_color(canvas, ColorBlack);
        }
        furi_mutex_release(app->mutex);
        return;
    }
    if(app->state == StateAbout) {
        canvas_set_font(canvas, FontSecondary);
        /* the whole screen: name, version and build on the left, the QR code on the right with
         * "github.com" over it (a scanner needs white around the code: one touching the bezel did not
         * decode, 4 px on the right, 1 px below and the caption 4 px above do) */
        char t[24];
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, "SFTV TX");
        canvas_set_font(canvas, FontSecondary);
        snprintf(t, sizeof t, "version %s", SFTV_VERSION);
        canvas_draw_str(canvas, 2, 24, t);
        snprintf(t, sizeof t, "build %d", SFTV_BUILD);
        canvas_draw_str(canvas, 2, 33, t);
        /* authorship, bottom left: the font has no (c) sign, so it is a 7 px glyph */
        canvas_draw_icon(canvas, 2, 55, &I_copyright_7x7);
        canvas_draw_str(canvas, 12, 62, "2026 negenii");
        canvas_draw_icon(canvas, 74, 13, &I_qr_github);
        canvas_draw_str(canvas, 74 + (50 - (int)canvas_string_width(canvas, "github.com")) / 2, 9, "github.com"); /* caption over the code */
        furi_mutex_release(app->mutex);
        return;
    }
    if(app->state == StateAnimList) {
        char l[16], m[32];
        if(app->al_count == 0 || app->anim.n == 0 || !app->anim_frame) {
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, 2, 12, app->al_count ? "Can't read it" : "No animations");
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, 2, 26, app->al_count ? app->anim_dir + 5 : "/ext/dolphin is empty");
            anim_bar(canvas, SLOT(NULL, NULL), SLOT(NULL, NULL), SLOT(&I_back_10px, "menu"), -1.0f);
        } else {
            const sstv_anim_info* a = &app->anim;
            int ay = a->h <= 64 - ABAR_H_PLAIN ? (64 - ABAR_H_PLAIN - a->h) / 2 : 0;
            canvas_draw_xbm(canvas, (128 - a->w) / 2, ay, (size_t)a->w, (size_t)a->h, app->anim_frame);
            const char* nm = strrchr(app->anim_dir, '/');
            nm = nm ? nm + 1 : app->anim_dir;
            snprintf(m, sizeof m, "%.*s", 17, nm);
            char* us = strrchr(m, '_'); /* drop the _128x64 suffix */
            if(us && us != m && us[1] >= '0' && us[1] <= '9' && strchr(us, 'x')) *us = 0;
            if(app->al_sent) { /* already sent once: a "Sent" pill in the top-right corner */
                const char* t = "Sent";
                int w = (int)canvas_string_width(canvas, t) + 8;
                canvas_set_color(canvas, ColorWhite);
                canvas_draw_rbox(canvas, 127 - w - 1, 0, w + 2, 13, 4); /* white halo over the animation */
                canvas_set_color(canvas, ColorBlack);
                canvas_draw_rbox(canvas, 127 - w, 1, w, 11, 3);
                canvas_set_color(canvas, ColorWhite);
                canvas_draw_str(canvas, 127 - w + 4, 10, t);
                canvas_set_color(canvas, ColorBlack);
            }
            snprintf(l, sizeof l, "%d/%d", app->al_index + 1, app->al_count);
            BarSlot nav = {&I_ButtonLeft_4x7, l, &I_ButtonRight_4x7};
            anim_bar(canvas, nav, SLOT(NULL, m), SLOT(&I_Ok_btn_9x9, "pick"), -1.0f);
        }
        furi_mutex_release(app->mutex);
        return;
    }
    if(app->state == StateAnim || ((app->state == StateTransmit || app->state == StateDone) && app->is_anim)) {
        draw_anim(canvas, app);
        furi_mutex_release(app->mutex);
        return;
    }
    if(app->state == StateConvert && app->conv_anim) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, app->conv_err[0] ? "Can't send this animation" : "Preparing to send");
        canvas_set_font(canvas, FontSecondary);
        if(app->conv_err[0]) {
            canvas_draw_str(canvas, 2, 28, app->conv_err);
            canvas_draw_str(canvas, 2, 50, "Any key: back");
        } else {
            char t[32];
            snprintf(t, sizeof t, "%d frames %dx%d, %d fps", app->anim.n, app->anim.w, app->anim.h, app->anim.fps);
            canvas_draw_str(canvas, 2, 28, t);
            elements_progress_bar(canvas, 2, 36, 124, (float)app->conv_pct / 100.0f);
        }
        furi_mutex_release(app->mutex);
        return;
    }
    if(app->state == StateConvert) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, app->conv_err[0] ? "Can't use this file" : "Converting...");
        canvas_set_font(canvas, FontSecondary);
        if(app->conv_err[0]) {
            canvas_draw_str(canvas, 2, 28, app->conv_err);
            canvas_draw_str(canvas, 2, 40, "JPEG (not progressive), PNG,");
            canvas_draw_str(canvas, 2, 50, "BMP or PBM. Any key: back");
        } else {
            canvas_draw_str(canvas, 2, 28, "to 400x240, dithered");
            elements_progress_bar(canvas, 2, 36, 124, (float)app->conv_pct / 100.0f);
        }
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
        note_font_begin(canvas);
        int tfh = (int)canvas_current_font_height(canvas);
        int tlh = tfh + 2, tml = (50 - 4) / tlh;
        if(tml < 1) tml = 1;
        note_draw_wrapped(canvas, app->note_text, 4, 2 + tfh, PANEL_X - 11, tlh, tml);
        note_font_end(canvas);
        draw_panel(canvas, app);
        furi_mutex_release(app->mutex);
        return;
    }
    if(app->state == StateTest && app->calib) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "Calibrating");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 20, "A test signal is looping.");
        canvas_draw_line(canvas, 0, 23, 127, 23);
        draw_wrapped(canvas, "Move the Flipper around the receiver's mic until its screen shows quality 80% or more.", 2, 33, 124, 9, 0, 3);
        canvas_draw_icon(canvas, 2, 55, &I_back_10px);
        canvas_draw_str(canvas, 14, 63, "stop");
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

static bool read_anim_meta(Storage* storage, const char* dir, sstv_anim_info* a);
static bool load_anim_frame(Storage* storage, const char* dir, int i, int fb, CompressIcon* ci, uint8_t* out);

/* An animation shortcut (<name>.anim in the data folder: the path of the animation's folder, one
 * line), written when the animation is first sent so it can be sent again from the grid. */
static bool read_shortcut(Storage* storage, const char* path, char* dir, size_t cap) {
    File* f = storage_file_alloc(storage);
    size_t n = 0;
    if(storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) n = storage_file_read(f, dir, cap - 1);
    storage_file_close(f);
    storage_file_free(f);
    dir[n] = 0;
    while(n > 0 && (dir[n - 1] == '\n' || dir[n - 1] == '\r' || dir[n - 1] == ' ')) dir[--n] = 0;
    return n > 0;
}

/* 40x20 tile of an animation's first frame (nearest pixel), grey when unreadable. */
static void anim_thumb(Storage* storage, const char* dir, uint8_t* tile) {
    memset(tile, 0, SSTV_TINY_BYTES);
    sstv_anim_info* a = malloc(sizeof(sstv_anim_info));
    bool ok = read_anim_meta(storage, dir, a);
    if(ok) {
        int fb = sstv_anim_frame_bytes(a);
        CompressIcon* ci = compress_icon_alloc((size_t)fb + 64);
        uint8_t* fr = malloc((size_t)fb);
        ok = load_anim_frame(storage, dir, a->order[0], fb, ci, fr);
        if(ok) {
            int stride = sstv_anim_stride(a->w);
            for(int y = 0; y < SSTV_TINY_H; y++)
                for(int x = 0; x < SSTV_TINY_W; x++) {
                    int sx = x * a->w / SSTV_TINY_W, sy = y * a->h / SSTV_TINY_H;
                    if((fr[sy * stride + (sx >> 3)] >> (sx & 7)) & 1) tile[y * SSTV_TINY_LINE_BYTES + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
                }
        }
        free(fr);
        compress_icon_free(ci);
    }
    free(a);
    if(!ok) memset(tile, 0x55, SSTV_TINY_BYTES);
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
            /* the picture grid lists .pbm files, the animation grid the .anim shortcuts */
            if(n < 5 || strcasecmp(name + n - 4, ".pbm") != 0) continue; /* pictures only; .anim marks sent animations */
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
    if(app->file_index >= app->file_count) app->file_index = 0;
    app->thumbs_page = -1;
}

/* Tiles for the grid page under the cursor (3 files read, not the whole folder: the folder
 * outgrew the 64 tiles kept in RAM and new pictures went missing from the grid). Uses
 * app->image as the read buffer, so not while an animation holds that memory. */
static void ensure_thumbs(SstvApp* app) {
    int page = app->browse_sel / TILES_PER_PAGE * TILES_PER_PAGE;
    if(page == app->thumbs_page || !app->image) return;
    FuriString* path = furi_string_alloc();
    for(int k = 0; k < TILES_PER_PAGE; k++) {
        int idx = page + k - 1; /* grid index 0 is the SD card tile */
        uint8_t* t = app->thumbs[k];
        memset(t, 0x55, SSTV_TINY_BYTES); /* unreadable: grey tile */
        if(idx < 0 || idx >= app->file_count) continue;
        furi_string_printf(path, "%s/%s", SSTV_DATA_DIR, app->files[idx]);
        if(is_anim_file(app->files[idx])) {
            char dir[128];
            if(read_shortcut(app->storage, furi_string_get_cstr(path), dir, sizeof dir)) anim_thumb(app->storage, dir, t);
        } else if(load_pbm(app->storage, furi_string_get_cstr(path), app->image)) {
            sstv_preview_make_tiny(app->image, t);
        }
    }
    furi_string_free(path);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->thumbs_page = page;
    furi_mutex_release(app->mutex);
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

static void notes_restore(SstvApp* app);

static void set_state(SstvApp* app, AppState s) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->state = s;
    if(s == StatePreview) {
        app->frames_sent = 0;
        app->lines_done = 0;
    }
    furi_mutex_release(app->mutex);
    /* the saved notes come back once the screen leaves the transmission (see notes_release);
     * an animation brings them back itself when it closes */
    if(s != StateTransmit && s != StateDone && !app->is_anim) notes_restore(app);
    if(s == StateBrowse) ensure_thumbs(app);
    bool tx = (s == StateTransmit || s == StateTest);
    if(tx) {
        led_start(app);
    } else {
        led_stop(app);
    }
    /* keep the screen lit for the whole transmission, back to the user's timeout after */
    if(tx != app->backlight_held) {
        notification_message(
            app->notif,
            tx ? &sequence_display_backlight_enforce_on : &sequence_display_backlight_enforce_auto);
        app->backlight_held = tx;
    }
    if(s != StateMenu) {
        icon_animation_stop(app->scan_anim);
        if(app->note_anim) icon_animation_stop(app->note_anim);
        if(app->film_anim) icon_animation_stop(app->film_anim);
        if(app->opts_anim) icon_animation_stop(app->opts_anim);
        app->menu_anim = -1;
    }
    view_port_update(app->vp);
}

/* Start-of-pass call: C6 G6 on the buzzer right before the preamble (audible, far below the
 * 17-21 kHz band); the Playdate answers C7 E7 when it locks on (see play_start_chime there). */
static void chime_start(void) {
    if(!furi_hal_speaker_acquire(100)) return;
    furi_hal_speaker_start(1046.5f, 1.0f); /* C6 */
    furi_delay_ms(150);
    furi_hal_speaker_start(1568.0f, 1.0f); /* G6 */
    furi_delay_ms(150);
    furi_hal_speaker_stop();
    furi_hal_speaker_release();
}

/* The saved notes (16 KB) are not needed while a prebuilt pass (~28 KB) is on the air, and after a
 * picture conversion the heap is fragmented enough that the pass did not fit (the app crashed:
 * malloc does not fail on the Flipper). Released for the transmission, read back from the card
 * when the screen leaves Transmit/Done (set_state). */
static void notes_release(SstvApp* app) {
    free(app->user_notes);
    app->user_notes = NULL;
}

static void notes_restore(SstvApp* app) {
    if(app->user_notes) return;
    app->user_notes = malloc((size_t)MAX_USER_NOTES * UN_STRIDE);
    load_user_notes(app);
}

static void tx_failed(SstvApp* app) {
    FURI_LOG_E(TAG, "tx start failed: speaker busy or no memory (max block %u, free %u)", (unsigned)memmgr_heap_get_max_free_block(),
               (unsigned)memmgr_get_free_heap());
    notes_restore(app);
    notification_message(app->notif, &sequence_error);
}

static void start_tx(SstvApp* app) {
    chime_start();
    notes_release(app);
    if(sstv_tx_start(app->image, app->name, false, tx_callback, app)) {
        app->frames_sent = 0; /* a resend from Done must not show the last pass's count */
        app->frames_total = (uint16_t)sstv_tx_total_frames();
        app->coded = sstv_tx_is_coded();
        app->is_note = false;
        app->lines_done = 0;
        set_state(app, StateTransmit);
    } else {
        tx_failed(app);
    }
}

/* First line of the note -> ASCII slug (Cyrillic transliterated), so the same note always
 * gets the same name and overwrites on the receiver. */
static void note_translit(const char* in, char* out, size_t cap) {
    static const char* const cyr[32] = {
        "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
        "r", "s", "t", "u", "f", "h", "ts", "ch", "sh", "sch", "", "y", "", "e", "yu", "ya"};
    size_t n = 0;
    const uint8_t* p = (const uint8_t*)in;
    while(*p && *p != '\n' && n + 1 < cap) {
        uint32_t cp;
        if(*p < 0x80) {
            cp = *p;
            p += 1;
        } else if((*p & 0xE0) == 0xC0) {
            cp = (uint32_t)((*p & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if((*p & 0xF0) == 0xE0) {
            cp = (uint32_t)((*p & 0x0F) << 12) | (uint32_t)((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else {
            p += 1;
            continue;
        }
        char one[2] = {0, 0};
        const char* s = "";
        if(cp < 0x80) {
            one[0] = (char)cp;
            s = one;
        } else if(cp >= 0x430 && cp <= 0x44F) {
            s = cyr[cp - 0x430];
        } else if(cp >= 0x410 && cp <= 0x42F) {
            s = cyr[cp - 0x410];
        } else if(cp == 0x451 || cp == 0x401) {
            s = "e";
        }
        for(; *s && n + 1 < cap; s++) out[n++] = *s;
    }
    out[n] = 0;
}

static void start_tx_note(SstvApp* app) {
    chime_start();
    note_translit(app->note_text, app->note_name, SSTV_NOTE_NAME_MAX + 1);
    if(app->note_name[0] == 0) strcpy(app->note_name, "Note"); /* no name on the wire: the receiver numbers notes note_N (Latin) */
    notes_release(app);
    if(sstv_tx_start_note(app->note_text, app->note_name, tx_callback, app)) {
        app->frames_sent = 0; /* a resend from Done must not show the last pass's count */
        app->frames_total = (uint16_t)sstv_tx_total_frames();
        app->coded = false;
        app->is_note = true;
        app->lines_done = 0;
        set_state(app, StateTransmit);
    } else {
        tx_failed(app);
    }
}

/* Note keyboard on a temporary ViewDispatcher (the app itself runs a raw ViewPort, so the
 * standard text_input view is hosted only for the duration of the edit). Returns true if the
 * user confirmed. note_text is edited in place (prefilled for a preset). */
static bool g_note_kbd_ok;
static TextInput* g_note_ti;
static char g_note_hdr[40];
/* Live character counter in the keyboard header (the module writes each keystroke straight
 * into our buffer, so strlen is current). */
static void note_kbd_tick(void* ctx) {
    SstvApp* app = ctx;
    snprintf(g_note_hdr, sizeof g_note_hdr, "Type a note  %u/%d", (unsigned)strlen(app->note_text), SSTV_NOTE_MAX_CHARS);
    if(g_note_ti) text_input_set_header_text(g_note_ti, g_note_hdr);
}
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
    g_note_ti = ti;
    text_input_set_result_callback(
        ti, note_kbd_result, app, app->note_text, SSTV_NOTE_MAX_CHARS + 1, false);
    view_dispatcher_set_event_callback_context(vd, app);
    view_dispatcher_set_navigation_event_callback(vd, note_kbd_back);
    view_dispatcher_set_tick_event_callback(vd, note_kbd_tick, 150);
    note_kbd_tick(app); /* header shows the counter from the start */
    view_dispatcher_add_view(vd, 0, text_input_get_view(ti));
    view_dispatcher_attach_to_gui(vd, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(vd, 0);
    view_dispatcher_run(vd);
    view_dispatcher_remove_view(vd, 0);
    g_note_ti = NULL;
    text_input_free(ti);
    view_dispatcher_free(vd);
    app->kbd_vd = NULL;
    gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);
    return g_note_kbd_ok;
}

/* ---- any picture from the SD card (flipper/lib/imgdec: JPEG, PNG, BMP, PBM) ---- */
static int io_read(void* ctx, uint8_t* buf, int n) {
    return (int)storage_file_read((File*)ctx, buf, (size_t)n);
}
static bool io_seek(void* ctx, uint32_t off) {
    return storage_file_seek((File*)ctx, off, true);
}
static void io_progress(void* ctx, int pct) {
    SstvApp* app = ctx;
    if(pct != app->conv_pct && (pct - app->conv_pct >= 2 || pct == 100)) {
        app->conv_pct = pct;
        view_port_update(app->vp);
    }
}

static bool save_pbm(Storage* storage, const char* path, const uint8_t* image) {
    File* f = storage_file_alloc(storage);
    bool ok = storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) {
        const char* hdr = "P4\n400 240\n";
        ok = storage_file_write(f, hdr, strlen(hdr)) == strlen(hdr) &&
             storage_file_write(f, image, SSTV_IMAGE_BYTES) == SSTV_IMAGE_BYTES;
    }
    storage_file_close(f);
    storage_file_free(f);
    return ok;
}

/* A Flipper animation frame (/ext/dolphin/<anim>/frame_N.bm, asset packs too): icon-format data
 * (raw, or heatshrink with a header), decoded by the firmware; the size comes from the
 * animation's meta.txt ("Width: 128", "Height: 64"), else from a "_128x64" folder suffix. */
static bool load_bm(SstvApp* app, const char* path, char* err, size_t en) {
    int w = 0, h = 0;
    FuriString* dir = furi_string_alloc_set(path);
    size_t cut = furi_string_search_rchar(dir, '/', 0);
    if(cut != FURI_STRING_FAILURE) furi_string_left(dir, cut);
    FuriString* meta = furi_string_alloc_printf("%s/meta.txt", furi_string_get_cstr(dir));
    File* f = storage_file_alloc(app->storage);
    if(storage_file_open(f, furi_string_get_cstr(meta), FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[257];
        size_t n = storage_file_read(f, buf, sizeof buf - 1);
        buf[n] = 0;
        const char* p = strstr(buf, "Width:");
        if(p) w = atoi(p + 6);
        p = strstr(buf, "Height:");
        if(p) h = atoi(p + 7);
    }
    storage_file_close(f);
    if(w <= 0 || h <= 0) { /* folder name like L1_3d_printing_128x64 */
        const char* d = furi_string_get_cstr(dir);
        const char* us = strrchr(d, '_');
        if(us && sscanf(us + 1, "%dx%d", &w, &h) != 2) w = h = 0;
    }
    furi_string_free(meta);
    furi_string_free(dir);
    if(w <= 0 || h <= 0 || w > 400 || h > 240) { strncpy(err, "no frame size (meta.txt)", en - 1); storage_file_free(f); return false; }
    bool ok = storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING);
    size_t size = ok ? (size_t)storage_file_size(f) : 0;
    uint8_t* data = ok && size > 0 && size < 16384 ? malloc(size) : NULL;
    ok = data && storage_file_read(f, data, size) == size;
    storage_file_close(f);
    storage_file_free(f);
    if(!ok) { free(data); strncpy(err, "can't read the frame", en - 1); return false; }
    size_t stride = ((size_t)w + 7) / 8, raw = stride * (size_t)h;
    CompressIcon* ci = compress_icon_alloc(raw + 64);
    uint8_t* bits = NULL;
    compress_icon_decode(ci, data, &bits);
    ok = bits && img_from_bits(bits, w, h, (int)stride, true, app->image);
    compress_icon_free(ci);
    free(data);
    if(!ok) strncpy(err, "frame decode failed", en - 1);
    return ok;
}

/* File browser over the whole card; the pick is converted, saved next to the other pictures
 * (so it shows up in the grid next time) and opened on the preview screen. */
/* Converts one picture file (any format, or a single animation frame) for the preview. */
static void convert_still(SstvApp* app, const char* path) {
    app->conv_anim = false;
    app->conv_err[0] = 0;
    app->conv_pct = 0;
    set_state(app, StateConvert);
    File* f = storage_file_alloc(app->storage);
    bool ok = storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING);
    size_t plen = strlen(path);
    bool is_bm = plen > 3 && strcasecmp(path + plen - 3, ".bm") == 0;
    if(ok && is_bm) {
        storage_file_close(f);
        ok = load_bm(app, path, app->conv_err, sizeof app->conv_err);
    } else if(ok) {
        ImgIo io = {f, io_read, io_seek, (uint32_t)storage_file_size(f), io_progress, app};
        ok = img_load(&io, path, app->image, app->conv_err, sizeof app->conv_err);
    } else {
        strncpy(app->conv_err, "can't open the file", sizeof app->conv_err - 1);
    }
    storage_file_close(f);
    storage_file_free(f);
    if(!ok) {
        if(!app->conv_err[0]) strncpy(app->conv_err, "conversion failed", sizeof app->conv_err - 1);
        view_port_update(app->vp);
        return; /* the error stays on screen until a key */
    }
    /* name: the file's base name, safe characters only, short enough for the meta block; an
     * animation frame is "<animation>_f<N>" (L1_3d_printing_128x64/frame_7.bm -> L1_3d_printing_f7) */
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char bmname[64];
    if(is_bm) {
        char folder[48] = "";
        const char* end = base - 1; /* the '/' before the file */
        const char* st = end;
        while(st > path && *(st - 1) != '/') st--;
        size_t fl = (size_t)(end - st) < sizeof folder - 1 ? (size_t)(end - st) : sizeof folder - 1;
        memcpy(folder, st, fl);
        folder[fl] = 0;
        char* us = strrchr(folder, '_');
        if(us && strchr(us, 'x')) *us = 0; /* drop the _128x64 suffix */
        const char* num = base;
        while(*num && (*num < '0' || *num > '9')) num++;
        snprintf(bmname, sizeof bmname, "%.14s_f%.4s", folder, num);
        char* dot = strrchr(bmname, '.');
        if(dot) *dot = 0;
        base = bmname;
    }
    char nm[SSTV_NAME_MAX + 1];
    size_t n = 0;
    for(const char* q = base; *q && *q != '.' && n < SSTV_NAME_MAX - 4; q++) {
        char c = *q;
        nm[n++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '_';
    }
    if(n == 0) nm[n++] = 'x';
    nm[n] = 0;
    FuriString* out = furi_string_alloc_printf("%s/%s.pbm", SSTV_DATA_DIR, nm);
    bool saved = save_pbm(app->storage, furi_string_get_cstr(out), app->image);
    furi_string_free(out);
    if(saved) {
        /* rescan (it reuses app->image for the thumbnails), then open the new file like any other */
        char fname[40];
        snprintf(fname, sizeof fname, "%s.pbm", nm);
        scan_files(app);
        for(int i = 0; i < app->file_count; i++)
            if(strcmp(app->files[i], fname) == 0) {
                app->file_index = i;
                app->browse_sel = i + 1;
            }
        if(open_file(app)) set_state(app, StatePreview);
        else set_state(app, StateBrowse);
    } else {
        /* card full or read-only: send it anyway from memory */
        sstv_preview_make(app->image, app->preview);
        strncpy(app->name, nm, sizeof(app->name) - 1);
        app->name[sizeof(app->name) - 1] = 0;
        set_state(app, StatePreview);
    }
}

/* ---- whole animations: /ext/dolphin/<name>/ (or an asset pack's Anims/<name>/) holds
 * frame_N.bm and meta.txt. Picking any frame (or meta.txt) packs the whole animation with the
 * sstv_anim codec and plays it; OK sends it. ---- */
#define ANIM_TX_MAX 24576 /* ~6 min on the air; the TX frame buffer for it takes ~45 KB */

static int meta_int(const char* buf, const char* key) {
    const char* p = strstr(buf, key);
    return p ? atoi(p + strlen(key)) : -1;
}

/* Parses <dir>/meta.txt (size, frame rate, play order). */
static bool read_anim_meta(Storage* storage, const char* dir, sstv_anim_info* a) {
    memset(a, 0, sizeof *a);
    FuriString* mp = furi_string_alloc_printf("%s/meta.txt", dir);
    File* f = storage_file_alloc(storage);
    char* buf = malloc(2048);
    size_t n = 0;
    if(storage_file_open(f, furi_string_get_cstr(mp), FSAM_READ, FSOM_OPEN_EXISTING)) n = storage_file_read(f, buf, 2047);
    buf[n] = 0;
    storage_file_close(f);
    storage_file_free(f);
    furi_string_free(mp);
    bool ok = n > 0 && strstr(buf, "Flipper Animation");
    if(ok) {
        a->w = meta_int(buf, "Width:");
        a->h = meta_int(buf, "Height:");
        a->passive = meta_int(buf, "Passive frames:");
        a->active = meta_int(buf, "Active frames:");
        a->fps = meta_int(buf, "Frame rate:");
        const char* p = strstr(buf, "Frames order:");
        int maxf = -1;
        if(p) {
            p += 13;
            while(*p && *p != '\n' && a->order_len < SSTV_ANIM_MAX_ORDER) {
                while(*p == ' ' || *p == '\t' || *p == '\r') p++;
                if(*p < '0' || *p > '9') break;
                int v = atoi(p);
                while(*p >= '0' && *p <= '9') p++;
                if(v >= SSTV_ANIM_MAX_FRAMES) { maxf = SSTV_ANIM_MAX_FRAMES; break; }
                a->order[a->order_len++] = (uint8_t)v;
                if(v > maxf) maxf = v;
            }
        }
        a->n = maxf + 1;
        if(a->passive < 0) a->passive = a->order_len;
        if(a->active < 0) a->active = 0;
        if(a->fps <= 0) a->fps = 2;
        if(a->fps > 30) a->fps = 30;
        ok = a->w > 0 && a->w <= 128 && a->h > 0 && a->h <= 64 && a->order_len > 0 && a->n > 0 &&
             a->n <= SSTV_ANIM_MAX_FRAMES;
    }
    free(buf);
    return ok;
}

/* <dir>/frame_<i>.bm -> out (XBM, fb bytes), decoded with `ci` (fb + 64 bytes). */
static bool load_anim_frame(Storage* storage, const char* dir, int i, int fb, CompressIcon* ci, uint8_t* out) {
    FuriString* fp = furi_string_alloc_printf("%s/frame_%d.bm", dir, i);
    File* f = storage_file_alloc(storage);
    bool ok = storage_file_open(f, furi_string_get_cstr(fp), FSAM_READ, FSOM_OPEN_EXISTING);
    size_t size = ok ? (size_t)storage_file_size(f) : 0;
    uint8_t* data = ok && size > 0 && size <= (size_t)fb + 64 ? malloc(size) : NULL;
    ok = data && storage_file_read(f, data, size) == size;
    storage_file_close(f);
    storage_file_free(f);
    furi_string_free(fp);
    if(ok) {
        uint8_t* bits = NULL;
        compress_icon_decode(ci, data, &bits);
        ok = bits != NULL;
        if(ok) memcpy(out, bits, (size_t)fb);
    }
    free(data);
    return ok;
}

static bool anim_read_meta(SstvApp* app) {
    return read_anim_meta(app->storage, app->anim_dir, &app->anim);
}

static bool anim_load_frame(SstvApp* app, int i, uint8_t* out) {
    return load_anim_frame(app->storage, app->anim_dir, i, sstv_anim_frame_bytes(&app->anim), app->anim_ci, out);
}

static bool anim_get(void* ctx, int i, uint8_t* bits) {
    SstvApp* app = ctx;
    bool ok = anim_load_frame(app, i, bits);
    if(!ok) app->anim_read_fail = true;
    return ok;
}

static void anim_progress(void* ctx, int done) {
    SstvApp* app = ctx;
    io_progress(app, done * 100 / app->anim.n);
}

static void anim_timer_cb(void* ctx) {
    SstvApp* app = ctx;
    AppEvent ev = {.type = EvAnimTick};
    furi_message_queue_put(app->queue, &ev, 0);
}

/* The app keeps ~28 KB it does not need while an animation is up (the saved notes and the
 * 400x240 picture); with the code loaded only ~44 KB of heap is left, too little to pack an
 * animation and hold its transmission. Released on entry, restored on the way out. */
static void anim_release_mem(SstvApp* app) {
    free(app->user_notes);
    app->user_notes = NULL;
    free(app->image);
    app->image = NULL;
}

static void anim_restore_mem(SstvApp* app) {
    if(!app->image) app->image = malloc(SSTV_IMAGE_BYTES);
    if(!app->user_notes) {
        app->user_notes = malloc((size_t)MAX_USER_NOTES * UN_STRIDE);
        load_user_notes(app);
    }
}

/* The name an animation goes out under (and its sent-marker shortcut <name>.anim): the folder
 * without its _128x64 suffix, safe characters only, at most SSTV_NOTE_NAME_MAX. */
static void anim_name(const char* dir, char* out) {
    const char* st = strrchr(dir, '/');
    st = st ? st + 1 : dir;
    size_t n = 0;
    for(const char* q = st; *q && n < SSTV_NOTE_NAME_MAX; q++) {
        char c = *q;
        out[n++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '_';
    }
    out[n] = 0;
    char* us = strrchr(out, '_');
    if(us && us != out && strchr(us, 'x') && us[1] >= '0' && us[1] <= '9') *us = 0;
}

static void anim_close(SstvApp* app) {
    if(app->anim_timer) furi_timer_stop(app->anim_timer);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->is_anim = false;
    free(app->anim_stream);
    app->anim_stream = NULL;
    free(app->anim_frame);
    app->anim_frame = NULL;
    furi_mutex_release(app->mutex);
    if(app->anim_ci) compress_icon_free(app->anim_ci);
    app->anim_ci = NULL;
    anim_restore_mem(app);
}

/* Next frame of the play order onto the screen. */
static void anim_tick(SstvApp* app) {
    if((!app->is_anim && app->state != StateAnimList) || !app->anim_frame) return;
    int fb = sstv_anim_frame_bytes(&app->anim);
    uint8_t* tmp = malloc((size_t)fb);
    app->anim_pos = (app->anim_pos + 1) % app->anim.order_len;
    if(anim_load_frame(app, app->anim.order[app->anim_pos], tmp)) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        memcpy(app->anim_frame, tmp, (size_t)fb);
        furi_mutex_release(app->mutex);
        view_port_update(app->vp);
    }
    free(tmp);
}

/* path is a frame_N.bm or the meta.txt of an animation folder: packs the whole animation and
 * opens StateAnim. False when it is not an animation (the caller converts the file as a picture). */
static bool anim_open(SstvApp* app, const char* path) {
    const char* base = strrchr(path, '/');
    if(!base) return false;
    base++;
    bool is_meta = strcmp(base, "meta.txt") == 0;
    size_t bl = strlen(base);
    bool is_frame = strncmp(base, "frame_", 6) == 0 && bl > 3 && strcasecmp(base + bl - 3, ".bm") == 0;
    if(!is_meta && !is_frame) return false;
    size_t dl = (size_t)(base - 1 - path);
    if(dl >= sizeof app->anim_dir) return false;
    memcpy(app->anim_dir, path, dl);
    app->anim_dir[dl] = 0;
    if(!anim_read_meta(app)) return false;
    app->anim_pick = is_frame ? atoi(base + 6) : -1;

    app->conv_anim = true;
    app->conv_err[0] = 0;
    app->conv_pct = 0;
    set_state(app, StateConvert);
    int fb = sstv_anim_frame_bytes(&app->anim);
    anim_release_mem(app);
    /* two separate blocks: the encoder's scratch (one segment of frames + the model, ~24 KB) and
     * the output, as big as the largest free block allows (up to ANIM_TX_MAX) */
    size_t sb = (size_t)sstv_anim_scratch_bytes(&app->anim);
    size_t have = memmgr_heap_get_max_free_block();
    FURI_LOG_I(TAG, "anim %dx%d n=%d scratch %u max block %u free %u", app->anim.w, app->anim.h, app->anim.n, (unsigned)sb,
               (unsigned)have, (unsigned)memmgr_get_free_heap());
    if(have < sb + 2048) {
        snprintf(app->conv_err, sizeof app->conv_err, "no memory: need %uK, have %uK", (unsigned)(sb / 1024 + 2), (unsigned)(have / 1024));
        anim_restore_mem(app);
        view_port_update(app->vp);
        return true;
    }
    uint8_t* scratch = malloc(sb);
    size_t cap = memmgr_heap_get_max_free_block();
    cap = cap > 6144 ? cap - 6144 : 0; /* leave room for the frame reads and the TX parity */
    if(cap > ANIM_TX_MAX) cap = ANIM_TX_MAX;
    if(cap < 4096) {
        free(scratch);
        snprintf(app->conv_err, sizeof app->conv_err, "no memory for the output");
        anim_restore_mem(app);
        view_port_update(app->vp);
        return true;
    }
    app->anim_ci = compress_icon_alloc((size_t)fb + 64);
    uint8_t* out = malloc(cap);
    app->anim_read_fail = false;
    int len = sstv_anim_encode(&app->anim, anim_get, app, scratch, out, (int)cap, anim_progress, app);
    free(scratch);
    if(len <= 0) {
        free(out);
        compress_icon_free(app->anim_ci);
        app->anim_ci = NULL;
        strncpy(app->conv_err, app->anim_read_fail ? "can't read a frame" : len == -2 ? "odd meta.txt" : "too big for memory",
                sizeof app->conv_err - 1);
        anim_restore_mem(app);
        view_port_update(app->vp);
        return true;
    }
    app->anim_stream = realloc(out, (size_t)len);
    app->anim_len = len;
    anim_name(app->anim_dir, app->name);
    app->anim_frame = malloc((size_t)fb);
    app->anim_pos = 0;
    if(!anim_load_frame(app, app->anim.order[0], app->anim_frame)) memset(app->anim_frame, 0, (size_t)fb);
    app->is_anim = true;
    app->is_note = false;
    furi_timer_start(app->anim_timer, furi_ms_to_ticks(1000 / (uint32_t)app->anim.fps));
    set_state(app, StateAnim);
    return true;
}

static void start_tx_anim(SstvApp* app) {
    chime_start();
    if(sstv_tx_start_anim(app->anim_stream, app->anim_len, app->name, tx_callback, app)) {
        app->frames_sent = 0; /* a resend from Done must not show the last pass's count */
        /* shortcut in the grid for sending it again */
        FuriString* sp = furi_string_alloc_printf("%s/%s.anim", SSTV_DATA_DIR, app->name);
        File* f = storage_file_alloc(app->storage);
        if(storage_file_open(f, furi_string_get_cstr(sp), FSAM_WRITE, FSOM_CREATE_ALWAYS))
            storage_file_write(f, app->anim_dir, strlen(app->anim_dir));
        storage_file_close(f);
        storage_file_free(f);
        furi_string_free(sp);
        app->anim_shortcut_new = true;
        app->frames_total = (uint16_t)sstv_tx_total_frames();
        app->coded = false;
        app->is_note = false;
        app->lines_done = 0;
        set_state(app, StateTransmit);
    } else {
        FURI_LOG_E(TAG, "anim tx: speaker busy or no memory (%u free)", (unsigned)memmgr_heap_get_max_free_block());
        notification_message(app->notif, &sequence_error);
    }
}

/* ---- animation list: every animation folder (with a meta.txt) under /ext/dolphin and under
 * each asset pack's Anims folder, flipped through with Left/Right, the current one playing from
 * the card as it is (packing happens only on OK). ---- */
#define AL_MAX_ROOTS 8

static bool al_is_anim_dir(Storage* storage, const char* root, const char* name) {
    FuriString* mp = furi_string_alloc_printf("%s/%s/meta.txt", root, name);
    FileInfo fi;
    bool ok = storage_common_stat(storage, furi_string_get_cstr(mp), &fi) == FSE_OK;
    furi_string_free(mp);
    return ok;
}

/* The k-th animation folder of `root` into `name` (cap bytes); counts them when k < 0. */
static int al_scan_root(Storage* storage, const char* root, int k, char* name, size_t cap) {
    File* dir = storage_file_alloc(storage);
    int n = 0;
    if(storage_dir_open(dir, root)) {
        FileInfo info;
        char nm[64];
        while(storage_dir_read(dir, &info, nm, sizeof nm)) {
            if(!(info.flags & FSF_DIRECTORY) || nm[0] == '.') continue;
            if(!al_is_anim_dir(storage, root, nm)) continue;
            if(n == k) {
                strncpy(name, nm, cap - 1);
                name[cap - 1] = 0;
                n++;
                break;
            }
            n++;
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    return n;
}

static void al_close(SstvApp* app) {
    if(app->anim_timer) furi_timer_stop(app->anim_timer);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    free(app->anim_frame);
    app->anim_frame = NULL;
    furi_mutex_release(app->mutex);
    if(app->anim_ci) compress_icon_free(app->anim_ci);
    app->anim_ci = NULL;
}

/* Shows animation al_index: its folder into anim_dir, its meta, its first frame, playing. */
static void al_show(SstvApp* app) {
    al_close(app);
    int k = app->al_index;
    char name[64] = "";
    for(int r = 0; r < app->al_nroots; r++) {
        if(k < app->al_root_count[r]) {
            al_scan_root(app->storage, app->al_roots[r], k, name, sizeof name);
            snprintf(app->anim_dir, sizeof app->anim_dir, "%s/%s", app->al_roots[r], name);
            break;
        }
        k -= app->al_root_count[r];
    }
    { /* sent before? a <name>.anim shortcut was written when it went out */
        char nm[SSTV_NOTE_NAME_MAX + 1];
        anim_name(app->anim_dir, nm);
        FuriString* sp = furi_string_alloc_printf("%s/%s.anim", SSTV_DATA_DIR, nm);
        FileInfo fi;
        app->al_sent = name[0] && storage_common_stat(app->storage, furi_string_get_cstr(sp), &fi) == FSE_OK;
        furi_string_free(sp);
    }
    if(!name[0] || !read_anim_meta(app->storage, app->anim_dir, &app->anim)) {
        app->anim.n = 0;
        view_port_update(app->vp);
        return;
    }
    int fb = sstv_anim_frame_bytes(&app->anim);
    app->anim_ci = compress_icon_alloc((size_t)fb + 64);
    uint8_t* fr = malloc((size_t)fb);
    if(!anim_load_frame(app, app->anim.order[0], fr)) memset(fr, 0, (size_t)fb);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->anim_frame = fr;
    app->anim_pos = 0;
    furi_mutex_release(app->mutex);
    furi_timer_start(app->anim_timer, furi_ms_to_ticks(1000 / (uint32_t)app->anim.fps));
    view_port_update(app->vp);
}

static void alist_open(SstvApp* app) {
    if(!app->al_roots) app->al_roots = malloc(sizeof(*app->al_roots) * AL_MAX_ROOTS);
    app->al_nroots = 0;
    app->al_count = 0;
    strcpy(app->al_roots[app->al_nroots++], "/ext/dolphin");
    File* dir = storage_file_alloc(app->storage);
    if(storage_dir_open(dir, "/ext/asset_packs")) {
        FileInfo info;
        char nm[48];
        while(app->al_nroots < AL_MAX_ROOTS && storage_dir_read(dir, &info, nm, sizeof nm)) {
            if(!(info.flags & FSF_DIRECTORY) || nm[0] == '.') continue;
            snprintf(app->al_roots[app->al_nroots], sizeof app->al_roots[0], "/ext/asset_packs/%s/Anims", nm);
            app->al_nroots++;
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    for(int r = 0; r < app->al_nroots; r++) {
        app->al_root_count[r] = al_scan_root(app->storage, app->al_roots[r], -1, NULL, 0);
        app->al_count += app->al_root_count[r];
    }
    if(app->al_index >= app->al_count) app->al_index = 0;
    set_state(app, StateAnimList);
    al_show(app);
}

static void alist_leave(SstvApp* app) {
    al_close(app);
    free(app->al_roots);
    app->al_roots = NULL;
}

/* Any .txt on the card as a note: read up to SSTV_NOTE_MAX_CHARS bytes (cut on a whole UTF-8
 * character, carriage returns dropped), then the usual note preview. */
static void open_txt(SstvApp* app) {
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogsFileBrowserOptions opt;
    dialog_file_browser_set_basic_options(&opt, ".txt", NULL);
    opt.base_path = "/ext";
    opt.hide_dot_files = true;
    FuriString* pick = furi_string_alloc_set(app->last_txt_dir[0] ? app->last_txt_dir : "/ext");
    gui_remove_view_port(app->gui, app->vp);
    bool chosen = dialog_file_browser_show(dialogs, pick, pick, &opt);
    gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);
    furi_record_close(RECORD_DIALOGS);
    if(chosen) {
        const char* path = furi_string_get_cstr(pick);
        strncpy(app->last_txt_dir, path, sizeof app->last_txt_dir - 1);
        app->last_txt_dir[sizeof app->last_txt_dir - 1] = 0;
        char* slash = strrchr(app->last_txt_dir, '/');
        if(slash && slash != app->last_txt_dir) *slash = 0;
        File* f = storage_file_alloc(app->storage);
        size_t n = 0;
        if(storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) n = storage_file_read(f, app->note_text, SSTV_NOTE_MAX_CHARS);
        storage_file_close(f);
        storage_file_free(f);
        /* keep whole UTF-8 characters: drop a lead byte whose continuation bytes were cut off */
        size_t k = n;
        while(k > 0 && ((uint8_t)app->note_text[k - 1] & 0xC0) == 0x80) k--;
        if(k > 0 && ((uint8_t)app->note_text[k - 1] & 0x80)) {
            uint8_t lead = (uint8_t)app->note_text[k - 1];
            size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if(n - (k - 1) < need) n = k - 1;
        }
        size_t w = 0;
        for(size_t r = 0; r < n; r++) {
            char c = app->note_text[r];
            if(c == 0) break;
            if(c != '\r') app->note_text[w++] = c;
        }
        app->note_text[w] = 0;
        app->editing_user_idx = -1;
        if(w > 0) set_state(app, StateNotePreview);
    }
    furi_string_free(pick);
    view_port_update(app->vp);
}

static void open_sd(SstvApp* app) {
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogsFileBrowserOptions opt;
    dialog_file_browser_set_basic_options(&opt, "*", NULL);
    opt.base_path = "/ext";
    opt.hide_dot_files = true;
    FuriString* pick = furi_string_alloc_set(app->last_sd_dir[0] ? app->last_sd_dir : "/ext");
    gui_remove_view_port(app->gui, app->vp);
    bool chosen = dialog_file_browser_show(dialogs, pick, pick, &opt);
    gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);
    furi_record_close(RECORD_DIALOGS);
    if(!chosen) {
        furi_string_free(pick);
        view_port_update(app->vp);
        return;
    }
    const char* path = furi_string_get_cstr(pick);
    /* remember the folder for the next pick */
    strncpy(app->last_sd_dir, path, sizeof(app->last_sd_dir) - 1);
    app->last_sd_dir[sizeof(app->last_sd_dir) - 1] = 0;
    char* slash = strrchr(app->last_sd_dir, '/');
    if(slash && slash != app->last_sd_dir) *slash = 0;

    convert_still(app, path); /* a frame_N.bm becomes a picture here; whole animations live under Animation */
    furi_string_free(pick);
}

/* End-of-pass duet: the Flipper plays C6 E6 right after its last symbol; the Playdate answers
 * G6 C7 when every line arrived (E-flat6 C6 when some are missing), 270 ms after it decodes
 * END, so the four notes run back to back across the two devices. */
#define CHIME_NOTE_MS 150
static void chime_first_half(void) {
    if(!furi_hal_speaker_acquire(100)) return;
    furi_hal_speaker_start(1046.5f, 1.0f); /* C6 */
    furi_delay_ms(CHIME_NOTE_MS);
    furi_hal_speaker_start(1318.5f, 1.0f); /* E6 */
    furi_delay_ms(CHIME_NOTE_MS);
    furi_hal_speaker_stop();
    furi_hal_speaker_release();
}

static void start_calibrate(SstvApp* app) {
    app->sweep = false;
    app->calib = true;
    if(sstv_tx_start_calibrate(tx_callback, app)) {
        set_state(app, StateTest);
    } else {
        app->calib = false;
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
        case EvAnimTick:
            anim_tick(app);
            break;
        case EvToastTick: /* redraw the notice; once it is over, one last redraw clears it */
            if(furi_get_tick() >= app->toast_until) {
                furi_timer_stop(app->toast_timer);
                app->toast[0] = 0;
            }
            view_port_update(app->vp);
            break;
        case EvTxDone:
            sstv_tx_stop();
            set_state(app, StateDone);
            chime_first_half();
            break;
        case EvInput:
            if(ev.input.type != InputTypeShort && ev.input.type != InputTypeRepeat) break;
            if(app->state == StateConvert) {
                if(app->conv_err[0] && app->anim_from_list) { /* packing failed: back to the list */
                    app->anim_from_list = false;
                    set_state(app, StateAnimList);
                    al_show(app);
                } else if(app->conv_err[0]) {
                    set_state(app, StateBrowse);
                }
                break;
            }
            if(app->state == StateOptions) {
                if(ev.input.key == InputKeyDown) app->opt_index = (app->opt_index + 1) % OPT_COUNT;
                if(ev.input.key == InputKeyUp) app->opt_index = (app->opt_index + OPT_COUNT - 1) % OPT_COUNT;
                if(ev.input.type == InputTypeShort && ev.input.key == InputKeyBack) set_state(app, StateMenu);
                if(ev.input.type == InputTypeShort && ev.input.key == InputKeyOk) {
                    if(app->opt_index == 0) start_calibrate(app);
                    else set_state(app, StateAbout);
                }
                view_port_update(app->vp);
                break;
            }
            if(app->state == StateAbout) {
                if(ev.input.type == InputTypeShort && ev.input.key == InputKeyBack) set_state(app, StateOptions);
                view_port_update(app->vp);
                break;
            }
            if(app->state == StateAnimList) {
                if(app->al_count > 0 && (ev.input.key == InputKeyRight || ev.input.key == InputKeyLeft)) {
                    app->al_index = (app->al_index + (ev.input.key == InputKeyRight ? 1 : app->al_count - 1)) % app->al_count;
                    al_show(app);
                }
                if(app->al_count > 0 && (ev.input.key == InputKeyDown || ev.input.key == InputKeyUp)) {
                    /* ten at a time; wraps like Left/Right */
                    int step = 10 % app->al_count;
                    app->al_index = (app->al_index + (ev.input.key == InputKeyDown ? step : app->al_count - step)) % app->al_count;
                    al_show(app);
                }
                if(ev.input.type != InputTypeShort) break;
                if(ev.input.key == InputKeyBack) {
                    alist_leave(app);
                    set_state(app, StateMenu);
                }
                if(ev.input.key == InputKeyOk && app->al_count > 0 && app->anim.n > 0) {
                    FuriString* mp = furi_string_alloc_printf("%s/meta.txt", app->anim_dir);
                    al_close(app);
                    if(anim_open(app, furi_string_get_cstr(mp))) app->anim_from_list = true;
                    furi_string_free(mp);
                }
                break;
            }
            if(app->state == StateAnim && ev.input.type == InputTypeShort) {
                if(ev.input.key == InputKeyOk) start_tx_anim(app);
                if(ev.input.key == InputKeyBack && app->anim_from_list) { /* back to the list, same animation */
                    anim_close(app);
                    app->anim_from_list = false;
                    set_state(app, StateAnimList);
                    al_show(app);
                    break;
                }
                if(ev.input.key == InputKeyBack) {
                    anim_close(app);
                    if(app->anim_shortcut_new) { /* show the new shortcut tile, cursor on it */
                        app->anim_shortcut_new = false;
                        char fname[40];
                        snprintf(fname, sizeof fname, "%s.anim", app->name);
                        scan_files(app);
                        for(int k = 0; k < app->file_count; k++)
                            if(strcmp(app->files[k], fname) == 0) app->browse_sel = k + 1;
                    }
                    set_state(app, StateBrowse);
                }
                if(ev.input.key == InputKeyLeft && app->anim_pick >= 0) { /* just the picked frame, as a picture */
                    FuriString* fp = furi_string_alloc_printf("%s/frame_%d.bm", app->anim_dir, app->anim_pick);
                    anim_close(app);
                    convert_still(app, furi_string_get_cstr(fp));
                    furi_string_free(fp);
                }
                break;
            }
            if(app->state == StateMenu) {
                /* 2x2 grid: 0 Animation, 1 Image / 2 Options, 3 Note */
                if(ev.input.key == InputKeyLeft || ev.input.key == InputKeyRight) app->menu_index ^= 1;
                if(ev.input.key == InputKeyUp || ev.input.key == InputKeyDown) app->menu_index ^= 2;
                if(ev.input.key == InputKeyOk && ev.input.type == InputTypeShort) {
                    /* 0 Animation, 1 Image / 2 Options, 3 Note */
                    if(app->menu_index == 2) {
                        app->opt_index = 0;
                        set_state(app, StateOptions);
                    } else if(app->menu_index == 0) {
                        alist_open(app);
                    } else if(app->menu_index == 1) {
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
                int n = app->file_count + 1; /* + the SD card tile */
                if(ev.input.key == InputKeyRight) app->browse_sel = (app->browse_sel + 1) % n;
                if(ev.input.key == InputKeyLeft) app->browse_sel = (app->browse_sel + n - 1) % n;
                if(ev.input.key == InputKeyDown) app->browse_sel = (app->browse_sel + TILES_PER_PAGE) % n;
                if(ev.input.key == InputKeyUp) app->browse_sel = (app->browse_sel + n - TILES_PER_PAGE % n) % n;
                furi_mutex_release(app->mutex);
                ensure_thumbs(app); /* the cursor may have moved to another page */
                if(ev.input.key == InputKeyBack && ev.input.type == InputTypeShort) set_state(app, StateMenu);
                if(ev.input.key == InputKeyOk && ev.input.type == InputTypeShort) {
                    if(app->browse_sel == 0) {
                        open_sd(app);
                    } else {
                        app->file_index = app->browse_sel - 1;
                        if(is_anim_file(app->files[app->file_index])) {
                            FuriString* sp = furi_string_alloc_printf("%s/%s", SSTV_DATA_DIR, app->files[app->file_index]);
                            char dir[128];
                            if(read_shortcut(app->storage, furi_string_get_cstr(sp), dir, sizeof dir)) {
                                FuriString* mp = furi_string_alloc_printf("%s/meta.txt", dir);
                                if(!anim_open(app, furi_string_get_cstr(mp))) {
                                    strncpy(app->conv_err, "animation folder is gone", sizeof app->conv_err - 1);
                                    app->conv_anim = true;
                                    set_state(app, StateConvert);
                                }
                                furi_string_free(mp);
                            }
                            furi_string_free(sp);
                        } else if(open_file(app)) set_state(app, StatePreview);
                    }
                }
                view_port_update(app->vp);
                break;
            }
            if(app->state == StateNoteList) {
                int len = NOTE_LIST_LEN(app);
                if(ev.input.key == InputKeyDown) app->note_index = (app->note_index + 1) % len;
                if(ev.input.key == InputKeyUp) app->note_index = (app->note_index + len - 1) % len;
                if(ev.input.key == InputKeyBack && ev.input.type == InputTypeShort) set_state(app, StateMenu);
                if(ev.input.key == InputKeyOk && ev.input.type == InputTypeShort) {
                    if(app->note_index == 0) {
                        app->note_text[0] = 0;
                        app->editing_user_idx = -1;
                        if(edit_note(app)) {
                            /* Save adds it to the saved list, then goes to the preview to send */
                            app->editing_user_idx = add_user_note(app, app->note_text);
                            save_user_notes(app);
                            set_state(app, StateNotePreview);
                        } else {
                            set_state(app, StateNoteList);
                        }
                    } else if(app->note_index == 1) {
                        open_txt(app);
                    } else if(app->note_index <= app->user_note_count + 1) {
                        strncpy(app->note_text, un_ptr(app, app->note_index - 2), UN_MAX);
                        app->note_text[SSTV_NOTE_MAX_CHARS] = 0;
                        app->editing_user_idx = app->note_index - 2;
                        set_state(app, StateNotePreview);
                    } else {
                        /* preset: edit-in-place is offered from the preview (ASCII keyboard) */
                        strncpy(app->note_text, SSTV_NOTE_PRESETS[app->note_index - 2 - app->user_note_count], SSTV_NOTE_MAX_CHARS);
                        app->note_text[SSTV_NOTE_MAX_CHARS] = 0;
                        app->editing_user_idx = -1;
                        set_state(app, StateNotePreview);
                    }
                }
                view_port_update(app->vp);
                break;
            }
            if(app->state == StatePreview && (app->pic_del_confirm || ev.input.key == InputKeyLeft)) {
                if(ev.input.type != InputTypeShort) break;
                if(!app->pic_del_confirm) {
                    app->pic_del_confirm = true; /* Left: ask first */
                } else if(ev.input.key == InputKeyOk) {
                    /* the picture's .pbm in the data folder (presets came from tools/make_*.py,
                     * SD imports from convert_still: all plain files, nothing built in) */
                    FuriString* dp = furi_string_alloc_printf("%s/%s", SSTV_DATA_DIR, app->files[app->file_index]);
                    bool gone = storage_simply_remove(app->storage, furi_string_get_cstr(dp));
                    furi_string_free(dp);
                    if(gone) {
                        /* name without .pbm; the notice stays TOAST_MS over the grid */
                        strncpy(app->toast, app->files[app->file_index], sizeof app->toast - 1);
                        char* dot = strrchr(app->toast, '.');
                        if(dot) *dot = 0;
                        app->toast_until = furi_get_tick() + furi_ms_to_ticks(TOAST_MS);
                        furi_timer_start(app->toast_timer, furi_ms_to_ticks(TOAST_FRAME_MS));
                        notification_message(app->notif, &sequence_blink_green_100);
                    } else {
                        notification_message(app->notif, &sequence_error);
                    }
                    app->pic_del_confirm = false;
                    int sel = app->browse_sel;
                    scan_files(app);
                    app->browse_sel = sel > app->file_count ? app->file_count : sel; /* the next one slides in */
                    ensure_thumbs(app);
                    set_state(app, StateBrowse);
                } else if(ev.input.key == InputKeyBack) {
                    app->pic_del_confirm = false;
                }
                view_port_update(app->vp);
                break;
            }
            if(app->state == StateNotePreview) {
                if(app->note_del_confirm) {
                    if(ev.input.key == InputKeyOk && ev.input.type == InputTypeShort) {
                        delete_user_note(app, app->editing_user_idx);
                        save_user_notes(app);
                        app->note_del_confirm = false;
                        app->editing_user_idx = -1;
                        set_state(app, StateNoteList);
                    } else if(ev.input.key == InputKeyBack && ev.input.type == InputTypeShort) {
                        app->note_del_confirm = false;
                    }
                    view_port_update(app->vp);
                    break;
                }
                if(ev.input.key == InputKeyBack && ev.input.type == InputTypeShort) set_state(app, StateNoteList);
                if(ev.input.key == InputKeyUp && ev.input.type == InputTypeShort) {
                    if(edit_note(app) && app->editing_user_idx >= 0) {
                        strncpy(un_ptr(app, app->editing_user_idx), app->note_text, UN_MAX); /* saved notes keep UN_MAX bytes */
                        un_ptr(app, app->editing_user_idx)[UN_MAX] = 0;
                        save_user_notes(app);
                    }
                    set_state(app, StateNotePreview);
                }
                if(ev.input.key == InputKeyLeft && ev.input.type == InputTypeShort && app->editing_user_idx >= 0)
                    app->note_del_confirm = true;
                if(ev.input.key == InputKeyOk && ev.input.type == InputTypeShort) start_tx_note(app);
                view_port_update(app->vp);
                break;
            }
            if(ev.input.type != InputTypeShort) break;
            if(ev.input.key == InputKeyBack) {
                if(app->state == StateTest) {
                    sstv_tx_stop();
                    set_state(app, StateOptions); /* the tests all start from Options */
                    app->calib = false;
                } else if(app->state == StateTransmit) {
                    /* first Back pauses; while paused, Back stops */
                    if(sstv_tx_is_paused()) {
                        sstv_tx_stop();
                        set_state(app, app->is_anim ? StateAnim : app->is_note ? StateNotePreview : StatePreview);
                    } else {
                        sstv_tx_pause();
                        app->pausing = true;
                        view_port_update(app->vp);
                    }
                } else if(app->state == StateDone) {
                    sstv_tx_stop();
                    set_state(app, app->is_anim ? StateAnim : app->is_note ? StateNotePreview : StatePreview);
                } else if(app->state == StatePreview) {
                    set_state(app, StateBrowse);
                } else {
                    set_state(app, StateMenu);
                }
            } else if(ev.input.key == InputKeyOk) {
                if(app->state == StateTransmit && sstv_tx_is_paused()) {
                    sstv_tx_resume();
                    app->pausing = false;
                    view_port_update(app->vp);
                } else if(app->state == StateDone) {
                    if(app->is_anim) start_tx_anim(app);
                    else if(app->is_note) start_tx_note(app);
                    else start_tx(app);
                } else if(app->state == StatePreview) {
                    start_tx(app);
                }
            }
            break;
        }
    }
    sstv_tx_stop();
    if(app->backlight_held) {
        notification_message(app->notif, &sequence_display_backlight_enforce_auto);
        app->backlight_held = false;
    }
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
    app->user_notes = malloc((size_t)MAX_USER_NOTES * UN_STRIDE);
    app->editing_user_idx = -1;
    app->gui = furi_record_open(RECORD_GUI);
    app->notif = furi_record_open(RECORD_NOTIFICATION);
    app->vp = view_port_alloc();
    view_port_draw_callback_set(app->vp, draw_callback, app);
    view_port_input_callback_set(app->vp, input_callback, app);
    app->scan_anim = icon_animation_alloc(&A_scan);
    icon_animation_set_update_callback(app->scan_anim, scan_anim_cb, app);
    app->note_anim = icon_animation_alloc(&A_note_write);
    icon_animation_set_update_callback(app->note_anim, scan_anim_cb, app);
    app->film_anim = icon_animation_alloc(&A_anim_play);
    icon_animation_set_update_callback(app->film_anim, scan_anim_cb, app);
    app->opts_anim = icon_animation_alloc(&A_options);
    icon_animation_set_update_callback(app->opts_anim, scan_anim_cb, app);
    app->menu_anim = -1;
    app->anim_timer = furi_timer_alloc(anim_timer_cb, FuriTimerTypePeriodic, app);
    app->toast_timer = furi_timer_alloc(toast_timer_cb, FuriTimerTypePeriodic, app);

    app->storage = furi_record_open(RECORD_STORAGE);
    /* the app used to be called SSTV: carry the pictures over */
    FileInfo info;
    if(storage_common_stat(app->storage, SSTV_OLD_DATA_DIR, &info) == FSE_OK &&
       storage_common_stat(app->storage, SSTV_DATA_DIR, &info) != FSE_OK) {
        storage_common_rename(app->storage, SSTV_OLD_DATA_DIR, SSTV_DATA_DIR);
    }
    storage_simply_mkdir(app->storage, SSTV_DATA_DIR);
    load_user_notes(app);
    scan_files(app);
    run_viewport(app);
    anim_close(app);
    furi_timer_free(app->anim_timer);
    furi_timer_stop(app->toast_timer);
    furi_timer_free(app->toast_timer); /* before the view port it redraws goes away */

    furi_record_close(RECORD_STORAGE);
    icon_animation_stop(app->scan_anim);
    icon_animation_free(app->scan_anim);
    icon_animation_stop(app->note_anim);
    icon_animation_free(app->note_anim);
    icon_animation_stop(app->film_anim);
    icon_animation_free(app->film_anim);
    icon_animation_stop(app->opts_anim);
    icon_animation_free(app->opts_anim);
    view_port_free(app->vp);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    led_stop(app);
    furi_hal_light_set(LightRed | LightGreen | LightBlue, 0);
    free(app->image);
    free(app->user_notes);
    furi_string_free(app->path);
    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->queue);
    free(app);
    return 0;
}
