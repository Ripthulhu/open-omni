#ifndef OMNI_DSP_LINK_H
#define OMNI_DSP_LINK_H
#include "interchip.h"
#include "mcu2_link.h" /* Shared nonblocking byte I/O callback contract. */

typedef enum {
    OMNI_DSP_IDLE = 0,
    OMNI_DSP_RUNNING = 1,
    OMNI_DSP_SUCCESS = 2,
    OMNI_DSP_TIMEOUT = 3,
    OMNI_DSP_IO_ERROR = 4,
    OMNI_DSP_CANCELLED = 5
} omni_dsp_phase;

typedef struct {
    mcu2_link_io io;
    omni_link_parser parser;
    omni_dsp_phase phase;
    uint32_t start_ms, attempts, tx_bytes, rx_bytes, received_frames, rejected_frames;
    uint16_t reply_length, first_unexpected_length;
    uint8_t reply[3];
    uint8_t reply_tail[6]; /* Wire bytes 7..12; meanings not established. */
    uint8_t first_unexpected[8];
} omni_dsp_link;

/* Explicit status query only: BD04E102, no mode/reset/volume commands.
 * Backend establishes a fresh RX boundary before start. Protocol has no
 * transaction ID; a stale matching response cannot be distinguished later.
 * Accepts the observed 13-byte DB0DE103 response. Stock consumes bytes4..12;
 * no seven-byte status form is established. Reply fields preserve wire order.
 * One main-loop owner; 250ms overall and 20ms inter-byte policy deadlines.
 */
bool omni_dsp_link_init(omni_dsp_link *, mcu2_link_io);
bool omni_dsp_link_start(omni_dsp_link *, uint32_t now_ms);
void omni_dsp_link_poll(omni_dsp_link *, uint32_t now_ms);
void omni_dsp_link_cancel(omni_dsp_link *);
#endif
