#include "board_clock.h"
#include "audio_clock.h"
#include "audio_clock_startup.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t regs[OMNI_AC_REGISTER_COUNT], power;
    uint32_t write_values[32]; omni_audio_clock_register_t write_regs[32];
    unsigned reads,writes,guards,calls;
    int fail_read,fail_write;
    bool owned,xo_ready,pll_lock,div_ready;
} fixture_t;

static bool guard(void *context)
{
    fixture_t *f=context; ++f->calls; ++f->guards; return f->owned;
}
static bool read_reg(void *context,omni_audio_clock_register_t reg,uint32_t *value)
{
    fixture_t *f=context; ++f->calls;
    assert(reg<OMNI_AC_REGISTER_COUNT && reg!=OMNI_AC_POWER_SET && reg!=OMNI_AC_POWER_CLEAR);
    if ((int)f->reads++==f->fail_read) return false;
    *value=f->regs[reg];
    if (reg==OMNI_AC_XO_STATUS) *value=f->xo_ready?1u:0u;
    if (reg==OMNI_AC_PLL0STAT) *value=f->pll_lock && !(f->power&0x800200u)?1u:0u;
    if ((reg==OMNI_AC_PLL0DIV || reg==OMNI_AC_MCLKDIV) && !f->div_ready) *value|=0x80000000u;
    return true;
}
static bool write_reg(void *context,omni_audio_clock_register_t reg,uint32_t value)
{
    fixture_t *f=context; ++f->calls;
    /* The component can never write CPU/USB/FRO, pin-direction, Flexcomm
     * selector, or register-access clock gates. */
    assert(reg==OMNI_AC_XO_CTRL || reg==OMNI_AC_CLOCK_CTRL || reg==OMNI_AC_MCLKSEL ||
        reg==OMNI_AC_POWER_SET || reg==OMNI_AC_POWER_CLEAR || reg==OMNI_AC_PLL0SEL ||
        reg==OMNI_AC_PLL0CTRL || reg==OMNI_AC_PLL0NDEC || reg==OMNI_AC_PLL0PDEC ||
        reg==OMNI_AC_PLL0SSCG0 || reg==OMNI_AC_PLL0SSCG1 || reg==OMNI_AC_PLL0DIV || reg==OMNI_AC_MCLKDIV);
    if ((int)f->writes==f->fail_write) return false;
    assert(f->writes<32u);
    f->write_regs[f->writes]=reg; f->write_values[f->writes++]=value;
    if (reg==OMNI_AC_POWER_SET) { assert(!(value&~0x800200u)); f->power|=value; }
    else if (reg==OMNI_AC_POWER_CLEAR) { assert(!(value&~0x900300u)); f->power&=~value; }
    else f->regs[reg]=value;
    return true;
}
static void init(fixture_t *f,omni_audio_clock_t *clock)
{
    memset(f,0,sizeof(*f)); f->fail_read=f->fail_write=-1;
    f->owned=f->xo_ready=f->pll_lock=f->div_ready=true;
    f->regs[OMNI_AC_MAINCLKA]=OMNI_CORE_MAIN_A;
    f->regs[OMNI_AC_USB0SEL]=3; f->regs[OMNI_AC_USB0DIV]=1;
    f->regs[OMNI_AC_FRO192M_CTRL]=0x76543210u;
    f->regs[OMNI_AC_XO_CTRL]=0xe0c3459bu; /* Includes reserved readback noise. */
    f->regs[OMNI_AC_CLOCK_CTRL]=0xf0000103u;
    f->regs[OMNI_AC_MCLKSEL]=7;
    f->power=0x10901300u;
    omni_audio_clock_ops_t ops={f,guard,read_reg,write_reg};
    assert(omni_audio_clock_init(clock,&ops));
}
static void begin(fixture_t *f,omni_audio_clock_t *clock,uint32_t now)
{
    assert(omni_audio_clock_begin(clock,16000000u,f->regs[OMNI_AC_XO_CTRL],true,now));
    assert(f->calls==0u); /* begin never enters the hardware backend. */
}
static void poll(fixture_t *f,omni_audio_clock_t *clock,uint32_t now)
{
    unsigned before=f->calls;
    omni_audio_clock_poll(clock,now);
    assert(f->calls-before<=2u);
}
static uint32_t until(fixture_t *f,omni_audio_clock_t *clock,
                       omni_audio_clock_state_t state,uint32_t now)
{
    for (unsigned n=0;n<510u && clock->state!=state;++n,++now) {
        assert(clock->state<OMNI_AUDIO_CLOCK_LOCAL_READY);
        poll(f,clock,now);
    }
    assert(clock->state==state); return now;
}

static uint32_t startup_uptime, startup_tick_step;
static unsigned startup_watchdog_calls;
static uint32_t startup_milliseconds(void)
{
    uint32_t now=startup_uptime;
    startup_uptime+=startup_tick_step;
    return now;
}
static void startup_watchdog(void) { ++startup_watchdog_calls; }

static void test_ready_but_unconfigured_xo(void)
{
    fixture_t f; omni_audio_clock_t clock;
    bool changed=false;
    /* Actual bb599059 cold-start capture: XO_READY=1 did not imply the
     * board's required control configuration or system-output enable. */
    init(&f,&clock); f.pll_lock=false;
    f.regs[OMNI_AC_XO_CTRL]=0x0021428au;
    f.regs[OMNI_AC_CLOCK_CTRL]=0xc1u;
    f.regs[OMNI_AC_FC0SEL]=3u; f.regs[OMNI_AC_FC2SEL]=7u;
    f.regs[OMNI_AC_PLL0SEL]=7u;
    f.regs[OMNI_AC_PLL0DIV]=f.regs[OMNI_AC_MCLKDIV]=0x40000000u;
    assert(omni_audio_clock_startup_prepare_xo(&clock.ops,&changed)==OMNI_AUDIO_CLOCK_ERROR_NONE);
    assert(changed && f.writes==4u && f.xo_ready);
    assert(f.regs[OMNI_AC_XO_CTRL]==0x01c3459au && f.regs[OMNI_AC_CLOCK_CTRL]==0xe1u);
    assert(f.regs[OMNI_AC_PLL0SEL]==7u); /* preparation does not program the PLL */
    startup_uptime=3600000u; startup_tick_step=1u; startup_watchdog_calls=0u;
    assert(omni_audio_clock_startup_run(&clock,0x01c3459au,startup_milliseconds,startup_watchdog));
    assert(clock.state==OMNI_AUDIO_CLOCK_LOCAL_READY && f.writes==26u);

    /* Already correct settings need no ownership claim or writes. This is
     * required for read-only reuse of a previously owned, settled clock. */
    init(&f,&clock); f.owned=false;
    f.regs[OMNI_AC_XO_CTRL]=0x01c3459au; f.regs[OMNI_AC_CLOCK_CTRL]=0xe1u;
    assert(omni_audio_clock_startup_prepare_xo(&clock.ops,&changed)==OMNI_AUDIO_CLOCK_ERROR_NONE);
    assert(!changed && f.writes==0u && f.guards==0u);
    f.regs[OMNI_AC_XO_CTRL]=0x0021428au;
    assert(omni_audio_clock_startup_prepare_xo(&clock.ops,&changed)==OMNI_AUDIO_CLOCK_ERROR_OWNERSHIP);
    assert(!changed && f.writes==0u);

    /* CPU/USB routing must be proven safe before changing a live oscillator. */
    const omni_audio_clock_register_t unsafe[]={OMNI_AC_MAINCLKA,OMNI_AC_MAINCLKB,
        OMNI_AC_AHBDIV,OMNI_AC_USB0SEL,OMNI_AC_USB0DIV,OMNI_AC_MCLKIO,OMNI_AC_MCLKSEL};
    for (unsigned i=0;i<sizeof(unsafe)/sizeof(unsafe[0]);++i) {
        init(&f,&clock); f.regs[OMNI_AC_XO_CTRL]=0x0021428au;
        f.regs[unsafe[i]]=unsafe[i]==OMNI_AC_USB0SEL || unsafe[i]==OMNI_AC_USB0DIV?0u:1u;
        omni_audio_clock_error_t error=omni_audio_clock_startup_prepare_xo(&clock.ops,&changed);
        assert(error==(unsafe[i]==OMNI_AC_MCLKSEL?OMNI_AUDIO_CLOCK_ERROR_OWNERSHIP:
            OMNI_AUDIO_CLOCK_ERROR_PREREQUISITE));
        assert(!changed && f.writes==0u);
    }
    for (int failure=0;failure<4;++failure) {
        init(&f,&clock); f.regs[OMNI_AC_XO_CTRL]=0x0021428au; f.fail_write=failure;
        assert(omni_audio_clock_startup_prepare_xo(&clock.ops,&changed)==OMNI_AUDIO_CLOCK_ERROR_IO);
        assert(f.writes==(unsigned)failure && changed==(failure!=0));
    }
    /* Correct oscillator fields with a missing system-output clock gate also
     * require preparation, despite XO_READY remaining asserted. */
    init(&f,&clock); f.regs[OMNI_AC_XO_CTRL]=0x01c3459au; f.regs[OMNI_AC_CLOCK_CTRL]=0xc1u;
    assert(omni_audio_clock_startup_prepare_xo(&clock.ops,&changed)==OMNI_AUDIO_CLOCK_ERROR_NONE);
    assert(changed && f.regs[OMNI_AC_CLOCK_CTRL]==0xe1u);
}

static void test_startup_runner(void)
{
    fixture_t f; omni_audio_clock_t clock;
    const uint32_t uptimes[]={1000u,3600000u,0xfffffff0u};
    for (unsigned i=0;i<sizeof(uptimes)/sizeof(uptimes[0]);++i) {
        init(&f,&clock);
        startup_uptime=uptimes[i]; startup_tick_step=1u; startup_watchdog_calls=0u;
        assert(omni_audio_clock_startup_run(&clock,f.regs[OMNI_AC_XO_CTRL],
            startup_milliseconds,startup_watchdog));
        assert(clock.started_ms==uptimes[i]);
        assert(clock.state==OMNI_AUDIO_CLOCK_LOCAL_READY && clock.error==OMNI_AUDIO_CLOCK_ERROR_NONE);
        assert(startup_watchdog_calls>0u && startup_watchdog_calls<OMNI_AUDIO_CLOCK_TOTAL_TIMEOUT_MS);
        assert((uint32_t)(startup_uptime-uptimes[i])<OMNI_AUDIO_CLOCK_TOTAL_TIMEOUT_MS);
    }

    /* Reproduce the former call-site bug: beginning at zero at a real nonzero
     * uptime times out before even reading a register. The runner above must
     * use its same independent clock for begin and every subsequent poll. */
    init(&f,&clock); begin(&f,&clock,0u); poll(&f,&clock,3600000u);
    assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_TOTAL_TIMEOUT && f.calls==0u);

    init(&f,&clock); f.pll_lock=false;
    startup_uptime=3600000u; startup_tick_step=1u; startup_watchdog_calls=0u;
    assert(omni_audio_clock_startup_run(&clock,f.regs[OMNI_AC_XO_CTRL],
        startup_milliseconds,startup_watchdog));
    assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_NONE && clock.last_status==0u);
    assert(startup_watchdog_calls>0u);

    /* A stopped tick may not turn the startup wrapper into an infinite
     * watchdog-fed wait, nor may the poll-count guard claim clock readiness. */
    init(&f,&clock); startup_uptime=1000u; startup_tick_step=0u; startup_watchdog_calls=0u;
    assert(!omni_audio_clock_startup_run(&clock,f.regs[OMNI_AC_XO_CTRL],
        startup_milliseconds,startup_watchdog));
    assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_TOTAL_TIMEOUT);
    assert(startup_watchdog_calls==OMNI_AUDIO_CLOCK_STARTUP_POLL_LIMIT);
    assert(f.regs[OMNI_AC_MCLKSEL]==7u);

    init(&f,&clock); f.pll_lock=false; begin(&f,&clock,0u);
    until(&f,&clock,OMNI_AUDIO_CLOCK_LOCAL_READY,0u);
    f.regs[OMNI_AC_XO_STATUS]=1u;
    f.regs[OMNI_AC_PLL0STAT]=0u; /* LOCK need never assert in fractional mode. */
    assert(omni_audio_clock_startup_ready(true,f.regs,f.power));
    assert(!omni_audio_clock_startup_ready(false,f.regs,f.power));
    assert(!omni_audio_clock_startup_ready(true,NULL,f.power));
    for (uint32_t sel=0;sel<8u;++sel) {
        f.regs[OMNI_AC_MCLKSEL]=sel;
        assert(omni_audio_clock_startup_ready(true,f.regs,f.power)==(sel==1u));
    }
    f.regs[OMNI_AC_MCLKSEL]=1u;
    const omni_audio_clock_register_t reuse_checks[]={OMNI_AC_XO_STATUS,OMNI_AC_XO_CTRL,
        OMNI_AC_PLL0SEL,OMNI_AC_PLL0CTRL,OMNI_AC_PLL0NDEC,OMNI_AC_PLL0PDEC,
        OMNI_AC_PLL0SSCG0,OMNI_AC_PLL0SSCG1,OMNI_AC_PLL0DIV,OMNI_AC_MCLKDIV};
    for (unsigned i=0;i<sizeof(reuse_checks)/sizeof(reuse_checks[0]);++i) {
        uint32_t bit=reuse_checks[i]==OMNI_AC_XO_CTRL?2u:1u;
        f.regs[reuse_checks[i]]^=bit;
        assert(!omni_audio_clock_startup_ready(true,f.regs,f.power));
        f.regs[reuse_checks[i]]^=bit;
    }
    const uint32_t power_bits[]={0x100u,0x200u,0x100000u,0x800000u};
    for (unsigned i=0;i<sizeof(power_bits)/sizeof(power_bits[0]);++i)
        assert(!omni_audio_clock_startup_ready(true,f.regs,f.power|power_bits[i]));
    const uint32_t divider_flags[]={0x20000000u,0x40000000u,0x80000000u};
    for (unsigned i=0;i<sizeof(divider_flags)/sizeof(divider_flags[0]);++i) {
        f.regs[OMNI_AC_PLL0DIV]=divider_flags[i];
        assert(!omni_audio_clock_startup_ready(true,f.regs,f.power));
        f.regs[OMNI_AC_PLL0DIV]=0u; f.regs[OMNI_AC_MCLKDIV]=divider_flags[i]|1u;
        assert(!omni_audio_clock_startup_ready(true,f.regs,f.power));
        f.regs[OMNI_AC_MCLKDIV]=1u;
    }
}

static void test_fro_autotrim_verification(void)
{
    fixture_t f; omni_audio_clock_t clock;
    /* Actual running629bc observations while USB auto adjustment remained on.
     * The read-only DAC trim may change during the required PLL settle time. */
    const uint32_t observed[]={0x4176d3a0u,0x4177d3a0u,0x4178d3a0u,
        0x4179d3a0u,0x417ad3a0u,0x437ad3a0u,0xc17ad3a1u};
    for(unsigned i=0;i<sizeof(observed)/sizeof(observed[0]);++i) {
        init(&f,&clock); f.regs[OMNI_AC_FRO192M_CTRL]=0x4179d3a0u;
        begin(&f,&clock,1000u);
        uint32_t now=until(&f,&clock,OMNI_AUDIO_CLOCK_WAIT_PLL,1000u);
        f.regs[OMNI_AC_FRO192M_CTRL]=observed[i];
        until(&f,&clock,OMNI_AUDIO_CLOCK_LOCAL_READY,now);
        assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_NONE && clock.successful_writes==22u);
        assert(f.regs[OMNI_AC_FRO192M_CTRL]==observed[i]);
    }
    const unsigned protected_bits[]={14u,15u,24u,30u};
    for(unsigned i=0;i<sizeof(protected_bits)/sizeof(protected_bits[0]);++i) {
        init(&f,&clock); f.regs[OMNI_AC_FRO192M_CTRL]=0x4179d3a0u;
        begin(&f,&clock,1000u);
        uint32_t now=until(&f,&clock,OMNI_AUDIO_CLOCK_VERIFY,1000u);
        f.regs[OMNI_AC_FRO192M_CTRL]^=1u<<protected_bits[i];
        until(&f,&clock,OMNI_AUDIO_CLOCK_FAILED,now);
        assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_STATE_CHANGED);
        assert(clock.mismatch_reg==OMNI_AC_FRO192M_CTRL);
        assert(clock.mismatch_expected==0x4179d3a0u);
        assert(clock.mismatch_actual==f.regs[OMNI_AC_FRO192M_CTRL]);
        assert(clock.mismatch_mask==0x4100c000u && clock.step==OMNI_AC_FRO192M_CTRL);
    }
    /* Manual trimming has no autonomous writer; changed DAC remains a fault. */
    init(&f,&clock); f.regs[OMNI_AC_FRO192M_CTRL]=0x4079d3a0u;
    begin(&f,&clock,1000u);
    uint32_t now=until(&f,&clock,OMNI_AUDIO_CLOCK_VERIFY,1000u);
    f.regs[OMNI_AC_FRO192M_CTRL]^=1u<<16;
    until(&f,&clock,OMNI_AUDIO_CLOCK_FAILED,now);
    assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_STATE_CHANGED);
    assert(clock.mismatch_reg==OMNI_AC_FRO192M_CTRL && clock.mismatch_mask==0x41ffc000u);
}

int main(void)
{
    test_ready_but_unconfigured_xo();
    test_startup_runner();
    test_fro_autotrim_verification();
    fixture_t f; omni_audio_clock_t clock={0};
    omni_audio_clock_poll(NULL,0); omni_audio_clock_cancel(NULL);
    assert(!omni_audio_clock_init(NULL,NULL));
    assert(!omni_audio_clock_begin(&clock,16000000u,0,true,0));
    for (unsigned i=0;i<2u;++i) {
        init(&f,&clock);
        assert(!omni_audio_clock_begin(&clock,i?16000000u:32000000u,0,i==0u,0));
        assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_PREREQUISITE && f.calls==0u);
    }
    /* Reject every unsupported inherited routing/oscillator condition before
     * the first write, even though the outer guard claimed ownership. */
    const omni_audio_clock_register_t bad_regs[]={OMNI_AC_MAINCLKA,OMNI_AC_MAINCLKB,
        OMNI_AC_AHBDIV,OMNI_AC_USB0SEL,OMNI_AC_USB0DIV,OMNI_AC_XO_CTRL,
        OMNI_AC_MCLKIO,OMNI_AC_FC0SEL,OMNI_AC_FC2SEL,OMNI_AC_MCLKSEL};
    for (unsigned i=0;i<sizeof(bad_regs)/sizeof(bad_regs[0]);++i) {
        init(&f,&clock); begin(&f,&clock,0);
        omni_audio_clock_register_t r=bad_regs[i];
        f.regs[r]=r==OMNI_AC_XO_CTRL?0u:r==OMNI_AC_USB0SEL?0u:r==OMNI_AC_USB0DIV?0u:1u;
        until(&f,&clock,OMNI_AUDIO_CLOCK_FAILED,0);
        assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_PREREQUISITE && f.writes==0u);
    }
    /* Successful order, reserved-bit hygiene, no peripheral output enabling,
     * and preservation of all unrelated power and CPU/USB/FRO settings. */
    init(&f,&clock); begin(&f,&clock,0);
    until(&f,&clock,OMNI_AUDIO_CLOCK_LOCAL_READY,0);
    unsigned success_reads=f.reads;
    assert(clock.successful_writes==22u && f.writes==22u);
    assert(f.regs[OMNI_AC_XO_CTRL]==0x01c3459au);
    assert(f.regs[OMNI_AC_CLOCK_CTRL]==0x123u);
    assert(f.regs[OMNI_AC_PLL0DIV]==0 && f.regs[OMNI_AC_MCLKDIV]==1 && f.regs[OMNI_AC_MCLKSEL]==1);
    assert(f.regs[OMNI_AC_MCLKIO]==0 && f.regs[OMNI_AC_FC0SEL]==0 && f.regs[OMNI_AC_FC2SEL]==0);
    assert(f.regs[OMNI_AC_FRO192M_CTRL]==0x76543210u);
    assert((f.power&0x900300u)==0 && (f.power&~0x900300u)==(0x10901300u&~0x900300u));
    unsigned terminal_calls=f.calls;
    assert(!omni_audio_clock_begin(&clock,16000000u,0,true,100));
    poll(&f,&clock,100); omni_audio_clock_cancel(&clock);
    assert(f.calls==terminal_calls && clock.state==OMNI_AUDIO_CLOCK_LOCAL_READY);
    printf("{\"passed\":true,\"writes\":[");
    for (unsigned i=0;i<f.writes;++i)
        printf("%s[%u,%u]",i?",":"",(unsigned)f.write_regs[i],f.write_values[i]);
    printf("]}\n");

    /* A never-ready oscillator cannot result in PLL or MCLK programming. */
    init(&f,&clock); f.xo_ready=false; begin(&f,&clock,0);
    uint32_t now=until(&f,&clock,OMNI_AUDIO_CLOCK_WAIT_XO,0);
    poll(&f,&clock,clock.phase_ms+99u); assert(clock.state==OMNI_AUDIO_CLOCK_WAIT_XO);
    poll(&f,&clock,clock.phase_ms+100u);
    assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_XO_TIMEOUT && f.writes==4u);
    (void)now;
    init(&f,&clock); f.pll_lock=false; begin(&f,&clock,0);
    until(&f,&clock,OMNI_AUDIO_CLOCK_WAIT_PLL,0);
    poll(&f,&clock,clock.phase_ms); /* arm timer after the power-up poll returned */
    poll(&f,&clock,clock.phase_ms+5u); assert(clock.state==OMNI_AUDIO_CLOCK_WAIT_PLL);
    poll(&f,&clock,clock.phase_ms+6u); assert(clock.state==OMNI_AUDIO_CLOCK_WAIT_PLL);
    uint32_t after_settle=clock.phase_ms+7u;
    poll(&f,&clock,after_settle); assert(clock.state==OMNI_AUDIO_CLOCK_DIV_SETUP);
    until(&f,&clock,OMNI_AUDIO_CLOCK_LOCAL_READY,after_settle);
    assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_NONE && clock.last_status==0u);
    init(&f,&clock); begin(&f,&clock,0);
    until(&f,&clock,OMNI_AUDIO_CLOCK_WAIT_PLL,0);
    poll(&f,&clock,clock.phase_ms);
    poll(&f,&clock,clock.phase_ms+20u);
    assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_PLL_TIMEOUT && f.writes==17u);
    init(&f,&clock); begin(&f,&clock,0);
    until(&f,&clock,OMNI_AUDIO_CLOCK_WAIT_PLL,0);
    poll(&f,&clock,clock.phase_ms); poll(&f,&clock,clock.phase_ms+1u);
    assert(clock.state==OMNI_AUDIO_CLOCK_WAIT_PLL);
    poll(&f,&clock,clock.phase_ms+6u); assert(clock.state==OMNI_AUDIO_CLOCK_WAIT_PLL);
    poll(&f,&clock,clock.phase_ms+7u); assert(clock.state==OMNI_AUDIO_CLOCK_DIV_SETUP);

    /* Simulate an interrupt delaying the final power-up store relative to
     * its poll argument. The next poll establishes a fresh epoch; old time
     * may never be credited toward the hardware's settling interval. */
    init(&f,&clock); f.pll_lock=false; begin(&f,&clock,1000u);
    until(&f,&clock,OMNI_AUDIO_CLOCK_WAIT_PLL,1000u);
    uint32_t delayed_first=clock.phase_ms+6u;
    poll(&f,&clock,delayed_first);
    assert(clock.state==OMNI_AUDIO_CLOCK_WAIT_PLL && clock.phase_ms==delayed_first);
    poll(&f,&clock,delayed_first+6u); assert(clock.state==OMNI_AUDIO_CLOCK_WAIT_PLL);
    poll(&f,&clock,delayed_first+7u); assert(clock.state==OMNI_AUDIO_CLOCK_DIV_SETUP);

    init(&f,&clock); f.div_ready=false; begin(&f,&clock,0);
    until(&f,&clock,OMNI_AUDIO_CLOCK_WAIT_DIV,0);
    poll(&f,&clock,clock.phase_ms+20u);
    assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_DIV_TIMEOUT);
    init(&f,&clock); begin(&f,&clock,0); poll(&f,&clock,500);
    assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_TOTAL_TIMEOUT && f.calls==0u);

    /* Wrap does not turn a time-limited operation into an infinite wait. */
    init(&f,&clock); begin(&f,&clock,0xfffffff0u);
    until(&f,&clock,OMNI_AUDIO_CLOCK_LOCAL_READY,0xfffffff0u);
    init(&f,&clock); f.xo_ready=false; begin(&f,&clock,0xffffffd0u);
    until(&f,&clock,OMNI_AUDIO_CLOCK_WAIT_XO,0xffffffd0u);
    poll(&f,&clock,clock.phase_ms+100u); assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_XO_TIMEOUT);

    /* Failed callbacks and ownership loss never cause later writes or retries. */
    for (int failure=0;failure<22;++failure) {
        init(&f,&clock); f.fail_write=failure; begin(&f,&clock,0);
        until(&f,&clock,OMNI_AUDIO_CLOCK_FAILED,0);
        assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_IO && f.writes==(unsigned)failure);
        terminal_calls=f.calls; poll(&f,&clock,400); assert(f.calls==terminal_calls);
    }
    for (int failure=0;failure<(int)success_reads;++failure) {
        init(&f,&clock); f.fail_read=failure; begin(&f,&clock,0);
        until(&f,&clock,OMNI_AUDIO_CLOCK_FAILED,0);
        assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_IO);
    }
    for (unsigned phase=OMNI_AUDIO_CLOCK_PREFLIGHT;phase<=OMNI_AUDIO_CLOCK_VERIFY;++phase) {
        init(&f,&clock); begin(&f,&clock,0);
        now=until(&f,&clock,(omni_audio_clock_state_t)phase,0);
        unsigned writes=f.writes; f.owned=false; poll(&f,&clock,now);
        assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_OWNERSHIP && f.writes==writes);
        init(&f,&clock); begin(&f,&clock,0);
        now=until(&f,&clock,(omni_audio_clock_state_t)phase,0); writes=f.writes;
        omni_audio_clock_cancel(&clock); poll(&f,&clock,now);
        assert(clock.state==OMNI_AUDIO_CLOCK_CANCELED && f.writes==writes);
    }
    /* Clock drift or external configuration mutation prevents local-ready. */
    for (unsigned changed=0;changed<12u;++changed) {
        init(&f,&clock); begin(&f,&clock,0);
        now=until(&f,&clock,OMNI_AUDIO_CLOCK_VERIFY,0);
        f.regs[changed]^=2u;
        if (changed==OMNI_AC_MCLKIO) f.regs[changed]^=3u;
        if (changed==OMNI_AC_FRO192M_CTRL) f.regs[changed]^=2u|(1u<<14);
        until(&f,&clock,OMNI_AUDIO_CLOCK_FAILED,now);
        assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_STATE_CHANGED);
    }
    const omni_audio_clock_register_t changed_pll[]={OMNI_AC_PLL0SEL,OMNI_AC_PLL0CTRL,
        OMNI_AC_PLL0NDEC,OMNI_AC_PLL0PDEC,OMNI_AC_PLL0SSCG0,OMNI_AC_PLL0SSCG1};
    for (unsigned i=0;i<sizeof(changed_pll)/sizeof(changed_pll[0]);++i) {
        init(&f,&clock); begin(&f,&clock,0);
        now=until(&f,&clock,OMNI_AUDIO_CLOCK_VERIFY,0);
        f.regs[changed_pll[i]]^=1u;
        until(&f,&clock,OMNI_AUDIO_CLOCK_FAILED,now);
        assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_STATE_CHANGED);
    }
    init(&f,&clock); begin(&f,&clock,0);
    now=until(&f,&clock,OMNI_AUDIO_CLOCK_VERIFY,0); f.xo_ready=false;
    until(&f,&clock,OMNI_AUDIO_CLOCK_FAILED,now);
    assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_STATE_CHANGED);
    /* Reject the former12MHz board and a divided96MHz MAIN: this
     * candidate has exactly one admitted CPU/AHB clock contract. */
    for (unsigned rejected=0;rejected<2u;++rejected) {
        init(&f,&clock);
        if (!rejected) f.regs[OMNI_AC_MAINCLKA]=0u;
        else f.regs[OMNI_AC_AHBDIV]=1u;
        begin(&f,&clock,0); until(&f,&clock,OMNI_AUDIO_CLOCK_FAILED,0);
        assert(clock.error==OMNI_AUDIO_CLOCK_ERROR_PREREQUISITE && !f.writes);
    }
    return 0;
}
