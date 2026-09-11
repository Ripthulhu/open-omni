#include "usb_guard.h"
#include "fsl_device_registers.h"
#include <string.h>

static uint32_t since, watching, repairs, last_repair;
static uint32_t first[10];
/* UM11126: SETUP latches reception separately from INTSTAT.EP0OUT.
 * Main cannot observe an in-progress USB ISR. Re-signal only an enabled,
 * attached, continuously pending SETUP whose interrupt is absent for 2ms.
 * No controller reset, descriptor mutation, flash write or volume change.
 * A 10ms backoff bounds retries if a different failure prevents servicing. */
void omni_usb_guard_poll(uint32_t now)
{
    uint32_t mask=__get_PRIMASK();__disable_irq();
    uint32_t command=USB0->DEVCMDSTAT,status=USB0->INTSTAT,enabled=USB0->INTEN;
    if ((command & (USB_DEVCMDSTAT_DCON_MASK|USB_DEVCMDSTAT_SETUP_MASK)) !=
            (USB_DEVCMDSTAT_DCON_MASK|USB_DEVCMDSTAT_SETUP_MASK) ||
        !(enabled&USB_INTSTAT_EP0OUT_MASK) || (status&USB_INTSTAT_EP0OUT_MASK)) {
        watching=0;
    } else if (!watching) {
        watching=1;since=now;
    } else if ((uint32_t)(now-since)>=2U && (!repairs || (uint32_t)(now-last_repair)>=10U)) {
        if (!repairs) {
            first[0]=now;first[1]=command;first[2]=status;first[3]=enabled;
            first[4]=USB0->INFO;first[5]=USB0->EPLISTSTART;
            first[6]=USB0->EPSKIP;first[7]=USB0->EPINUSE;
            uintptr_t list=USB0->EPLISTSTART;
            if (!(list&255U) && list>=0x20000000U && list<=0x2002ff00U) {
                first[8]=((volatile uint32_t *)list)[0];
                first[9]=((volatile uint32_t *)list)[2];
            }
        }
        ++repairs;watching=2;since=now;last_repair=now;
        USB0->INTSETSTAT=USB_INTSTAT_EP0OUT_MASK;
    }
    __set_PRIMASK(mask);
}
void omni_usb_guard_status(uint8_t out[60])
{
    uint32_t values[15]={1,repairs,watching,since};
    memcpy(values+4,first,sizeof(first));
    memcpy(out,values,sizeof(values));
}
