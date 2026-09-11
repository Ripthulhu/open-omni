#ifndef OMNI_UI_IDLE_H
#define OMNI_UI_IDLE_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t last_activity;
    uint32_t timeout_ms;
    int16_t db;
    uint8_t mute;
    bool observed;
} omni_ui_idle;

static inline void omni_ui_idle_init(omni_ui_idle *s, uint32_t now)
{
    *s=(omni_ui_idle){.last_activity=now,.timeout_ms=60000u};
}

/* Stock UI index, independently distinct from the headset auto-off timer. */
static inline bool omni_ui_idle_timeout(omni_ui_idle *s,unsigned index)
{
    static const uint8_t minutes[]={0,1,5,10,15,30,60};
    if(!s || index>=sizeof(minutes)) return false;
    s->timeout_ms=(uint32_t)minutes[index]*60000u;
    return true;
}

/* Only user input or changed volume/mute extends visibility. Background
 * status, redraws, HID polling and display transfer retries are not activity. */
static inline bool omni_ui_idle_update(omni_ui_idle *s, uint32_t now,
                                      int16_t db, uint8_t mute, bool input)
{
    if(input || !s->observed || db!=s->db || mute!=s->mute) s->last_activity=now;
    s->observed=true; s->db=db; s->mute=mute;
    return !s->timeout_ms || (uint32_t)(now-s->last_activity)<s->timeout_ms;
}
#endif
