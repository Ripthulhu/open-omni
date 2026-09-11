#ifndef OMNI_HEADSET_QUERY_H
#define OMNI_HEADSET_QUERY_H
#include <stdbool.h>
#include <stdint.h>

#define OMNI_HEADSET_QUERY_QUEUE_MS 2000u
#define OMNI_HEADSET_QUERY_TIMEOUT_MS 250u
#define OMNI_HEADSET_QUERY_MAX_REPLY 46u
typedef enum {
    HEADSET_QUERY_IDLE,HEADSET_QUERY_QUEUED,HEADSET_QUERY_SEND,
    HEADSET_QUERY_DRAIN,HEADSET_QUERY_WAIT,HEADSET_QUERY_DONE,
    HEADSET_QUERY_TIMEOUT,HEADSET_QUERY_CANCELLED,HEADSET_QUERY_IO_ERROR,
    HEADSET_QUERY_NACK,HEADSET_QUERY_INVALID_REPLY
} omni_headset_query_phase;
typedef struct {
    void *context;
    int (*tx)(void *,uint8_t);
    int (*tx_complete)(void *);
} omni_headset_query_io;

/* Exact read whitelist IDs:1 E1,2 E4,3 bulk20,4 bulk80,5 ANC,6 transparency,
 * 7 ANClevel,8 micstate,9 micvolume,10 sidetonebank1,11 sidetonebank2.
 * Admission/build identity belongs to HID's caller. This function does not
 * access the UART. Same token/profile is idempotent; changed profile rejects.
 * A new nonzero token replaces frozen evidence only when no request is busy. */
bool omni_headset_query_request(uint32_t token,unsigned profile,uint32_t now);
bool omni_headset_query_busy(void);
bool omni_headset_query_active(void);
bool omni_headset_query_transport_fault(void);
/* Sole UART reader copies the SAME bytes to this observer and battery/gain.
 * A queued request consumes no replies; start requires proven RX exhaustion,
 * a frame boundary,20ms since physical TXIDLE,and no competing owner. */
void omni_headset_query_observe(uint8_t byte,uint32_t now);
void omni_headset_query_poll(uint32_t now,bool can_start,omni_headset_query_io io);
/* Cooperative yield cancels queued/unsent work; partial TX completes and
 * drains before cancellation,or faults at its deadline. No automatic retry. */
void omni_headset_query_yield(uint32_t now);
void omni_headset_query_release(uint32_t now);
void omni_headset_query_acquire(void);
void omni_headset_query_io_error(uint32_t now);
/* Read-only,15-word pages. Page0:version,page,token,profile,phase,flags,
 * queued_ms,started_ms,finished_ms,tx_bytes,rx_bytes,matched_length,
 * unrelated,invalid,peer_status(255 absent).
 * Flags:0busy,1active,2matched,3transportfault,4cancelrequested.
 * Page1:version,page,token,profile,phase,received_ms,drained_ms,
 * parser_frames,parser_malformed,parser_expired,ack_count,timeout_reason
 * (0none,1queue,2request),request_length,raw_request[8] inwords13/14.
 * Timestamps are main-loop observations. A matching report can be unsolicited:
 * the protocol has no transaction ID and these deadlines are local policy. */
bool omni_headset_query_status(unsigned page,uint32_t now,uint32_t out[15]);
/* Terminal-only matching-token raw evidence:3 LE words version,token,length,
 * then48 bytes zero padded. Returns false without a retained matching reply. */
bool omni_headset_query_reply(uint32_t token,uint8_t out[60]);
#endif
