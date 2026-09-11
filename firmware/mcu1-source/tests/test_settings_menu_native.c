#include "settings_menu.h"
#include "dsp_settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static omni_settings_menu_io callbacks;
static uint8_t cache[20][36],request[128];
static unsigned lengths[20],request_id,request_length,preset_id,preset_value;
static uint32_t settings[15]={1,1,5,0,0},backend_token,backend_phase,backend_flags;
static bool busy;
void omni_settings_menu_bind(omni_settings_menu_io io) {callbacks=io;}
uint32_t omni_ui_milliseconds(void) {return 123;}
void omni_ui_settings_status(uint32_t out[15]) {memcpy(out,settings,60);}
bool omni_ui_settings_set(unsigned t,unsigned b,unsigned s,unsigned h)
{settings[1]=t;settings[2]=b;settings[3]=s;settings[4]=h;return true;}
bool omni_mcu2_runtime_read(unsigned page,uint32_t out[15])
{memset(out,0,60);if(!page){out[1]=20;out[12]=0x201;}else out[7]=3;return true;}
bool omni_mcu2_select_input(uint8_t side) {return side<2;}
bool omni_dsp_settings_busy(void) {return busy;}
bool omni_dsp_settings_value(unsigned id,unsigned page,uint8_t out[60])
{assert(page==0 && id<20);memset(out,0,60);uint32_t h[6]={1,id,0,lengths[id],lengths[id]?5u:0u,1};memcpy(out,h,24);memcpy(out+24,cache[id],36);return true;}
bool omni_dsp_settings_request(uint32_t token,unsigned id,const uint8_t *p,size_t n,uint32_t now)
{assert(now==123 && n<=128);backend_token=token;request_id=id;request_length=(unsigned)n;memcpy(request,p,n);return true;}
bool omni_dsp_settings_status(unsigned page,uint32_t out[15])
{assert(!page);memset(out,0,60);out[2]=backend_token;out[4]=backend_phase;out[5]=backend_flags;return true;}
size_t omni_dsp_settings_eq_preset(unsigned id,unsigned preset,uint8_t out[128])
{preset_id=id;preset_value=preset;memset(out,0,128);out[0]=(uint8_t)preset;return id==12?128:78;}
int main(void)
{
    omni_settings_menu_bind_native();unsigned value;
    assert(callbacks.read(32,&value) && value==5);
    assert(callbacks.write(32,8));assert(settings[1]==1 && settings[2]==8 && settings[3]==0);
    assert(!callbacks.read(2,&value));
    assert(!callbacks.write(9,6)); /* Unknown E3 siblings cannot be guessed. */
    lengths[9]=3;cache[9][0]=1;cache[9][1]=3;cache[9][2]=2;
    assert(callbacks.write(9,6));assert(request_id==9 && request_length==3);
    assert(request[0]==1 && request[1]==6 && request[2]==2);
    assert(callbacks.write(3,0));assert(request[0]==0 && request[1]==1);
    assert(callbacks.write(3,7));assert(request[0]==1 && request[1]==7);
    assert(callbacks.write(11,5) && request[0]==30);
    assert(!callbacks.write(11,7));
    assert(callbacks.write(13,8));assert(preset_id==13 && preset_value==9 && request_length==78);
    lengths[13]=78;cache[13][0]=8;assert(callbacks.read(13,&value) && value==9);
    cache[13][0]=9;assert(callbacks.read(13,&value) && value==8);
    busy=true;assert(!callbacks.write(2,4));busy=false;
    backend_flags=1;assert(!strcmp(callbacks.status(),"SENDING"));
    backend_flags=0;backend_phase=DSP_SETTINGS_ACCEPTED;assert(!strcmp(callbacks.status(),"SENT"));
    backend_phase=DSP_SETTINGS_TIMEOUT;assert(!strcmp(callbacks.status(),"SETTING FAILED"));
    assert(callbacks.read(36,&value) && value==1);assert(callbacks.write(36,0));
    assert(!strcmp(callbacks.status(),"APPLIED"));
    puts("native menu payloads, sibling preservation and status pass");return 0;
}
