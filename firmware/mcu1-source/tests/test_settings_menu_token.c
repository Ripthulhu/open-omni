#include "settings_menu.h"
#include "dsp_settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A1 regression: the startup output-mode owner and the settings menu must never
 * be handed the same transaction token. Drives the REAL menu producer against
 * the REAL dsp_settings backend (omni_dsp_settings_request is NOT stubbed, so
 * the backend's idempotence/collision guard actually runs). Before the fix the
 * menu's private counter and the mode owner both started at 0x80000001; the
 * backend rejected the menu's first write of a different control as conflicting
 * reuse and the menu wedged. The fix routes both through the shared central
 * source omni_dsp_settings_next_token(). */

static omni_settings_menu_io io;
static uint32_t now=1;
static uint8_t wire[140];static unsigned used;
void omni_settings_menu_bind(omni_settings_menu_io callbacks) {io=callbacks;}
uint32_t omni_ui_milliseconds(void) {return now;}
void omni_ui_settings_status(uint32_t out[15]) {memset(out,0,60);}
bool omni_ui_settings_set(unsigned a,unsigned b,unsigned c,unsigned d) {(void)a;(void)b;(void)c;(void)d;return false;}
bool omni_mcu2_runtime_read(unsigned p,uint32_t out[15]) {(void)p;(void)out;return false;}
bool omni_mcu2_select_input(uint8_t p) {(void)p;return false;}
bool omni_headset_query_busy(void) {return false;}
bool omni_headset_query_request(uint32_t t,unsigned p,uint32_t n) {(void)t;(void)p;(void)n;return false;}
static int tx(void *ctx,uint8_t b) {(void)ctx;assert(used<sizeof(wire));wire[used++]=b;return 1;}
static int drain(void *ctx) {(void)ctx;return 1;}
static void feed(const uint8_t *p,unsigned n) {for(unsigned i=0;i<n;++i) omni_dsp_settings_observe(p[i],now);}

/* Faithful startup OUTPUT_MODE owner, driving the real backend exactly as
 * native_gain_adapter does: SET43 -> ACK -> settle -> GET43 -> ACK + readback
 * -> ACCEPTED, with its token taken from the shared central source. */
static void run_mode_owner(uint32_t token)
{
    uint32_t w[15];
    used=0;
    assert(omni_dsp_settings_request(token,DSP_SETTING_OUTPUT_MODE,(const uint8_t[]){2},1,now));
    omni_dsp_settings_poll(++now,true,(omni_dsp_settings_io){0,tx,drain});
    assert(omni_dsp_settings_status(0,w) && w[4]==DSP_SETTINGS_WAIT);
    assert(used==5u && wire[2]==0x43u && wire[3]==1u && wire[4]==2u); /* SET43 */
    feed((const uint8_t[]){0xdd,3,0x43,0},4);                        /* SET ACK */
    omni_dsp_settings_poll(++now,true,(omni_dsp_settings_io){0,tx,drain});
    assert(omni_dsp_settings_status(0,w) && w[4]==DSP_SETTINGS_VERIFY_QUEUED);
    now+=OMNI_DSP_SETTINGS_MODE_SETTLE_MS+1u;                         /* clear settle guard */
    used=0;omni_dsp_settings_poll(now,true,(omni_dsp_settings_io){0,tx,drain});
    assert(used==4u && wire[2]==0x43u && wire[3]==2u);                /* GET43 */
    feed((const uint8_t[]){0xdd,3,0x43,0},4);                         /* GET ACK */
    feed((const uint8_t[]){0xdb,5,0x43,3,2},5);                       /* readback mode2 */
    omni_dsp_settings_poll(++now,false,(omni_dsp_settings_io){0,tx,drain});
    assert(omni_dsp_settings_status(0,w) && w[4]==DSP_SETTINGS_ACCEPTED &&
           w[2]==token && w[3]==DSP_SETTING_OUTPUT_MODE && !omni_dsp_settings_busy());
}

int main(void)
{
    omni_settings_menu_bind_native();

    /* The startup mode owner takes the first internal token. */
    uint32_t mode_token=omni_dsp_settings_next_token();
    assert(mode_token==0x80000001u && (mode_token&0x80000000u)); /* nonzero, high half */
    run_mode_owner(mode_token);

    /* REAL menu first write of a DIFFERENT control (MIC volume, enum id 2). The
     * menu's old private counter also produced 0x80000001, colliding with the
     * mode owner: the backend returned false and the menu never advanced. Now
     * sharing the central source it MUST be admitted as a fresh transaction. */
    assert(io.write(DSP_SETTING_MIC_VOLUME,8));
    uint32_t w[15];assert(omni_dsp_settings_status(0,w));
    assert(w[3]==DSP_SETTING_MIC_VOLUME); /* the menu's write is what queued */
    assert(w[2]!=mode_token);             /* distinct token: no collision */
    assert(w[2]&0x80000000u);             /* stays in the internal namespace */
    assert(omni_dsp_settings_busy());     /* queued, not rejected */

    puts("settings menu and startup mode owner draw disjoint tokens; menu write admitted");
    return 0;
}
