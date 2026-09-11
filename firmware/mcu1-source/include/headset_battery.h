#ifndef OMNI_HEADSET_BATTERY_H
#define OMNI_HEADSET_BATTERY_H
#include <stdbool.h>
#include <stdint.h>

#define OMNI_HEADSET_BATTERY_QUERY_MS 30000u
#define OMNI_HEADSET_LINK_QUERY_MS 1000u
#define OMNI_HEADSET_BATTERY_TIMEOUT_MS 250u
#define OMNI_HEADSET_BATTERY_STALE_MS 90000u
typedef struct {
    void *context;
    int (*tx)(void *,uint8_t);
    int (*tx_complete)(void *);
} omni_headset_battery_io;

/* One serialized main-loop owner; init is startup-only. Observations copy the
 * SAME bytes consumed by gain, or by the bounded idle drain, never a second
 * UART reader. No hardware, allocations, mode changes or volume writes. */
void omni_headset_battery_init(void);
void omni_headset_battery_observe(uint8_t byte,uint32_t now);
void omni_headset_battery_expire(uint32_t now);
bool omni_headset_battery_frame_pending(void);
bool omni_headset_battery_busy(void);
bool omni_headset_battery_transport_fault(void);
/* can_start gates only a new complete query; an active query finishes/drains
 * or reaches its bounded deadline. RF status BD04E402 is polled every 1s even
 * while disconnected. Connected epochs trigger BD05E20201 then BD05E20202;
 * battery refresh remains 30s. This is a status query, not a proven keepalive.
 * The four-byte E4 query must not become the five-byte subscription variant. */
void omni_headset_battery_poll(uint32_t now,bool can_start,omni_headset_battery_io io);
void omni_headset_battery_io_error(void);
void omni_headset_battery_acquire(void);
/* Explicit ownership loss invalidates displayed state and resets framing.
 * Cooperative callers finish an active query before invoking this. */
void omni_headset_battery_release(void);

/* Read-only HID57, pages 0/1, 15 little-endian words:
 * page0: version,page,flags,raw_percent,raw_charge,percent_ms,charge_ms,
 * percent_age_ms,connection_state,query_state,cycles,timeouts,frames,invalid,
 * cancellations. Uninitialized raw/state=255; consult connectionknown because
 * an explicit connection state 255 is disconnected. Absent age=UINT32_MAX.
 * flags: bit0 freshpercent,1 freshcharge,2 connectionknown,3 connected,
 * 4 validcachedpercent,5 validcachedcharge,6 querybusy,7 partialframe,
 * 8 transportfault. Freshness expires at 90s; disconnected hides both.
 * page1: version,page,query_kind(0 RF,1 percent,2 charge),total_tx,total_rx,io_errors,nacks,
 * malformed,expired,percent_frame_length,charge_frame_length,
 * raw_percent[8] in words 11/12,raw_charge[6]+zero[2] in words 13/14.
 * Parser times are main-loop observations, not UART edge timestamps. */
bool omni_headset_battery_status(unsigned page,uint32_t now,uint32_t out[15]);
/* lowbyte:0..100 or255 unknown/stale; byte1:0 unknown,1 notcharging,
 * 2 charging,3 full. Independent of the transmitter charging-slot battery. */
uint32_t omni_headset_battery_display(uint32_t now);
#endif
