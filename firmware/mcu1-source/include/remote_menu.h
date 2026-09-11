#ifndef OMNI_REMOTE_MENU_H
#define OMNI_REMOTE_MENU_H
#include "interchip.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OMNI_REMOTE_MENU_QUEUE_MS 2000u
#define OMNI_REMOTE_MENU_TIMEOUT_MS 500u
#define OMNI_REMOTE_MENU_GAP_MS 20u
typedef enum {
    REMOTE_MENU_IDLE,REMOTE_MENU_QUEUED,REMOTE_MENU_SEND,REMOTE_MENU_DRAIN,
    REMOTE_MENU_WAIT,REMOTE_MENU_GAP,REMOTE_MENU_ACCEPTED,REMOTE_MENU_TIMEOUT,
    REMOTE_MENU_CANCELLED,REMOTE_MENU_IO_ERROR,REMOTE_MENU_NACK,REMOTE_MENU_INVALID_REPLY
} omni_remote_menu_phase;
typedef struct {
    void *context;
    int (*tx)(void *,uint8_t);
    int (*drained)(void *);
} omni_remote_menu_io;
typedef struct {
    omni_link_parser parser;
    omni_remote_menu_phase phase;
    uint8_t frames[2][4],count,index,offset,context,pending_context,peer_status;
    bool desired_valid,desired_open,mode_known,mode_open,planned_open;
    bool pending,pending_open,pending_force,fault,cancel,ack,negative,bad_reply;
    bool frame_eligible,have_link,connected;
    uint32_t token,pending_token,queued_ms,started_ms,drained_ms;
    uint32_t local_token,local_context,serial,last_token,last_result;
    bool local_open;
    uint32_t tx_bytes,acks,timeouts,nacks,invalid,unrelated,coalesced;
    uint32_t peer_requests,duplicates,completed,generation;
} omni_remote_menu;

bool omni_remote_menu_init(omni_remote_menu *);
/* Single cooperative owner. Latest local token (<0x80000000) is idempotent
 * only with identical values. Context0 omits92;1/2 are opaque captured actions,
 * NOT menu depths. When accepted mode differs/unknown, send910A(open)/9109(close)
 * before92. An active sequence is immutable. A pending newer desired context
 * replaces the older pending intent, counting coalescence; UI actions belong
 * in the UI queue, not here. No traffic occurs in request/observe callbacks. */
bool omni_remote_menu_request(omni_remote_menu *,uint32_t token,bool open,
                              unsigned context,uint32_t now);
/* Complete raw frames: DD03opcode,status; DB04910A/08; DB05E403state1..3.
 * Peer enter/exit uses the exact captured/static echo+92 sequence. Duplicates
 * coalesce while that context is pending/active; a new request after completion
 * receives its echo again, without toggling explicit desired-open state.
 *04..07 navigation is owned by UI and deliberately does not imply92 depth.
 * E4 disconnect invalidates accepted mode; reconnect while desired-open
 * restores910A only, as captured. ACK proves DSP acceptance, not headset readback.
 * Prefer byte() when sharing the continuous UART stream: it also rejects an
 * ACK whose first byte arrived before our final TX byte was accepted. */
bool omni_remote_menu_observe(omni_remote_menu *,const uint8_t *,size_t,uint32_t now);
void omni_remote_menu_byte(omni_remote_menu *,uint8_t byte,uint32_t now);
bool omni_remote_menu_busy(const omni_remote_menu *);
bool omni_remote_menu_active(const omni_remote_menu *);
bool omni_remote_menu_frame_pending(const omni_remote_menu *);
/* can_start requires shared UART ownership, RX exhaustion/frame boundary and
 * TXidle spacing. It gates both a new intent and the second frame; this client
 * retains ownership during GAP. Each poll sends at most4bytes. No retry loop.
 * Partial transmission failure latches fault until release+acquire at a real
 * stopped/resynchronized UART boundary. The adapter must enforce exclusivity. */
void omni_remote_menu_poll(omni_remote_menu *,uint32_t now,bool can_start,omni_remote_menu_io);
void omni_remote_menu_yield(omni_remote_menu *,uint32_t now);
void omni_remote_menu_release(omni_remote_menu *,uint32_t now);
void omni_remote_menu_acquire(omni_remote_menu *);
void omni_remote_menu_io_error(omni_remote_menu *,uint32_t now);
/* Page0: version,page,phase,flags,activeToken,pendingToken,desiredOpen/255,
 * acceptedOpen/255,activeContext,pendingContext,frameIndex,offset,start,drain,status.
 * Flags: active1,pending2,fault4,cancel8,desiredKnown16,acceptedKnown32,
 * linkKnown64,connected128,ack256. Page1:version,page,txbytes,acks,timeouts,nacks,
 * invalid,unrelated,coalesced,peerrequests,duplicates,completed,lasttoken,
 * lastresult,generation. Terminal result can coexist with a pending intent. */
bool omni_remote_menu_status(const omni_remote_menu *,unsigned page,uint32_t out[15]);
#endif
