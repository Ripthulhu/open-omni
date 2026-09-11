#ifndef OMNI_DISPLAY_H
#define OMNI_DISPLAY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Single-owner cooperative driver. Backend must copy/retain TX bytes until
 * busy() reports completion, and must never block in these callbacks.
 * GPIO arguments name recovered pins, not unverified reset/power roles. */
typedef struct {
    void *context;
    void (*pins)(void *, bool p0_15, bool p1_8);
    int (*start)(void *, bool data, const uint8_t *, size_t);
    int (*busy)(void *); /* 1 active, 0 physically completed, -1 failure */
    void (*cancel)(void *);
} omni_display_io;
typedef enum { OMNI_DISPLAY_START, OMNI_DISPLAY_WAIT_1,
    OMNI_DISPLAY_WAIT_2, OMNI_DISPLAY_WAIT_3, OMNI_DISPLAY_INIT,
    OMNI_DISPLAY_CLEAR, OMNI_DISPLAY_ON, OMNI_DISPLAY_SETTLE,
    OMNI_DISPLAY_READY, OMNI_DISPLAY_RANGE, OMNI_DISPLAY_PIXELS,
    OMNI_DISPLAY_FAILED, OMNI_DISPLAY_CONTRAST } omni_display_state;
typedef struct {
    omni_display_io io;
    omni_display_state state;
    uint32_t since;
    bool active;
    uint8_t command[2],contrast;
    uint8_t frame[1024];
} omni_display;
bool omni_display_init(omni_display *, omni_display_io, uint32_t now_ms);
void omni_display_poll(omni_display *, uint32_t now_ms);
/* Accept only when ready; copy makes caller lifetime independent of DMA. */
bool omni_display_present(omni_display *, const uint8_t frame[1024]);
/* Stock1..10 -> SPI81,level*23. Queued only at a frame boundary. */
bool omni_display_brightness(omni_display *,unsigned level);
#endif
