#include "audio_mode.h"

#include <string.h>

static const uint8_t stop_frame[] = {0xbd, 6, 0x51, 1, 4, 0};
static const uint8_t gate_frame[] = {0xbd, 6, 0x88, 1, 0, 0};
static const uint8_t resume_frame[] = {0xbd, 5, 0x88, 1, 0};

static bool active(const omni_audio_mode_t *mode)
{
    return mode->state > OMNI_AUDIO_MODE_IDLE &&
           mode->state < OMNI_AUDIO_MODE_LOCAL_COMPLETE;
}

static void isolate(omni_audio_mode_t *mode)
{
    if (!mode->pins_isolated) {
        mode->pins_isolated = mode->ops.pins(mode->ops.context, false);
        if (!mode->pins_isolated) mode->isolation_failed = true;
    }
}

static void fail(omni_audio_mode_t *mode, omni_audio_mode_error_t error)
{
    mode->error = error;
    mode->state = OMNI_AUDIO_MODE_FAILED;
    isolate(mode);
}

static void transition(omni_audio_mode_t *mode, omni_audio_mode_state_t state,
                       uint32_t now_ms)
{
    mode->state = state;
    mode->phase_ms = now_ms;
    mode->frame_offset = 0;
}

static void send_frame(omni_audio_mode_t *mode, const uint8_t *frame, size_t length,
                       omni_audio_mode_state_t drain, uint32_t now_ms)
{
    if (mode->frame_offset == 0u) {
        mode->ack_command = frame[2];
        mode->ack_waiting = false;
        mode->ack_received = false;
        mode->ack_status = 0xffu;
    }
    size_t remaining = length - mode->frame_offset;
    int accepted = mode->ops.write(mode->ops.context, frame + mode->frame_offset, remaining);
    if (accepted < 0 || (size_t)accepted > remaining) {
        fail(mode, OMNI_AUDIO_MODE_ERROR_TX);
        return;
    }
    mode->frame_offset = (uint8_t)(mode->frame_offset + (unsigned)accepted);
    mode->transmitted_bytes += (uint32_t)accepted;
    if (mode->frame_offset == length) {
        /* A reply can arrive before the next TXIDLE poll. The complete request
         * must first have been accepted; partial/stale replies cannot qualify. */
        mode->ack_waiting = mode->require_ack;
        transition(mode, drain, now_ms);
    }
}

static void wait_drain(omni_audio_mode_t *mode, omni_audio_mode_state_t next, uint32_t now_ms)
{
    omni_audio_mode_io_t status = mode->ops.tx_status(mode->ops.context);
    if (status == OMNI_AUDIO_MODE_IO_COMPLETE) transition(mode, next, now_ms);
    else if (status != OMNI_AUDIO_MODE_IO_PENDING) fail(mode, OMNI_AUDIO_MODE_ERROR_TX);
}

bool omni_audio_mode_init(omni_audio_mode_t *mode, const omni_audio_mode_ops_t *ops)
{
    if (mode == NULL || ops == NULL || ops->write == NULL || ops->tx_status == NULL ||
        ops->pins == NULL || ops->configure_begin == NULL || ops->configure_poll == NULL) return false;
    /* Copy before clearing permits callers to pass &mode->ops at explicit reinit. */
    omni_audio_mode_ops_t saved_ops = *ops;
    memset(mode, 0, sizeof(*mode));
    mode->ops = saved_ops;
    mode->initialized = true;
    return true;
}

bool omni_audio_mode_begin(omni_audio_mode_t *mode, uint32_t sample_rate, uint32_t now_ms)
{
    if (mode == NULL || !mode->initialized ||
        (sample_rate != 48000u && sample_rate != 96000u) ||
        (mode->state != OMNI_AUDIO_MODE_IDLE && mode->state != OMNI_AUDIO_MODE_LOCAL_COMPLETE)) return false;
    mode->sample_rate = sample_rate;
    mode->started_ms = now_ms;
    mode->transmitted_bytes = 0;
    mode->error = OMNI_AUDIO_MODE_ERROR_NONE;
    mode->isolation_failed = false;
    mode->acknowledged_frames = 0;
    mode->ack_waiting = false;
    mode->ack_received = false;
    transition(mode, OMNI_AUDIO_MODE_INITIAL_DELAY, now_ms);
    return true;
}

bool omni_audio_mode_require_ack(omni_audio_mode_t *mode)
{
    if (!mode || !mode->initialized ||
        (mode->state != OMNI_AUDIO_MODE_IDLE &&
         mode->state != OMNI_AUDIO_MODE_LOCAL_COMPLETE &&
         !(mode->state == OMNI_AUDIO_MODE_INITIAL_DELAY && !mode->transmitted_bytes))) return false;
    mode->require_ack = true;
    return true;
}

bool omni_audio_mode_receive_ack(omni_audio_mode_t *mode,
                                 const uint8_t *frame, size_t length)
{
    if (!mode || !frame || !active(mode) || !mode->require_ack || !mode->ack_waiting ||
        length != 4u || frame[0] != 0xddu || frame[1] != 3u ||
        frame[2] != mode->ack_command) return false;
    mode->ack_waiting = false;
    mode->ack_status = frame[3];
    if (frame[3] != 0u) fail(mode, OMNI_AUDIO_MODE_ERROR_REJECTED);
    else { mode->ack_received = true; ++mode->acknowledged_frames; }
    return true;
}

static bool accepted(const omni_audio_mode_t *mode)
{ return !mode->require_ack || mode->ack_received; }

omni_audio_mode_io_t omni_audio_mode_quiesce(omni_audio_mode_t *mode, uint32_t now_ms)
{
    if(!mode || !mode->initialized) return OMNI_AUDIO_MODE_IO_ERROR;
    if(!mode->quiescing) {
        mode->quiescing=true;mode->quiesce_started_ms=now_ms;
        mode->quiesce_drained=false;
    }
    if((uint32_t)(now_ms-mode->quiesce_started_ms)>=OMNI_AUDIO_MODE_TIMEOUT_MS) {
        fail(mode,OMNI_AUDIO_MODE_ERROR_TIMEOUT);return OMNI_AUDIO_MODE_IO_ERROR;
    }
    /* With backpressure a frame can span polls. Complete its remaining bytes;
     * stopping at FIFO acceptance could leave the peer inside a partial frame. */
    if(mode->frame_offset) {
        switch(mode->state) {
        case OMNI_AUDIO_MODE_SEND_STOP:
            send_frame(mode,stop_frame,sizeof(stop_frame),OMNI_AUDIO_MODE_STOP_DRAIN,now_ms);break;
        case OMNI_AUDIO_MODE_SEND_GATE:
            send_frame(mode,gate_frame,sizeof(gate_frame),OMNI_AUDIO_MODE_GATE_DRAIN,now_ms);break;
        case OMNI_AUDIO_MODE_SEND_RESUME:
            send_frame(mode,resume_frame,sizeof(resume_frame),OMNI_AUDIO_MODE_RESUME_DRAIN,now_ms);break;
        case OMNI_AUDIO_MODE_SEND_RATE: {
            const uint8_t rate[]={0xbd,6,0x51,1,1,mode->sample_rate==96000u?1u:0u};
            send_frame(mode,rate,sizeof(rate),OMNI_AUDIO_MODE_RATE_DRAIN,now_ms);break;
        }
        default: return OMNI_AUDIO_MODE_IO_ERROR;
        }
        return mode->state==OMNI_AUDIO_MODE_FAILED?OMNI_AUDIO_MODE_IO_ERROR:OMNI_AUDIO_MODE_IO_PENDING;
    }
    if(!mode->quiesce_drained) {
        omni_audio_mode_io_t status=mode->ops.tx_status(mode->ops.context);
        if(status==OMNI_AUDIO_MODE_IO_PENDING) return status;
        if(status!=OMNI_AUDIO_MODE_IO_COMPLETE) {
            fail(mode,OMNI_AUDIO_MODE_ERROR_TX);return OMNI_AUDIO_MODE_IO_ERROR;
        }
        mode->quiesce_drained=true;mode->quiesce_drained_ms=now_ms;
    }
    if((uint32_t)(now_ms-mode->quiesce_drained_ms)<OMNI_AUDIO_MODE_INTERFRAME_MS)
        return OMNI_AUDIO_MODE_IO_PENDING;
    isolate(mode);mode->state=OMNI_AUDIO_MODE_CANCELED;
    return mode->isolation_failed?OMNI_AUDIO_MODE_IO_ERROR:OMNI_AUDIO_MODE_IO_COMPLETE;
}

void omni_audio_mode_poll(omni_audio_mode_t *mode, uint32_t now_ms)
{
    if (mode == NULL || !mode->initialized || !active(mode) || mode->quiescing) return;
    if ((uint32_t)(now_ms - mode->started_ms) >= OMNI_AUDIO_MODE_TIMEOUT_MS) {
        fail(mode, OMNI_AUDIO_MODE_ERROR_TIMEOUT);
        return;
    }
    switch (mode->state) {
    case OMNI_AUDIO_MODE_INITIAL_DELAY:
        if ((uint32_t)(now_ms - mode->phase_ms) >= 50u)
            transition(mode, OMNI_AUDIO_MODE_SEND_STOP, now_ms);
        break;
    case OMNI_AUDIO_MODE_SEND_STOP:
        send_frame(mode, stop_frame, sizeof(stop_frame), OMNI_AUDIO_MODE_STOP_DRAIN, now_ms);
        break;
    case OMNI_AUDIO_MODE_STOP_DRAIN:
        wait_drain(mode, OMNI_AUDIO_MODE_STOP_GAP, now_ms);
        break;
    case OMNI_AUDIO_MODE_STOP_GAP:
        if (accepted(mode) && (uint32_t)(now_ms - mode->phase_ms) >= OMNI_AUDIO_MODE_INTERFRAME_MS)
            transition(mode, OMNI_AUDIO_MODE_SEND_GATE, now_ms);
        break;
    case OMNI_AUDIO_MODE_SEND_GATE:
        send_frame(mode, gate_frame, sizeof(gate_frame), OMNI_AUDIO_MODE_GATE_DRAIN, now_ms);
        break;
    case OMNI_AUDIO_MODE_GATE_DRAIN:
        wait_drain(mode, OMNI_AUDIO_MODE_QUIET, now_ms);
        break;
    case OMNI_AUDIO_MODE_QUIET:
        if (accepted(mode) && (uint32_t)(now_ms - mode->phase_ms) >= 75u)
            transition(mode, OMNI_AUDIO_MODE_ISOLATE, now_ms);
        break;
    case OMNI_AUDIO_MODE_ISOLATE:
        mode->pins_isolated = mode->ops.pins(mode->ops.context, false);
        if (!mode->pins_isolated) {
            mode->isolation_failed = true;
            mode->error = OMNI_AUDIO_MODE_ERROR_PINS;
            mode->state = OMNI_AUDIO_MODE_FAILED;
        } else transition(mode, OMNI_AUDIO_MODE_CONFIGURE_BEGIN, now_ms);
        break;
    case OMNI_AUDIO_MODE_CONFIGURE_BEGIN:
        if (!mode->ops.configure_begin(mode->ops.context, mode->sample_rate))
            fail(mode, OMNI_AUDIO_MODE_ERROR_CONFIGURATION);
        else transition(mode, OMNI_AUDIO_MODE_CONFIGURING, now_ms);
        break;
    case OMNI_AUDIO_MODE_CONFIGURING: {
        omni_audio_mode_io_t status = mode->ops.configure_poll(mode->ops.context);
        if (status == OMNI_AUDIO_MODE_IO_COMPLETE) transition(mode, OMNI_AUDIO_MODE_CONNECT, now_ms);
        else if (status != OMNI_AUDIO_MODE_IO_PENDING) fail(mode, OMNI_AUDIO_MODE_ERROR_CONFIGURATION);
        break;
    }
    case OMNI_AUDIO_MODE_CONNECT:
        /* A failed enable can have changed some pins, so isolate again on error. */
        mode->pins_isolated = false;
        if (!mode->ops.pins(mode->ops.context, true)) fail(mode, OMNI_AUDIO_MODE_ERROR_PINS);
        else transition(mode, OMNI_AUDIO_MODE_RESUME_DELAY, now_ms);
        break;
    case OMNI_AUDIO_MODE_RESUME_DELAY:
        if ((uint32_t)(now_ms - mode->phase_ms) >= 75u)
            transition(mode, OMNI_AUDIO_MODE_SEND_RESUME, now_ms);
        break;
    case OMNI_AUDIO_MODE_SEND_RESUME:
        send_frame(mode, resume_frame, sizeof(resume_frame), OMNI_AUDIO_MODE_RESUME_DRAIN, now_ms);
        break;
    case OMNI_AUDIO_MODE_RESUME_DRAIN:
        wait_drain(mode, OMNI_AUDIO_MODE_RESUME_GAP, now_ms);
        break;
    case OMNI_AUDIO_MODE_RESUME_GAP:
        if (accepted(mode) && (uint32_t)(now_ms - mode->phase_ms) >= OMNI_AUDIO_MODE_INTERFRAME_MS)
            transition(mode, OMNI_AUDIO_MODE_SEND_RATE, now_ms);
        break;
    case OMNI_AUDIO_MODE_SEND_RATE: {
        const uint8_t rate_frame[] = {0xbd, 6, 0x51, 1, 1, mode->sample_rate == 96000u ? 1u : 0u};
        send_frame(mode, rate_frame, sizeof(rate_frame), OMNI_AUDIO_MODE_RATE_DRAIN, now_ms);
        break;
    }
    case OMNI_AUDIO_MODE_RATE_DRAIN:
        wait_drain(mode, OMNI_AUDIO_MODE_RATE_GAP, now_ms);
        break;
    case OMNI_AUDIO_MODE_RATE_GAP:
        /* Keep exclusive transport ownership through the last queue cooldown.
         * GATE already receives a longer 75ms quiet interval before any send. */
        if (accepted(mode) && (uint32_t)(now_ms - mode->phase_ms) >= OMNI_AUDIO_MODE_INTERFRAME_MS)
            transition(mode, OMNI_AUDIO_MODE_LOCAL_COMPLETE, now_ms);
        break;
    default:
        break;
    }
}

void omni_audio_mode_cancel(omni_audio_mode_t *mode)
{
    if (mode == NULL || !mode->initialized || !active(mode)) return;
    mode->state = OMNI_AUDIO_MODE_CANCELED;
    isolate(mode);
}
