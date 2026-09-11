#ifndef OMNI_MCU2_RUNTIME_H
#define OMNI_MCU2_RUNTIME_H
#include "mcu2_link.h"
#include <stddef.h>

#define OMNI_MCU2_RUNTIME_RX_BUDGET 128U
#define OMNI_MCU2_RUNTIME_TX_BUDGET 64U
#define OMNI_MCU2_RUNTIME_GAP_MS 100U
#define OMNI_MCU2_SELECT_TIMEOUT_MS 5000U
/* Detect refresh is five seconds; one-second margin permits complete delivery.
 * Expiry describes observation freshness, not active-side confirmation. */
#define OMNI_MCU2_PRESENCE_FRESH_MS 6000U
typedef enum {
    OMNI_MCU2_TX_NONE=0, OMNI_MCU2_TX_VERSION, OMNI_MCU2_TX_DETECT,
    OMNI_MCU2_TX_GAIN, OMNI_MCU2_TX_LIFECYCLE, OMNI_MCU2_TX_CONTROLS,
    OMNI_MCU2_TX_SELECT, OMNI_MCU2_TX_BACKEND_QUERY, OMNI_MCU2_TX_DSP86,
    OMNI_MCU2_TX_COUNT
} omni_mcu2_tx_kind;
typedef enum {
    OMNI_MCU2_SELECT_IDLE=0, OMNI_MCU2_SELECT_QUEUED,
    OMNI_MCU2_SELECT_WAITING, OMNI_MCU2_SELECT_OBSERVED,
    OMNI_MCU2_SELECT_TIMEOUT, OMNI_MCU2_SELECT_FAILED
} omni_mcu2_selection;

typedef struct {
    mcu2_link_io io;
    /* Bind before polling. Gain transmission is not complete until the
     * physical UART is idle; a missing callback never fabricates completion. */
    bool (*tx_idle)(void *context);
    bool started, fault, have_version, have_readiness, have_detect, have_state;
    bool have_setting, ready, have_controls, have_lifecycle, have_dsp86, latest_presence_is_detect;
    uint8_t version[3], readiness, detected_ports, active_mode, backend_setting;
    uint8_t queried_ports, lifecycle, controls[5], secondary_volume, secondary_aux, dsp86;
    uint32_t version_generation, detect_generation, state_generation, controls_generation;
    uint32_t query_sent[2], query_replied[2];
    uint32_t now_ms,detect_ms,state_ms;
    uint32_t tx_bytes, rx_bytes, tx_frames, rx_frames, rejected_frames, ignored_frames;
    uint32_t io_errors, gaps, discarded, last_rx_ms, last_frame_ms, started_ms;
    uint32_t pending, active_since_ms, version_sent_ms, next_detect_ms;
    uint32_t trace_first_tx_ms, trace_last_tx_ms, trace_first_rx_ms, trace_last_rx_ms;
    uint8_t trace_rx[OMNI_MCU2_TRACE_RX_BYTES];
    uint32_t trace_rx_used, trace_rx_truncated;
    uint8_t prefixes[OMNI_MCU2_TX_COUNT][8], lengths[OMNI_MCU2_TX_COUNT];
    uint8_t active_prefix[8], active_length, version_attempts;
    omni_mcu2_tx_kind active;
    uint16_t tx_used, rx_used;
    uint8_t rx_frame[OMNI_MCU2_FRAME_BYTES];
    omni_mcu2_selection selection;
    uint8_t selected_side;
    uint32_t selection_sent_ms, selection_state_generation;
    bool have_gain_desired, have_gain_sent;
    uint8_t gain_desired, gain_sent;
    uint32_t gain_generation, gain_active_generation, gain_sent_generation, gain_sent_ms;
} omni_mcu2_runtime;

/* Single main-loop owner. Fresh UART enable may occur during a peer frame:
 * the receiver validates full 1036-byte envelopes and searches for a new
 * supported header after corruption. No unknown command is transmitted. */
bool omni_mcu2_runtime_init(omni_mcu2_runtime *,mcu2_link_io,uint32_t now);
void omni_mcu2_runtime_poll(omni_mcu2_runtime *,uint32_t now);
void omni_mcu2_runtime_stop(omni_mcu2_runtime *);
bool omni_mcu2_runtime_query(omni_mcu2_runtime *,omni_mcu2_query);
bool omni_mcu2_runtime_lifecycle(omni_mcu2_runtime *,uint8_t state);
bool omni_mcu2_runtime_dsp86(omni_mcu2_runtime *,uint8_t state);
/* Secondary USB source attenuation index 0..12. Coalesced latest desired
 * value; sent status means 1036 bytes plus TXIDLE, not a peer ACK/readback. */
bool omni_mcu2_runtime_gain(omni_mcu2_runtime *,uint8_t index);
bool omni_mcu2_runtime_controls(omni_mcu2_runtime *,uint8_t volume_percent,
    uint8_t balance,uint8_t mic_state,uint8_t mic_percent,uint8_t setting_percent);
bool omni_mcu2_runtime_select(omni_mcu2_runtime *,uint8_t side);
void omni_mcu2_runtime_status(const omni_mcu2_runtime *,unsigned page,uint32_t out[15]);
#endif
