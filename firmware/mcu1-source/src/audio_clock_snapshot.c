#include "audio_clock_snapshot.h"
#include "fsl_device_registers.h"
#include <stddef.h>

static uint32_t fixed_read(uint32_t address)
{
    return *(volatile const uint32_t *)(uintptr_t)address;
}
bool omni_audio_clock_snapshot(uint32_t out[OMNI_AUDIO_CLOCK_SNAPSHOT_WORDS])
{
    if (!out) return false;
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    const uint32_t analog_gate=1u<<27;
    uint32_t gates=fixed_read(0x40000208u), reset=fixed_read(0x40000108u);
    bool analog=(gates&analog_gate)!=0u && (reset&analog_gate)==0u;
    /* Addresses are fixed by LPC5528's pinned register headers and UM11126.
     * Zero entries are metadata/conditional ANACTRL fields handled below. */
    static const uint32_t addresses[OMNI_AUDIO_CLOCK_SNAPSHOT_WORDS]={
        0,0,0,0,0,0,0,0,
        0x400200b8u,0x400200bcu,0x40000a18u,0x40000280u,0x40000284u,
        0x40000380u,0x400002a8u,0x40000398u,0x40000290u,0x40000580u,
        0x40000584u,0x40000588u,0x4000058cu,0x40000590u,0x40000594u,
        0x400003c4u,0x400002e0u,0x400003acu,0x40000420u,0x400002b0u,
        0x400002b8u,0x40000200u,0x40000204u,0,0x40000388u,
        0x400002a4u,0x400002f0u,0x400002f8u,0x40000320u,0x40000328u,0,0
    };
    for (unsigned i=0;i<OMNI_AUDIO_CLOCK_SNAPSHOT_WORDS;++i)
        out[i]=addresses[i]?fixed_read(addresses[i]):0u;
    out[0]=1u; out[1]=3u; out[2]=gates; out[31]=reset;
    if (analog) {
        out[1]|=4u;
        out[4]=fixed_read(0x40013020u); out[5]=fixed_read(0x40013024u);
        out[6]=fixed_read(0x40013010u); out[7]=fixed_read(0x400130b0u);
    }
    out[3]=fixed_read(0x40000208u); out[38]=fixed_read(0x40000108u);
    if (out[3]==gates && out[38]==reset) out[1]|=0x100u;
    if (analog && (out[3]&analog_gate)!=0u && (out[38]&analog_gate)==0u)
        out[39]=fixed_read(0x40013024u);
    __set_PRIMASK(mask); return true;
}
