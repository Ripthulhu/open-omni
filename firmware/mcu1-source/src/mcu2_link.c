#include "mcu2_link.h"
#include <string.h>

static void reset_trace(omni_mcu2_link *link)
{
    link->trace_first_tx_ms = link->trace_last_tx_ms = UINT32_MAX;
    link->trace_first_rx_ms = link->trace_last_rx_ms = UINT32_MAX;
    link->trace_rx_used = link->trace_rx_truncated = 0;
    memset(link->trace_rx, 0, sizeof(link->trace_rx));
}

bool omni_mcu2_link_init(omni_mcu2_link *link, mcu2_link_io io)
{
    if (!link || !io.tx || !io.rx) return false;
    memset(link, 0, sizeof(*link));
    link->io = io;
    reset_trace(link);
    return true;
}

bool omni_mcu2_link_start(omni_mcu2_link *link, uint32_t now_ms)
{
    return omni_mcu2_link_start_query(link, now_ms, OMNI_MCU2_QUERY_VERSION);
}

bool omni_mcu2_link_start_query(omni_mcu2_link *link, uint32_t now_ms,
                                omni_mcu2_query query)
{
    if (!link || !link->io.tx || !link->io.rx || link->phase == OMNI_MCU2_RUNNING ||
        (query != OMNI_MCU2_QUERY_VERSION && query != OMNI_MCU2_QUERY_DETECT))
        return false;
    link->query = query;
    link->phase = OMNI_MCU2_RUNNING;
    link->start_ms = now_ms;
    ++link->attempts;
    link->tx_bytes = 0;
    link->rx_bytes = 0;
    link->received_frames = 0;
    link->rejected_frames = 0;
    link->frame_used = 0;
    link->padding_nonzero = false;
    memset(link->reply, 0, sizeof(link->reply));
    memset(link->header, 0, sizeof(link->header));
    reset_trace(link);
    return true;
}

void omni_mcu2_link_cancel(omni_mcu2_link *link)
{
    if (link && link->phase == OMNI_MCU2_RUNNING)
        link->phase = OMNI_MCU2_CANCELLED;
}

void omni_mcu2_link_poll(omni_mcu2_link *link, uint32_t now_ms)
{
    static const uint8_t requests[2][4] = {{0xbc, 4, 0xe1, 2}, {0xbc, 3, 0x87, 0}};
    static const uint8_t response[] = {0xcb, 7, 0xe1, 3};
    if (!link || link->phase != OMNI_MCU2_RUNNING) return;
    uint32_t elapsed_ms = (uint32_t)(now_ms - link->start_ms);
    if (elapsed_ms >= OMNI_MCU2_QUERY_TIMEOUT_MS) {
        link->phase = OMNI_MCU2_TIMEOUT;
        return;
    }
    for (unsigned n = 0; n < 16U && link->tx_bytes < OMNI_MCU2_FRAME_BYTES; ++n) {
        uint8_t byte = link->tx_bytes < 4U ? requests[link->query][link->tx_bytes] : 0;
        int result = link->io.tx(link->io.context, byte);
        if (result == 0) break;
        if (result != 1) { link->phase = OMNI_MCU2_IO_ERROR; return; }
        if (link->tx_bytes == 0) link->trace_first_tx_ms = elapsed_ms;
        link->trace_last_tx_ms = elapsed_ms;
        ++link->tx_bytes;
    }
    for (unsigned n = 0; n < 32U; ++n) {
        uint8_t byte;
        int result = link->io.rx(link->io.context, &byte);
        if (result == 0) break;
        if (result != 1) { link->phase = OMNI_MCU2_IO_ERROR; return; }
        if (link->rx_bytes == 0) link->trace_first_rx_ms = elapsed_ms;
        link->trace_last_rx_ms = elapsed_ms;
        if (link->trace_rx_used < OMNI_MCU2_TRACE_RX_BYTES)
            link->trace_rx[link->trace_rx_used++] = byte;
        else
            ++link->trace_rx_truncated;
        ++link->rx_bytes;
        if (link->frame_used < sizeof(link->header)) link->header[link->frame_used] = byte;
        else if (byte != 0) link->padding_nonzero = true;
        ++link->frame_used;
        if (link->frame_used == OMNI_MCU2_FRAME_BYTES) {
            ++link->received_frames;
            bool match = link->query == OMNI_MCU2_QUERY_VERSION ?
                memcmp(link->header, response, sizeof(response)) == 0 :
                link->header[0] == 0xcb && link->header[1] == 4 &&
                link->header[2] == 0x87 && link->header[3] <= 3 &&
                link->header[4] == 0 && link->header[5] == 0 && link->header[6] == 0;
            if (link->tx_bytes == OMNI_MCU2_FRAME_BYTES && !link->padding_nonzero && match) {
                if (link->query == OMNI_MCU2_QUERY_VERSION)
                    memcpy(link->reply, link->header + 4, sizeof(link->reply));
                else link->reply[0] = link->header[3];
                link->phase = OMNI_MCU2_SUCCESS;
                return;
            }
            ++link->rejected_frames;
            link->frame_used = 0;
            link->padding_nonzero = false;
        }
    }
}
