#include "sstv_tx.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_interrupt.h>
#include <furi_hal_speaker.h>
#include <stm32wbxx_ll_tim.h>

#include "sstv_params.h"
#include "sstv_frame.h"

/* The speaker is on TIM16 CH1. furi_hal_speaker_acquire() enables the TIM16 bus clock and
 * routes the pin; we then program the timer ourselves so that frequency changes happen
 * exactly on cycle boundaries from the update interrupt. */
#define TX_TIMER TIM16
#define TX_TIMER_CLK_HZ 64000000UL

typedef enum {
    PhaseLeader,
    PhaseSync,
    PhaseBits,
    PhaseDone,
} TxPhase;

typedef struct {
    uint8_t* frames; /* SSTV_HEIGHT * SSTV_FRAME_BYTES, pre-packed */
    volatile TxPhase phase;
    volatile uint16_t line;
    uint16_t bit;
    uint32_t cycles_left;
    SstvTxCallback cb;
    void* ctx;
    bool running;
} SstvTx;

static SstvTx tx = {0};

static const uint32_t arr_bit[2] = {
    TX_TIMER_CLK_HZ / SSTV_F0_HZ - 1, /* 31999 -> 2000.00 Hz */
    TX_TIMER_CLK_HZ / SSTV_F1_HZ - 1, /* 21332 -> 3000.05 Hz */
};
static const uint32_t cycles_bit[2] = {
    SSTV_F0_HZ * SSTV_BIT_US / 1000000, /* 2 */
    SSTV_F1_HZ * SSTV_BIT_US / 1000000, /* 3 */
};
#define ARR_SYNC (TX_TIMER_CLK_HZ / SSTV_FSYNC_HZ - 1) /* 15999 */
#define CYCLES_SYNC (SSTV_FSYNC_HZ * SSTV_SYNC_US / 1000000) /* 20 */
#define CYCLES_LEADER (SSTV_F1_HZ * (SSTV_LEADER_US / 1000000)) /* 3000 */

/* Called at the start of a period (CNT just wrapped to 0): writing ARR/CCR without preload
 * applies to the period that has just begun. */
static inline void tx_set_tone(uint32_t arr, uint32_t cycles) {
    LL_TIM_SetAutoReload(TX_TIMER, arr);
    LL_TIM_OC_SetCompareCH1(TX_TIMER, (arr + 1) / 2);
    tx.cycles_left = cycles;
}

static inline void tx_emit_bit(void) {
    int b = sstv_frame_bit(tx.frames + tx.line * SSTV_FRAME_BYTES, tx.bit);
    tx_set_tone(arr_bit[b], cycles_bit[b]);
}

static void tx_isr(void* context) {
    UNUSED(context);
    if(!LL_TIM_IsActiveFlag_UPDATE(TX_TIMER)) return; /* TIM1 shares this vector */
    LL_TIM_ClearFlag_UPDATE(TX_TIMER);
    if(tx.phase == PhaseDone) return;
    if(--tx.cycles_left > 0) return;

    switch(tx.phase) {
    case PhaseLeader:
        tx.phase = PhaseSync;
        tx_set_tone(ARR_SYNC, CYCLES_SYNC);
        break;
    case PhaseSync:
        tx.phase = PhaseBits;
        tx.bit = 0;
        tx_emit_bit();
        break;
    case PhaseBits: {
        tx.bit++;
        if(tx.bit < SSTV_FRAME_BITS) {
            tx_emit_bit();
            break;
        }
        uint16_t finished = tx.line;
        tx.line++;
        if(tx.line < SSTV_HEIGHT) {
            tx.phase = PhaseSync;
            tx_set_tone(ARR_SYNC, CYCLES_SYNC);
            if(tx.cb) tx.cb(tx.ctx, finished, false);
        } else {
            tx.phase = PhaseDone;
            LL_TIM_DisableIT_UPDATE(TX_TIMER);
            LL_TIM_DisableAllOutputs(TX_TIMER);
            LL_TIM_DisableCounter(TX_TIMER);
            if(tx.cb) tx.cb(tx.ctx, finished, true);
        }
        break;
    }
    case PhaseDone:
    default:
        break;
    }
}

bool sstv_tx_start(const uint8_t* image_bits, SstvTxCallback cb, void* ctx) {
    if(tx.running) return false;
    if(!furi_hal_speaker_acquire(1000)) return false;

    tx.frames = malloc(SSTV_HEIGHT * SSTV_FRAME_BYTES);
    for(int y = 0; y < SSTV_HEIGHT; y++) {
        sstv_frame_pack(
            (uint8_t)y, image_bits + y * SSTV_LINE_BYTES, tx.frames + y * SSTV_FRAME_BYTES);
    }
    tx.cb = cb;
    tx.ctx = ctx;
    tx.line = 0;
    tx.bit = 0;
    tx.phase = PhaseLeader;
    tx.cycles_left = CYCLES_LEADER;

    LL_TIM_InitTypeDef init = {0};
    init.Prescaler = 0;
    init.CounterMode = LL_TIM_COUNTERMODE_UP;
    init.Autoreload = arr_bit[1];
    init.ClockDivision = LL_TIM_CLOCKDIVISION_DIV1;
    init.RepetitionCounter = 0;
    LL_TIM_Init(TX_TIMER, &init);

    LL_TIM_OC_InitTypeDef oc = {0};
    oc.OCMode = LL_TIM_OCMODE_PWM1;
    oc.OCState = LL_TIM_OCSTATE_ENABLE;
    oc.OCNState = LL_TIM_OCSTATE_DISABLE;
    oc.CompareValue = (arr_bit[1] + 1) / 2;
    oc.OCPolarity = LL_TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity = LL_TIM_OCPOLARITY_HIGH;
    oc.OCIdleState = LL_TIM_OCIDLESTATE_LOW;
    oc.OCNIdleState = LL_TIM_OCIDLESTATE_LOW;
    LL_TIM_OC_Init(TX_TIMER, LL_TIM_CHANNEL_CH1, &oc);

    LL_TIM_DisableARRPreload(TX_TIMER);
    LL_TIM_OC_DisablePreload(TX_TIMER, LL_TIM_CHANNEL_CH1);

    LL_TIM_ClearFlag_UPDATE(TX_TIMER); /* LL_TIM_Init generated one */
    furi_hal_interrupt_set_isr(FuriHalInterruptIdTim1UpTim16, tx_isr, NULL);
    LL_TIM_EnableIT_UPDATE(TX_TIMER);
    LL_TIM_EnableAllOutputs(TX_TIMER);
    LL_TIM_EnableCounter(TX_TIMER);

    tx.running = true;
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
    furi_hal_speaker_release(); /* stops the timer, disables the TIM16 bus, parks the pin */
    free(tx.frames);
    tx.frames = NULL;
    tx.running = false;
}

bool sstv_tx_is_running(void) {
    return tx.running;
}
