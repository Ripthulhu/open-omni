#ifndef OMNI_SOF_CAPTURE_H
#define OMNI_SOF_CAPTURE_H
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

#define OMNI_SOF_CAPTURE_SAMPLES 64u
#define OMNI_SOF_CAPTURE_TIMEOUT_MS 250u
typedef enum {
    OMNI_SOF_IDLE, OMNI_SOF_RUNNING, OMNI_SOF_DONE, OMNI_SOF_FAILED,
    OMNI_SOF_STOPPED
} omni_sof_capture_state_t;
typedef enum {
    OMNI_SOF_OK, OMNI_SOF_IO, OMNI_SOF_OWNERSHIP, OMNI_SOF_TIMEOUT,
    OMNI_SOF_CONFIG
} omni_sof_capture_error_t;
typedef enum {
    OMNI_SOF_CLOCK_PLL0=1, OMNI_SOF_CLOCK_FRO96=3
} omni_sof_capture_clock_t;
typedef struct {
    void *context;
    bool (*read)(void *, uint32_t address, uint32_t *value);
    bool (*write)(void *, uint32_t address, uint32_t value);
} omni_sof_capture_ops_t;
typedef struct {
    omni_sof_capture_ops_t ops;
    volatile omni_sof_capture_state_t state;
    omni_sof_capture_error_t error;
    uint32_t samples[OMNI_SOF_CAPTURE_SAMPLES][4];
    _Atomic uint32_t count; /* Complete records published to USB diagnostics. */
    uint32_t polls, crossed, duplicates, start_ms, elapsed_ms;
    uint32_t previous_capture, saved_selector, saved_route, saved_reset;
    omni_sof_capture_clock_t clock_selector;
    bool have_capture, timer_enabled, mux_enabled_here, route_saved;
} omni_sof_capture_t;

/* Main loop only; request dispatch from USB must defer these calls. Init once.
 * start rejects a timer whose peripheral clock or NVIC IRQ is already enabled.
 * poll does one bounded snapshot; no IRQ, PLL, FRO or audio register writes.
 * Each record is {USB frame before, CTIMER0 CR0, USB frame after, live PLL0 MD}.
 * Equal frame fields do not prove edge phase alignment; retain raw evidence. */
void omni_sof_capture_init(omni_sof_capture_t *, const omni_sof_capture_ops_t *);
bool omni_sof_capture_start(omni_sof_capture_t *, uint32_t now_ms);
/* Explicit timer source, independently of audio running. PLL0 retains ABI1.
 * FRO96 uses ABI2; each record's fourth word is raw FRO192M_CTRL, not PLL MD.
 * This measures average FRO cycles between host SOF edges; it cannot establish
 * instantaneous USB bit timing or an independent crystal's absolute accuracy.
 * Invalid selections fail without writing any peripheral register. */
bool omni_sof_capture_start_mode(omni_sof_capture_t *,uint32_t now_ms,
                                omni_sof_capture_clock_t);
void omni_sof_capture_poll(omni_sof_capture_t *, uint32_t now_ms);
void omni_sof_capture_stop(omni_sof_capture_t *);
/* ABI1: version,state,error,count,polls,crossed,duplicates,elapsed_ms,
 * capacity,timeout_ms,saved_selector,saved_route,0,0,0. */
/* ABI2 FRO96: status[12]=selector3, [13]=observed register0x40013010;
 * trace[13]=selector3. Other fields retain the ABI1 layout. */
void omni_sof_capture_status(const omni_sof_capture_t *, uint32_t out[15]);
/* ABI1: version,state,error,count,index,record[index][4],record[index+1][4],0,0.
 * Out of range records are zero; index is a record index, not a page number. */
void omni_sof_capture_trace(const omni_sof_capture_t *, uint32_t index, uint32_t out[15]);
omni_sof_capture_ops_t omni_sof_capture_mmio_ops(void);
#endif
