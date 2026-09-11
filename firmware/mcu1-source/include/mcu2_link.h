#ifndef OMNI_MCU2_LINK_H
#define OMNI_MCU2_LINK_H
#include <stdbool.h>
#include <stdint.h>

#define OMNI_MCU2_FRAME_BYTES 1036U
#define OMNI_MCU2_QUERY_TIMEOUT_MS 3000U
#define OMNI_MCU2_TRACE_RX_BYTES 64U

typedef struct {
    void *context;
    /* Return 1 for one transferred byte, 0 for unavailable, -1 for error. */
    int (*tx)(void *, uint8_t);
    int (*rx)(void *, uint8_t *);
} mcu2_link_io;

typedef enum {
    OMNI_MCU2_IDLE = 0,
    OMNI_MCU2_RUNNING = 1,
    OMNI_MCU2_SUCCESS = 2,
    OMNI_MCU2_TIMEOUT = 3,
    OMNI_MCU2_IO_ERROR = 4,
    OMNI_MCU2_CANCELLED = 5
} omni_mcu2_phase;

typedef enum {
    OMNI_MCU2_QUERY_VERSION = 0,
    OMNI_MCU2_QUERY_DETECT = 1
} omni_mcu2_query;

typedef struct {
    mcu2_link_io io;
    omni_mcu2_phase phase;
    uint32_t start_ms, attempts, tx_bytes, rx_bytes;
    uint32_t received_frames, rejected_frames;
    uint16_t frame_used;
    uint8_t reply[3]; /* Opaque stock bytes, valid only in SUCCESS. */
    uint8_t header[7];
    bool padding_nonzero;
    /* Elapsed main-loop observation times, not wire/ISR timestamps. A TX
     * timestamp records FIFO acceptance, not transmission of the stop bit.
     * UINT32_MAX means no byte was observed. Preserved at terminal states. */
    uint32_t trace_first_tx_ms, trace_last_tx_ms;
    uint32_t trace_first_rx_ms, trace_last_rx_ms;
    uint8_t trace_rx[OMNI_MCU2_TRACE_RX_BYTES];
    uint32_t trace_rx_used, trace_rx_truncated;
    omni_mcu2_query query;
} omni_mcu2_link;

/* Single main-loop owner. Backend must establish a fresh frame boundary before
 * each start (bounded flush or fresh UART enable); this module cannot correlate
 * a stale reply because the recovered protocol has no transaction identifier.
 * A partial TX timeout/cancel requires backend resynchronization before reuse.
 */
bool omni_mcu2_link_init(omni_mcu2_link *, mcu2_link_io);
bool omni_mcu2_link_start(omni_mcu2_link *, uint32_t now_ms);
bool omni_mcu2_link_start_query(omni_mcu2_link *, uint32_t now_ms, omni_mcu2_query);
void omni_mcu2_link_poll(omni_mcu2_link *, uint32_t now_ms);
void omni_mcu2_link_cancel(omni_mcu2_link *);
#endif
