#ifndef OMNI_DSP_METER_H
#define OMNI_DSP_METER_H
#include <stdbool.h>
#include <stdint.h>

#define OMNI_DSP_METER_QUERY_MS 100u
#define OMNI_DSP_METER_TIMEOUT_MS 250u
#define OMNI_DSP_METER_STALE_MS 500u
#define OMNI_DSP_METER_FRAME_BYTES 76u
typedef struct {
    void *context;
    int (*tx)(void *,uint8_t);
    int (*tx_complete)(void *);
} omni_dsp_meter_io;

/* Serialized main-loop owner; UART is supplied by the existing adapter.
 * Only BD045002 is transmitted. acquire follows physical UART acquisition;
 * release follows drain or explicit hardware shutdown. No pins or clocks.
 * Cooperative yield finishes any submitted prefix and physical stop bit,
 * then abandons the response wait. A partial TX/drain timeout latches a fault.
 * Successful replies remain cached across release; age describes receipt,
 * not freshness of the DSP's internal meter samples. */
void omni_dsp_meter_init(void);
void omni_dsp_meter_acquire(uint32_t now);
void omni_dsp_meter_release(uint32_t now);
void omni_dsp_meter_yield(uint32_t now);
void omni_dsp_meter_io_error(uint32_t now);
bool omni_dsp_meter_busy(void);
bool omni_dsp_meter_transport_fault(void);
void omni_dsp_meter_observe(uint8_t byte,uint32_t now);
void omni_dsp_meter_poll(uint32_t now,bool can_start,omni_dsp_meter_io io);

/* Read-only HID60; 60 bytes, little-endian header/status words.
 * page0: version=1,page,flags,phase,cycles,timeouts,txbytes,rxbytes,
 * received_ms,age_ms,generation,invalid,parser_errors,ioerrors,cancellations.
 * flags: bit0 cached-valid,1 receipt-age<500ms and no transport fault,
 * 2 busy,3 transport fault,4 acquired,5 partial RX,6 yielding.
 * phase: 0 idle,1 send,2 physical drain,3 reply wait.
 * page1/2: version,page,generation,48 raw bytes (first 48, then 28+20 zero).
 * Raw DB4C5003 has nine signed 32 BIG-endian L/R pairs, ordered
 * 4C,4D,4A,5E,5F,34,46,47,44. No scale/clamp/freshness is synthesized.
 * Require equal nonzero generations across all pages. Raw+time+generation
 * publish atomically via a two-slot cache; other counters are best-effort
 * transitional main-loop observations. Reader must be a nonreentrant ISR
 * preempting the single writer, or execute serially with the writer. */
bool omni_dsp_meter_read(unsigned page,uint32_t now,uint8_t out[60]);
#endif
