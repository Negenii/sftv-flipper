#include "sstv_tx.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_interrupt.h>
#include <furi_hal_speaker.h>
#include <stm32wbxx_ll_tim.h>

#include "sstv_params.h"
#include "sstv_frame.h"

/* The speaker is on TIM16 CH1. furi_hal_speaker_acquire() enables the TIM16 bus clock and
 * routes the pin; we then program the timer ourselves.
 *
 * Symbol timing is done in hardware: the repetition counter (RCR) makes the timer raise one
 * update event per symbol (after N whole PWM cycles), and ARR/CCR/RCR are written through
 * their preload registers, so the values programmed during symbol k take effect exactly at
 * the start of symbol k+1. The ISR therefore runs once per symbol (500 Hz) and only has to
 * arrive within 2 ms, instead of once per PWM period (up to 4.5 kHz) where any interrupt
 * latency longer than a period lost cycles and stretched symbols. */
#define TX_TIMER TIM16
#define TX_TIMER_CLK_HZ 64000000UL

typedef enum {
    PhaseLeader,
    PhaseSync,
    PhaseBits,
    PhasePaused,
    PhaseDone,
} TxPhase;

typedef struct {
    uint8_t* frames; /* physical frames, interleaved (SSTV_MAX_FRAMES capacity) */
    uint8_t* lines_after; /* lines complete after each physical frame */
    uint16_t total; /* physical frames for this image */
    int data_frames; /* data positions of the pass (for the parity frame lookup), 0 unknown */
    bool coded;
    volatile TxPhase phase;
    volatile uint16_t line; /* physical frame index */
    uint16_t symbol; /* next symbol index within the frame */
    uint16_t leader_left; /* leader symbols still to send */
    bool ladder; /* start leader: tone ladder (sstv_leader_symbol); resume leader: cycling 0..3 */
    uint8_t lock_left; /* lock frames still to send after the start ladder (preamble) */
    uint8_t lock[SSTV_FRAME_BYTES]; /* lock-frame pattern */
    int pending_line; /* physical frame whose last symbol was just programmed, -1 none */
    bool loop; /* restart from frame 0 after the last one */
    bool test; /* tone test: symbols from test_pattern with test_tone_hz instead of frames */
    bool sweep; /* HF sweep (inside the test mode): one frequency step per frame */
    int band; /* SSTV_BAND_AUDIBLE or SSTV_BAND_HF (near-ultrasound tones and sync) */
    uint8_t sync_part; /* HF sync is sent as SYNC_PARTS back-to-back 4 ms pieces */
    int32_t hf_err_ticks; /* HF symbol timing accumulator (see hf_cycles) */
    /* pause/resume: a prebuilt control block (4 interleaved physical frames) is played
     * between data frames; then the sequencer either holds silent (pause) or continues. */
    uint8_t ctrl[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    volatile int ctrl_line; /* frame within the control block being sent, -1 = not sending one */
    volatile bool ctrl_is_pause; /* the control block in flight is PAUSE (else RESUME) */
    volatile bool pause_pending; /* app asked to pause; honoured at the next data-frame boundary */
    volatile bool resume_pending; /* app asked to resume; honoured from the paused hold */
    volatile bool paused; /* holding silent after a PAUSE block */
    uint8_t test_pattern[SSTV_SYMBOLS_PER_FRAME];
    volatile int playing; /* symbol now on the speaker (see sstv_tx_current_symbol) */
    int queued; /* symbol programmed to start at the next update event */
    SstvTxCallback cb;
    void* ctx;
    bool running;
} SstvTx;

static SstvTx tx = {0};
static int next_band = SSTV_BAND_AUDIBLE; /* band for the next start (sstv_tx_set_band) */

static const int tone_hz[SSTV_NUM_TONES] = SSTV_TONES_INIT;
static const int tone_duty[SSTV_NUM_TONES] = SSTV_TONE_DUTY_PCT_INIT;
#define ARR_FOR(hz) (TX_TIMER_CLK_HZ / (uint32_t)(hz) - 1)
#define CYCLES_FOR(hz, us) ((uint32_t)(hz) * (uint32_t)(us) / 1000000UL)
#define ARR_SYNC ARR_FOR(SSTV_FSYNC_HZ) /* 12799 -> 5000.0 Hz */
#define CYCLES_SYNC CYCLES_FOR(SSTV_FSYNC_HZ, SSTV_SYNC_US) /* 80 at 16 ms, fits 8-bit RCR */

static const int hf_tone_hz[SSTV_MAX_TONES] = SSTV_HF_TONES_INIT;
#define HF_SYMBOL_TICKS ((uint32_t)(TX_TIMER_CLK_HZ / 1000000UL * SSTV_HF_SYMBOL_US)) /* 192000 */

/* HF symbols: not every tone fits a whole number of cycles into 3 ms (18.5 kHz = 55.5), so the
 * cycle count is chosen per symbol to keep the accumulated time error under one cycle: the
 * average symbol is exactly 3 ms in timer ticks, whatever the rounding of ARR. */
static uint32_t hf_cycles(uint32_t arr) {
    uint32_t period = arr + 1; /* ticks per cycle */
    tx.hf_err_ticks += (int32_t)HF_SYMBOL_TICKS;
    int32_t c = (tx.hf_err_ticks + (int32_t)period / 2) / (int32_t)period;
    if(c < 1) c = 1;
    tx.hf_err_ticks -= c * (int32_t)period;
    return (uint32_t)c;
}
/* The repetition counter is 8 bits: a 16 ms sync at 20.5 kHz is 328 cycles, so the HF sync goes
 * out as 4 back-to-back pieces of 4 ms (82 cycles each), one continuous burst on the air. HF data
 * symbols are 3 ms (SSTV_HF_SYMBOL_US). */
#define SYNC_PIECE_US 4000
#define SYNC_PARTS (SSTV_SYNC_US / SYNC_PIECE_US)

static inline void tone_settings(int v, uint32_t* arr, uint32_t* cycles, uint32_t* duty_pct) {
    if(tx.band == SSTV_BAND_HF) {
        *arr = ARR_FOR(hf_tone_hz[v] ? hf_tone_hz[v] : 3000); /* (no silence in the HF set now) */
        *cycles = hf_cycles(*arr);
        *duty_pct = hf_tone_hz[v] ? SSTV_HF_DUTY_PCT : 0;
        return;
    }
    if(tone_hz[v] == 0) {
        /* Silence: keep the timer running at 3000 Hz for the symbol length with 0 % duty. */
        *arr = ARR_FOR(3000);
        *cycles = CYCLES_FOR(3000, SSTV_BAND_SYMBOL_US(tx.band)); /* 12 at 4 ms, 9 at 3 ms */
        *duty_pct = 0;
        return;
    }
    *arr = ARR_FOR(tone_hz[v]); /* 3000 -> 21332 (3000.05 Hz), 3500 -> 18284, 4000 -> 15999 */
    *cycles = CYCLES_FOR(tone_hz[v], SSTV_SYMBOL_US); /* 12, 14, 16 at 4 ms */
    *duty_pct = (uint32_t)tone_duty[v];
}

/* Tone test: the six test values and their PWM duty (same pre-emphasis idea as the data
 * tones: louder duty for the weak high tones). */
static const int test_tone_hz[SSTV_TEST_TONES] = {0, 2500, 3000, 3500, 4000, 4500};
static const int test_tone_duty[SSTV_TEST_TONES] = {0, 12, 12, 30, 50, 50};

static inline void test_tone_settings(int v, uint32_t* arr, uint32_t* cycles, uint32_t* duty_pct) {
    if(test_tone_hz[v] == 0) {
        *arr = ARR_FOR(3000);
        *cycles = CYCLES_FOR(3000, SSTV_SYMBOL_US);
        *duty_pct = 0;
        return;
    }
    *arr = ARR_FOR(test_tone_hz[v]);
    *cycles = CYCLES_FOR(test_tone_hz[v], SSTV_SYMBOL_US);
    *duty_pct = (uint32_t)test_tone_duty[v];
}

/* HF sweep for the near-ultrasound feasibility test (tools/hf_sweep.mjs analyses a recording):
 * frame k of each 20-frame cycle = sync, then SWEEP_ON symbols of one tone, then silence.
 * Step 0 is a 3000 Hz reference (the data band), steps 1..19 are 12.0..21.0 kHz in 500 Hz. */
#define SWEEP_STEPS 20
#define SWEEP_ON 100 /* 400 ms of tone, then 256 ms of silence */
static inline int sweep_hz(int step) {
    return step == 0 ? 3000 : 12000 + (step - 1) * 500;
}

/* Fixed pseudo-random symbol order (LCG, seed 1), reproduced by the analysis script. */
static void test_pattern_fill(uint8_t* out) {
    uint32_t x = 1;
    for(int i = 0; i < SSTV_SYMBOLS_PER_FRAME; i++) {
        x = (x * 1103515245u + 12345u) & 0x7fffffffu;
        out[i] = (uint8_t)((x >> 16) % SSTV_TEST_TONES);
    }
}

/* Advances the sequencer by one symbol and returns its PWM settings. */
static void tx_next_symbol(uint32_t* arr, uint32_t* cycles, uint32_t* duty_pct) {
    *duty_pct = SSTV_SYNC_DUTY_PCT;
    tx.queued = -1;
    switch(tx.phase) {
    case PhaseLeader:
        if(tx.leader_left > 0) {
            tx.leader_left--;
            tx.queued = tx.ladder ? sstv_leader_symbol(SSTV_BAND_LEADER_SYMBOLS(tx.band) - 1 - tx.leader_left) :
                                    tx.leader_left % SSTV_BAND_NUM_TONES(tx.band);
            tone_settings(tx.queued, arr, cycles, duty_pct);
            return;
        }
        tx.phase = PhaseSync;
        /* fall through */
    case PhaseSync:
        if(tx.band == SSTV_BAND_HF) {
            *arr = ARR_FOR(SSTV_HF_FSYNC_HZ);
            *cycles = CYCLES_FOR(SSTV_HF_FSYNC_HZ, SYNC_PIECE_US);
            if(++tx.sync_part < SYNC_PARTS) return; /* more sync pieces follow */
            tx.sync_part = 0;
        } else {
            *arr = ARR_SYNC;
            *cycles = CYCLES_SYNC;
        }
        tx.phase = PhaseBits;
        tx.symbol = 0;
        return;
    case PhaseBits:
        if(tx.test && tx.sweep) {
            int hz = sweep_hz(tx.line % SWEEP_STEPS);
            tx.queued = tx.symbol < SWEEP_ON ? 1 : 0;
            *arr = ARR_FOR(tx.queued ? hz : 3000);
            *cycles = CYCLES_FOR(tx.queued ? hz : 3000, SSTV_SYMBOL_US);
            *duty_pct = tx.queued ? 50 : 0;
        } else if(tx.test) {
            tx.queued = tx.test_pattern[tx.symbol];
            test_tone_settings(tx.queued, arr, cycles, duty_pct);
        } else if(tx.lock_left > 0) {
            tx.queued = sstv_frame_symbol_bits(tx.lock, tx.symbol, SSTV_BAND_BITS(tx.band));
            tone_settings(tx.queued, arr, cycles, duty_pct);
        } else if(tx.ctrl_line >= 0) {
            tx.queued = sstv_frame_symbol_bits(tx.ctrl + tx.ctrl_line * SSTV_FRAME_BYTES, tx.symbol, SSTV_BAND_BITS(tx.band));
            tone_settings(tx.queued, arr, cycles, duty_pct);
        } else {
            tx.queued = sstv_frame_symbol_bits(tx.frames + tx.line * SSTV_FRAME_BYTES, tx.symbol, SSTV_BAND_BITS(tx.band));
            tone_settings(tx.queued, arr, cycles, duty_pct);
        }
        tx.symbol++;
        if(tx.symbol >= (tx.test ? SSTV_SYMBOLS_PER_FRAME : SSTV_BAND_SYMBOLS_PER_FRAME(tx.band))) {
            if(tx.lock_left > 0) {
                /* a preamble lock frame just finished; not part of the pass */
                tx.lock_left--;
                tx.phase = PhaseSync;
                return;
            }
            if(tx.ctrl_line >= 0) {
                /* a control-block frame just finished; it is not counted in the pass */
                tx.ctrl_line++;
                if(tx.ctrl_line >= SSTV_ILEAVE) {
                    tx.ctrl_line = -1;
                    if(tx.ctrl_is_pause) {
                        tx.paused = true;
                        tx.phase = PhasePaused;
                    } else {
                        tx.phase = PhaseSync; /* RESUME sent: play the rest of the pass */
                    }
                } else {
                    tx.phase = PhaseSync;
                }
                return;
            }
            tx.pending_line = tx.line;
            tx.line++;
            if(tx.line >= tx.total) {
                tx.line = 0;
                tx.phase = tx.loop ? PhaseSync : PhaseDone;
            } else if(tx.pause_pending) {
                /* honour the pause between data frames: send the PAUSE block, then hold */
                tx.pause_pending = false;
                tx.ctrl_line = 0;
                tx.ctrl_is_pause = true;
                tx.phase = PhaseSync;
            } else {
                tx.phase = PhaseSync;
            }
        }
        return;
    case PhasePaused:
        /* hold silent; the resume trigger in the ISR flips us back to PhaseLeader */
        tone_settings(0, arr, cycles, duty_pct);
        tx.queued = 0;
        return;
    case PhaseDone:
    default:
        tone_settings(0, arr, cycles, duty_pct);
        return;
    }
}

/* Writes the preload registers; the values take effect at the next update event. */
static inline void tx_program(uint32_t arr, uint32_t cycles, uint32_t duty_pct) {
    LL_TIM_SetAutoReload(TX_TIMER, arr);
    LL_TIM_OC_SetCompareCH1(TX_TIMER, (arr + 1) * duty_pct / 100);
    LL_TIM_SetRepetitionCounter(TX_TIMER, cycles - 1);
}

static void tx_isr(void* context) {
    UNUSED(context);
    if(!LL_TIM_IsActiveFlag_UPDATE(TX_TIMER)) return; /* TIM1 shares this vector */
    LL_TIM_ClearFlag_UPDATE(TX_TIMER);
    if(tx.phase == PhaseDone) return;

    /* A symbol has just started with the values we programmed last time; queue the next. */
    tx.playing = tx.queued;
    if(tx.phase == PhasePaused && tx.resume_pending) {
        /* leave the hold: a short leader re-calibrates the receiver, then the RESUME block */
        tx.resume_pending = false;
        tx.paused = false;
        tx.ctrl_line = 0;
        tx.ctrl_is_pause = false;
        tx.leader_left = SSTV_RESUME_LEADER_SYMBOLS;
        tx.ladder = false;
        tx.phase = PhaseLeader;
    }
    uint32_t arr, cycles, duty;
    tx_next_symbol(&arr, &cycles, &duty);
    tx_program(arr, cycles, duty);

    if(tx.pending_line >= 0) {
        int finished = tx.pending_line;
        tx.pending_line = -1;
        bool last = finished == tx.total - 1;
        if(last && !tx.loop) {
            /* The last symbol is playing now; the programmed "next" symbol (tone 0) must not
             * start: stop outputs at the coming update instead of feeding more symbols. */
            LL_TIM_DisableIT_UPDATE(TX_TIMER);
        }
        if(tx.cb) tx.cb(tx.ctx, (uint16_t)finished, last);
    }
}

/* Programs the timer and starts the sequencer; tx.frames/total/loop/test are set. */
static bool tx_run(bool leader, SstvTxCallback cb, void* ctx) {
    tx.band = tx.test ? SSTV_BAND_AUDIBLE : next_band; /* the tone test and sweep set their own tones */
    tx.cb = cb;
    tx.ctx = ctx;
    tx.line = 0;
    tx.symbol = 0;
    tx.pending_line = -1;
    tx.phase = PhaseLeader;
    tx.leader_left = leader ? SSTV_BAND_LEADER_SYMBOLS(tx.band) : 0;
    tx.hf_err_ticks = 0;
    tx.ladder = leader;
    tx.sync_part = 0;
    tx.lock_left = leader ? SSTV_LOCK_FRAMES : 0; /* start preamble: ladder, then lock frames */
    sstv_lock_frame(tx.lock);
    tx.ctrl_line = -1;
    tx.ctrl_is_pause = false;
    tx.pause_pending = false;
    tx.resume_pending = false;
    tx.paused = false;

    /* Symbol 0 goes straight into the active registers. */
    uint32_t arr0, cyc0, duty0;
    tx_next_symbol(&arr0, &cyc0, &duty0);
    tx.playing = tx.queued;

    LL_TIM_InitTypeDef init = {0};
    init.Prescaler = 0;
    init.CounterMode = LL_TIM_COUNTERMODE_UP;
    init.Autoreload = arr0;
    init.ClockDivision = LL_TIM_CLOCKDIVISION_DIV1;
    init.RepetitionCounter = cyc0 - 1;
    LL_TIM_Init(TX_TIMER, &init);

    LL_TIM_OC_InitTypeDef oc = {0};
    oc.OCMode = LL_TIM_OCMODE_PWM1;
    oc.OCState = LL_TIM_OCSTATE_ENABLE;
    oc.OCNState = LL_TIM_OCSTATE_DISABLE;
    oc.CompareValue = (arr0 + 1) * duty0 / 100;
    oc.OCPolarity = LL_TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity = LL_TIM_OCPOLARITY_HIGH;
    oc.OCIdleState = LL_TIM_OCIDLESTATE_LOW;
    oc.OCNIdleState = LL_TIM_OCIDLESTATE_LOW;
    LL_TIM_OC_Init(TX_TIMER, LL_TIM_CHANNEL_CH1, &oc);

    /* From now on ARR/CCR are buffered: symbol 1 is queued behind symbol 0. */
    LL_TIM_EnableARRPreload(TX_TIMER);
    LL_TIM_OC_EnablePreload(TX_TIMER, LL_TIM_CHANNEL_CH1);
    uint32_t arr1, cyc1, duty1;
    tx_next_symbol(&arr1, &cyc1, &duty1);
    tx_program(arr1, cyc1, duty1);

    LL_TIM_ClearFlag_UPDATE(TX_TIMER); /* LL_TIM_Init generated one */
    furi_hal_interrupt_set_isr(FuriHalInterruptIdTim1UpTim16, tx_isr, NULL);
    LL_TIM_EnableIT_UPDATE(TX_TIMER);
    LL_TIM_EnableAllOutputs(TX_TIMER);
    LL_TIM_EnableCounter(TX_TIMER);

    tx.running = true;
    return true;
}

bool sstv_tx_start(const uint8_t* image_bits, const char* name, bool loop, SstvTxCallback cb, void* ctx) {
    if(tx.running) return false;
    if(!furi_hal_speaker_acquire(1000)) return false;
    tx.frames = malloc(SSTV_MAX_FRAMES * SSTV_FRAME_BYTES);
    tx.lines_after = malloc(SSTV_MAX_FRAMES);
    tx.total = (uint16_t)sstv_build_physical(image_bits, name, tx.frames, tx.lines_after, &tx.coded);
    tx.data_frames = 0;
    for(int d = 1; d <= SSTV_MAX_DATA_FRAMES; d++) {
        if(sstv_pass_total(d, sstv_pass_intro(tx.coded)) == tx.total) {
            tx.data_frames = d;
            break;
        }
    }
    tx.loop = loop;
    tx.test = false;
    tx.sweep = false;
    return tx_run(true, cb, ctx);
}

bool sstv_tx_start_note(const char* text, const char* name, SstvTxCallback cb, void* ctx) {
    if(tx.running) return false;
    if(!furi_hal_speaker_acquire(1000)) return false;
    tx.frames = malloc(SSTV_MAX_FRAMES * SSTV_FRAME_BYTES);
    tx.lines_after = malloc(SSTV_MAX_FRAMES);
    tx.total = (uint16_t)sstv_build_note(text, name, tx.frames, tx.lines_after);
    tx.data_frames = 0; /* note LED just follows the tones; no parity distinction needed */
    tx.coded = false;
    tx.loop = false;
    tx.test = false;
    tx.sweep = false;
    return tx_run(true, cb, ctx);
}

bool sstv_tx_start_test(SstvTxCallback cb, void* ctx) {
    if(tx.running) return false;
    if(!furi_hal_speaker_acquire(1000)) return false;
    tx.frames = NULL;
    tx.lines_after = NULL;
    tx.total = 0xFFFF; /* never "done"; loops until stopped */
    tx.loop = true;
    tx.coded = false;
    tx.test = true;
    tx.sweep = false;
    test_pattern_fill(tx.test_pattern);
    return tx_run(false, cb, ctx);
}

bool sstv_tx_start_sweep(SstvTxCallback cb, void* ctx) {
    if(!sstv_tx_start_test(cb, ctx)) return false;
    tx.sweep = true; /* first frame is the sync; the sweep symbols start after it */
    return true;
}

void sstv_tx_stop(void) {
    if(!tx.running) return;
    LL_TIM_DisableIT_UPDATE(TX_TIMER);
    LL_TIM_DisableCounter(TX_TIMER);
    LL_TIM_DisableAllOutputs(TX_TIMER);
    LL_TIM_ClearFlag_UPDATE(TX_TIMER);
    furi_hal_interrupt_set_isr(FuriHalInterruptIdTim1UpTim16, NULL, NULL);
    tx.phase = PhaseDone;
    tx.playing = -2;
    furi_hal_speaker_release(); /* stops the timer, disables the TIM16 bus, parks the pin */
    free(tx.frames);
    tx.frames = NULL;
    free(tx.lines_after);
    tx.lines_after = NULL;
    tx.running = false;
}

/* Builds a control block (4 identical META frames, interleaved) into tx.ctrl on the caller's
 * thread, so the RS encode never runs inside the ISR. */
static void build_ctrl(int subtype, uint16_t arg) {
    uint8_t logical[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    uint8_t meta[SSTV_PAYLOAD_BYTES];
    memset(meta, 0, sizeof meta);
    meta[0] = (uint8_t)subtype;
    meta[1] = (uint8_t)(arg & 0xFF);
    meta[2] = (uint8_t)(arg >> 8);
    for(int i = 0; i < SSTV_ILEAVE; i++)
        sstv_frame_pack(sstv_hdr(SSTV_FT_META, 0), meta, logical + i * SSTV_FRAME_BYTES);
    sstv_interleave(logical, tx.ctrl);
}

void sstv_tx_pause(void) {
    if(!tx.running || tx.test || tx.paused || tx.pause_pending) return;
    build_ctrl(SSTV_META_PAUSE, tx.line);
    tx.pause_pending = true; /* the ISR starts the block at the next data-frame boundary */
}

void sstv_tx_resume(void) {
    if(!tx.running || !tx.paused || tx.resume_pending) return;
    build_ctrl(SSTV_META_RESUME, tx.line); /* tx.line = the next physical frame we will play */
    tx.resume_pending = true;
}

bool sstv_tx_is_paused(void) {
    return tx.running && tx.paused;
}

bool sstv_tx_is_running(void) {
    return tx.running;
}

int sstv_tx_total_frames(void) {
    return tx.running ? tx.total : 0;
}

int sstv_tx_lines_after(int frames_sent) {
    if(!tx.running || !tx.lines_after || frames_sent <= 0) return 0;
    if(frames_sent > tx.total) frames_sent = tx.total;
    return tx.lines_after[frames_sent - 1];
}

bool sstv_tx_is_coded(void) {
    return tx.running && tx.coded;
}

int sstv_tx_current_symbol(void) {
    return tx.running ? tx.playing : -2;
}

bool sstv_tx_current_is_parity(void) {
    if(!tx.running || tx.test || tx.data_frames == 0) return false;
    return sstv_pass_frame_kind(tx.line, tx.data_frames, sstv_pass_intro(tx.coded)) == SSTV_PASS_PARITY;
}

void sstv_tx_set_band(int band) {
    next_band = band == SSTV_BAND_HF ? SSTV_BAND_HF : SSTV_BAND_AUDIBLE;
}

int sstv_tx_band(void) {
    return next_band;
}
