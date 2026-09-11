#include "sof_capture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t ahb0,ahb1,reset0,reset1,selector,route,nvic,timer[32];
    uint32_t frame_before,frame_after,md,fro,reads,writes,usb_reads,md_reads,fro_reads;
    uint32_t fail_read,fail_write,log[160][2];
    bool corrupt_selector;
} fixture_t;

static bool read_reg(void *context,uint32_t a,uint32_t *v)
{
    fixture_t *f=context;
    if(++f->reads==f->fail_read) return false;
    switch(a) {
    case 0x40000200: *v=f->ahb0; break;
    case 0x40000204: *v=f->ahb1; break;
    case 0x40000100: *v=f->reset0; break;
    case 0x40000104: *v=f->reset1; break;
    case 0x4000026c: *v=f->selector; break;
    case 0x40006020: assert(f->ahb0&(1u<<11)); *v=f->route; break;
    case 0xe000e100: *v=f->nvic; break;
    case 0x40084004: *v=(f->usb_reads++&1u)?f->frame_after:f->frame_before; break;
    case 0x40000590: ++f->md_reads; *v=f->md; break;
    case 0x40013010: ++f->fro_reads; *v=f->fro; break;
    default:
        assert(a>=0x40008000 && a<=0x40008074 && (a&3u)==0);
        assert(f->ahb1&(1u<<26)); *v=f->timer[(a-0x40008000)/4]; break;
    }
    return true;
}

static bool write_reg(void *context,uint32_t a,uint32_t v)
{
    fixture_t *f=context;
    assert(f->writes<160u);
    f->log[f->writes][0]=a; f->log[f->writes][1]=v;
    if(++f->writes==f->fail_write) return false;
    /* Independent safety oracle: only CTIMER0 and its own routing/gates can be
     * written. No PLL/FRO/audio/USB/NVIC writes are accepted by this backend. */
    switch(a) {
    case 0x40000220: assert(v==(1u<<11)); f->ahb0|=v; break;
    case 0x40000224: assert(v==(1u<<26)); f->ahb1|=v; break;
    case 0x40000240: assert(v==(1u<<11)); f->ahb0&=~v; break;
    case 0x40000244: assert(v==(1u<<26)); f->ahb1&=~v; break;
    case 0x40000124:
        assert(v==(1u<<26)); f->reset1|=v; memset(f->timer,0,sizeof(f->timer)); break;
    case 0x40000144: assert(v==(1u<<26)); f->reset1&=~v; break;
    case 0x4000026c: f->selector=(f->corrupt_selector && (v==1u || v==3u))?7u:v; break;
    case 0x40006020: assert(f->ahb0&(1u<<11)); f->route=v; break;
    case 0x40008000: case 0x40008004: case 0x40008008: case 0x4000800c:
    case 0x40008010: case 0x40008014: case 0x40008028: case 0x40008070:
    case 0x40008074:
        assert(f->ahb1&(1u<<26)); f->timer[(a-0x40008000)/4]=v; break;
    default: assert(!"write outside observation-only peripheral scope");
    }
    return true;
}

static void prepare(fixture_t *f,omni_sof_capture_t *s)
{
    memset(f,0,sizeof(*f));
    f->ahb0=0x180u; f->ahb1=1u<<25; /* preserve USB device clock */
    f->selector=7u; f->route=0x1fu; f->md=0xf5c28f5cu;
    omni_sof_capture_ops_t ops={f,read_reg,write_reg};
    omni_sof_capture_init(s,&ops);
}
static void restored(const fixture_t *f)
{
    assert(f->ahb0==0x180u && f->ahb1==(1u<<25));
    assert(f->selector==7u && f->route==0x1fu);
    assert(f->timer[1]==0u && f->nvic==0u);
}

static void sample_bound_and_records(void)
{
    fixture_t f; omni_sof_capture_t s; uint32_t page[15]; prepare(&f,&s);
    assert(omni_sof_capture_start(&s,3600000u));
    assert(f.selector==1u && f.route==0x14u && f.timer[1]==1u);
    assert(f.timer[0x0c/4]==0u && f.timer[0x70/4]==0u);
    assert(f.timer[0x28/4]==1u); /* rise only; capture interrupt disabled */
    uint32_t writes=f.writes;
    assert(!omni_sof_capture_start(&s,3600000u) && f.writes==writes);
    omni_sof_capture_poll(&s,3600000u); /* reset CR0 is not a sample */
    assert(atomic_load(&s.count)==0u);
    for(uint32_t i=0;i<64u;++i) {
        f.frame_before=f.frame_after=0x800u|((2040u+i)&2047u);
        f.timer[0x2c/4]=49152u*(i+1u);
        f.md=0xf5c28f5cu+i;
        omni_sof_capture_poll(&s,3600001u+i);
        assert(atomic_load(&s.count)==i+1u);
        assert(s.samples[i][0]==((2040u+i)&2047u));
        assert(s.samples[i][1]==49152u*(i+1u) && s.samples[i][3]==f.md);
        if(i<63u) {
            omni_sof_capture_poll(&s,3600001u+i); /* no duplicate sample */
            assert(atomic_load(&s.count)==i+1u);
        }
    }
    assert(s.state==OMNI_SOF_DONE && s.error==OMNI_SOF_OK);
    restored(&f);
    writes=f.writes; omni_sof_capture_poll(&s,3600300u); assert(f.writes==writes);
    omni_sof_capture_trace(&s,62u,page);
    assert(page[3]==64u && page[4]==62u && page[6]==49152u*63u);
    assert(page[10]==49152u*64u);
    omni_sof_capture_trace(&s,UINT32_MAX,page);
    for(unsigned i=5;i<15u;++i) assert(page[i]==0u);
    omni_sof_capture_status(&s,page);
    assert(page[0]==1u && page[1]==OMNI_SOF_DONE && page[3]==64u && page[7]==64u);
}

static void wraps_and_timeout(void)
{
    fixture_t f; omni_sof_capture_t s; prepare(&f,&s);
    assert(omni_sof_capture_start(&s,UINT32_MAX-100u));
    f.frame_before=2047u; f.frame_after=0u; f.timer[0x2c/4]=0xfffffff0u;
    omni_sof_capture_poll(&s,UINT32_MAX-99u);
    assert(s.crossed==1u && s.samples[0][0]==2047u && s.samples[0][2]==0u);
    f.frame_before=f.frame_after=0u; f.timer[0x2c/4]=0u;
    omni_sof_capture_poll(&s,0u);
    assert(atomic_load(&s.count)==2u && s.samples[1][1]-s.samples[0][1]==16u);
    omni_sof_capture_poll(&s,148u); assert(s.state==OMNI_SOF_RUNNING);
    omni_sof_capture_poll(&s,149u);
    assert(s.state==OMNI_SOF_FAILED && s.error==OMNI_SOF_TIMEOUT);
    assert(s.elapsed_ms==250u); restored(&f);
    prepare(&f,&s); assert(omni_sof_capture_start(&s,900u));
    omni_sof_capture_poll(&s,1149u); assert(atomic_load(&s.count)==0u);
    omni_sof_capture_poll(&s,1150u); assert(s.error==OMNI_SOF_TIMEOUT); restored(&f);
}

static void ownership_and_failure(void)
{
    fixture_t f; omni_sof_capture_t s;
    for(unsigned which=0;which<3u;++which) {
        prepare(&f,&s);
        if(which==0) f.ahb1|=1u<<26;
        if(which==1) f.nvic|=1u<<10;
        if(which==2) f.reset0|=1u<<11;
        assert(!omni_sof_capture_start(&s,1000u));
        assert(s.error==OMNI_SOF_OWNERSHIP && f.writes==0u);
    }
    for(unsigned failure=1;failure<=10u;++failure) {
        prepare(&f,&s); f.fail_read=failure;
        assert(!omni_sof_capture_start(&s,1000u)); assert(s.error==OMNI_SOF_IO);
        assert(!(f.ahb1&(1u<<26)) && f.route==0x1fu && f.selector==7u);
    }
    prepare(&f,&s); assert(omni_sof_capture_start(&s,1000u));
    unsigned setup_writes=f.writes; omni_sof_capture_stop(&s);
    for(unsigned failure=1;failure<=setup_writes;++failure) {
        prepare(&f,&s); f.fail_write=failure;
        assert(!omni_sof_capture_start(&s,1000u)); assert(s.error==OMNI_SOF_IO);
        assert(!(f.ahb1&(1u<<26)) && !(f.reset1&(1u<<26)));
        assert(f.route==0x1fu && f.selector==7u && f.ahb0==0x180u);
    }
    prepare(&f,&s); f.corrupt_selector=true;
    assert(!omni_sof_capture_start(&s,1000u));
    assert(s.error==OMNI_SOF_CONFIG); restored(&f);
    prepare(&f,&s); f.reset1=1u<<26; f.ahb0|=1u<<11;
    assert(omni_sof_capture_start(&s,1000u)); omni_sof_capture_stop(&s);
    assert(s.state==OMNI_SOF_STOPPED && f.reset1==(1u<<26));
    assert(f.ahb0==(0x180u|(1u<<11)) && f.ahb1==(1u<<25));
    assert(f.route==0x1fu && f.selector==7u);
    prepare(&f,&s); assert(omni_sof_capture_start(&s,1000u));
    f.fail_read=f.reads+2u; omni_sof_capture_poll(&s,1001u);
    assert(s.error==OMNI_SOF_IO); restored(&f);
}
static void fro_source_and_isolation(void)
{
    fixture_t f; omni_sof_capture_t s; uint32_t page[15]; prepare(&f,&s);
    assert(!omni_sof_capture_start_mode(&s,0u,(omni_sof_capture_clock_t)2u));
    assert(s.error==OMNI_SOF_CONFIG && !f.reads && !f.writes);
    assert(omni_sof_capture_start_mode(&s,1000u,OMNI_SOF_CLOCK_FRO96));
    assert(f.selector==3u && f.timer[0x28/4]==1u);
    for(uint32_t i=0;i<64u;++i) {
        f.frame_before=f.frame_after=(2040u+i)&2047u;
        /* A later counter wrap to zero remains valid. Trim is observed raw,
         * including changing-read status; the probe never modifies it. */
        f.timer[0x2c/4]=UINT32_MAX-95999u+i*96000u;
        f.fro=0x4176d3a0u+((i&3u)<<16);
        omni_sof_capture_poll(&s,1001u+i);
        assert(atomic_load(&s.count)==i+1u && s.samples[i][3]==f.fro);
    }
    assert(s.state==OMNI_SOF_DONE && f.fro_reads==64u && !f.md_reads);
    restored(&f);
    omni_sof_capture_status(&s,page);
    assert(page[0]==2u && page[12]==3u && page[13]==0x40013010u);
    omni_sof_capture_trace(&s,0u,page);
    assert(page[0]==2u && page[8]==0x4176d3a0u && page[13]==3u);
    assert(page[10]-page[6]==96000u);
    omni_sof_capture_trace(&s,UINT32_MAX,page);
    for(unsigned i=5;i<13u;++i) assert(page[i]==0u);
    assert(page[13]==3u && page[14]==0u);
    /* Switching back deliberately restores the exact legacy ABI. */
    assert(omni_sof_capture_start(&s,2000u));
    omni_sof_capture_status(&s,page);
    assert(page[0]==1u && !page[12] && !page[13]);
    omni_sof_capture_stop(&s); restored(&f);
    prepare(&f,&s); f.corrupt_selector=true;
    assert(!omni_sof_capture_start_mode(&s,0u,OMNI_SOF_CLOCK_FRO96));
    assert(s.error==OMNI_SOF_CONFIG); restored(&f);
    prepare(&f,&s);
    assert(omni_sof_capture_start_mode(&s,UINT32_MAX-100u,OMNI_SOF_CLOCK_FRO96));
    omni_sof_capture_poll(&s,149u);
    assert(s.error==OMNI_SOF_TIMEOUT); restored(&f);
    prepare(&f,&s);
    assert(omni_sof_capture_start_mode(&s,0u,OMNI_SOF_CLOCK_FRO96));
    f.fail_read=f.reads+4u; omni_sof_capture_poll(&s,1u);
    assert(s.error==OMNI_SOF_IO); restored(&f);
}
int main(void)
{
    sample_bound_and_records(); wraps_and_timeout(); ownership_and_failure();
    fro_source_and_isolation();
    puts("SOF capture: bounded records, wrap, ownership, restoration and I/O failure tests passed");
    return 0;
}
