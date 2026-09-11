#include "dsp_link.h"
#include <string.h>

bool omni_dsp_link_init(omni_dsp_link *link, mcu2_link_io io)
{
    if (!link || !io.tx || !io.rx) return false;
    memset(link, 0, sizeof(*link));
    link->io = io;
    return omni_link_init(&link->parser, 20U);
}

bool omni_dsp_link_start(omni_dsp_link *link, uint32_t now_ms)
{
    if (!link || !link->io.tx || !link->io.rx || link->phase == OMNI_DSP_RUNNING)
        return false;
    if (!omni_link_init(&link->parser, 20U)) return false;
    link->phase = OMNI_DSP_RUNNING;
    link->start_ms = now_ms;
    ++link->attempts;
    link->tx_bytes = link->rx_bytes = link->received_frames = link->rejected_frames = 0;
    link->reply_length = link->first_unexpected_length = 0;
    memset(link->reply, 0, sizeof(link->reply));
    memset(link->reply_tail, 0, sizeof(link->reply_tail));
    memset(link->first_unexpected, 0, sizeof(link->first_unexpected));
    return true;
}

void omni_dsp_link_cancel(omni_dsp_link *link)
{
    if (link && link->phase == OMNI_DSP_RUNNING) link->phase = OMNI_DSP_CANCELLED;
}

void omni_dsp_link_poll(omni_dsp_link *link, uint32_t now_ms)
{
    static const uint8_t query[] = {0xbd, 4, 0xe1, 2};
    static const uint8_t expected[] = {0xdb, 13, 0xe1, 3};
    if (!link || link->phase != OMNI_DSP_RUNNING) return;
    if ((uint32_t)(now_ms - link->start_ms) >= 250U) {
        link->phase = OMNI_DSP_TIMEOUT;
        return;
    }
    for (unsigned n = 0; n < 16U && link->tx_bytes < sizeof(query); ++n) {
        int result = link->io.tx(link->io.context, query[link->tx_bytes]);
        if (result == 0) break;
        if (result != 1) { link->phase = OMNI_DSP_IO_ERROR; return; }
        ++link->tx_bytes;
    }
    omni_link_expire(&link->parser, now_ms);
    for (unsigned n = 0; n < 32U; ++n) {
        uint8_t byte;
        int result = link->io.rx(link->io.context, &byte);
        if (result == 0) break;
        if (result != 1) { link->phase = OMNI_DSP_IO_ERROR; return; }
        ++link->rx_bytes;
        const uint8_t *frame;
        size_t length;
        if (!omni_link_feed(&link->parser, byte, now_ms, &frame, &length)) continue;
        ++link->received_frames;
        if (link->tx_bytes == sizeof(query) && length == 13U &&
            memcmp(frame, expected, sizeof(expected)) == 0) {
            memcpy(link->reply, frame + 4, sizeof(link->reply));
            memcpy(link->reply_tail, frame + 7, sizeof(link->reply_tail));
            link->reply_length = (uint16_t)length;
            link->phase = OMNI_DSP_SUCCESS;
            return;
        }
        if (link->rejected_frames == 0) {
            link->first_unexpected_length = (uint16_t)length;
            size_t copy = length < sizeof(link->first_unexpected) ? length : sizeof(link->first_unexpected);
            memcpy(link->first_unexpected, frame, copy);
        }
        ++link->rejected_frames;
    }
}
