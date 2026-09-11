"""Exercise the real RX packet path with synthetic DMA blocks and fake registers."""
from pathlib import Path
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[1] / 'mcu1-source'
code = (SOURCE / 'src/microphone.c').read_text()
code = code.replace('#include "fsl_device_registers.h"', '')
code = code.replace('#include "board_clock.h"', '')
code = code.replace('#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))', '#define REG(a) (*reg(a))')
stub = r'''
#include <stdint.h>
#include <assert.h>
#define OMNI_CPU_GUARD_ITERATIONS(n) (n)
static uint32_t addresses[64], values[64], used;
static uint32_t *reg(uint32_t a) {
 for(unsigned i=0;i<used;++i) if(addresses[i]==a)return &values[i];
 assert(used<64);addresses[used]=a;return &values[used++];
}
static struct {uint32_t DMA0_REQ_ENA_CLR,DMA0_REQ_ENA_SET;} mux;
#define INPUTMUX (&mux)
#define DMA0_IRQn 0
static uint32_t __get_PRIMASK(void){return 0;}
static void __disable_irq(void){}
static void __set_PRIMASK(uint32_t m){(void)m;}
static void __DMB(void){}
static void NVIC_SetPriority(int i,int p){(void)i;(void)p;}
static void NVIC_EnableIRQ(int i){(void)i;}
'''
test = r'''
static void block(int value) {
 unsigned bank=expected_b;
 for(unsigned i=0;i<frames;++i)blocks[bank][i]=(uint16_t)(int16_t)(value+(int)i);
 REG(0x40082058u)=bank?0:BIT;REG(0x40082060u)=bank?BIT:0;REG(0x40082040u)=0;
 REG(0x40082448u)=((frames-4u)<<16)|(bank?0x10u:0x20u);
 REG(0x40086e04u)=0;
 omni_microphone_irq();
}
int main(void) {
 uint32_t head[4],status[15];uint8_t out[98];
 assert(omni_microphone_start(head,48000));
 assert(head[1]==0x40086e30u && ((head[0]>>8)&3u)==1u);
 block(-1000);block(500);block(2000);
 assert(omni_microphone_packet(out)==96u);
 assert((int16_t)((unsigned)out[0]|((unsigned)out[1]<<8))==-1000);
 assert((int16_t)((unsigned)out[94]|((unsigned)out[95]<<8))==-953);
 omni_microphone_status(status);assert(status[4]==144 && status[5]==144 && !status[2]);
 assert(omni_microphone_stop());assert(omni_microphone_packet(out)==96);
 for(unsigned i=0;i<96;++i)assert(out[i]==0);
 assert(omni_microphone_start(head,96000));block(100);block(200);block(300);
 assert(omni_microphone_packet(out)==96);
 assert((unsigned)out[0]==100u && (unsigned)out[2]==102u);
 for(unsigned i=0;i<30;++i)block(500);
 omni_microphone_status(status);assert(status[7]<=1024 && status[8]>0);
 REG(0x40082040u)=BIT;omni_microphone_irq();
 omni_microphone_status(status);assert(status[2] && !status[1]);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(stub + code + test)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(SOURCE / 'include'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Microphone signed PCM, 48/96k packets, stop silence, overflow and DMA fault checks passed')
