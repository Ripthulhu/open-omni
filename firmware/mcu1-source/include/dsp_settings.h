#ifndef OMNI_DSP_SETTINGS_H
#define OMNI_DSP_SETTINGS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* IDs are native diagnostic API, not stock HID or DSP command numbers. */
typedef enum {
    DSP_SETTING_LIMITER = 1,       /* [enabled 0..1] */
    DSP_SETTING_MIC_VOLUME,        /* [level 1..10], not dB */
    DSP_SETTING_SIDETONE,          /* [enabled 0..1, level 1..10], boom/bank1 */
    DSP_SETTING_MIC_NOISE,         /* [enabled 0..1, level 1..3] */
    DSP_SETTING_ANC_STATE,         /* [0 off,1 transparency,2/3/4 ANC level3/2/1] */
    DSP_SETTING_ANC_LEVEL,         /* [level 1..3] */
    DSP_SETTING_TRANSPARENCY,      /* [level 1..10] */
    /* E3 siblings MUST be an explicit complete snapshot [BT startup0..1,
     * mic muted LED0..10, call policy0..2]. Selector changes with the ID.
     * Never fill unknown sibling fields from guessed defaults. */
    DSP_SETTING_BT_STARTUP,
    DSP_SETTING_MIC_LED,
    DSP_SETTING_BT_CALL,
    DSP_SETTING_AUTO_OFF,          /* [minutes 0,1,5,10,15,30,60] */
    DSP_SETTING_EQ_WIRELESS,       /* complete 128-byte blob */
    DSP_SETTING_EQ_MIC,            /* complete 78-byte blob */
    DSP_SETTING_EQ_BT,             /* complete 78-byte blob */
    DSP_SETTING_OUTPUT_MODE,       /* [1 Speakers,2 Streaming], gain owner only */
    DSP_SETTING_MIC_STATE,         /* READ ONLY: [0 active,1 muted/retracted] */
    DSP_SETTING_BT_STATE,          /* READ ONLY: stock14/03 raw0x30..0x35.
                                   * 30off,31link lost,32pairing,33link ready,
                                   * 34/35link busy (stock GG translation). */
    DSP_SETTING_HOME_MODE,         /* [0/1] alternate home knob context.
                                   * D209 ACK means local dispatch ONLY: stock
                                   * masks a remote-forwarding failure. No SET echo. */
    DSP_SETTING_VP_LEVEL,          /* READ ONLY: stock D2/0B voice-prompt level.
                                   * Passive cache of the raw DB byte; frame layout
                                   * (subcmd 0x0B) confirmed on hardware 2026-09-12;
                                   * range/unit/persistence still unproven, so
                                   * encode() has NO writer case. Distinct D2
                                   * subcommand from master gain (D2/03)/home (D2/09). */
    DSP_SETTING_COUNT
} omni_dsp_setting;

typedef enum {
    DSP_SETTINGS_IDLE, DSP_SETTINGS_QUEUED, DSP_SETTINGS_SEND,
    DSP_SETTINGS_DRAIN, DSP_SETTINGS_WAIT, DSP_SETTINGS_VERIFY_QUEUED,
    DSP_SETTINGS_ACCEPTED,
    DSP_SETTINGS_TIMEOUT, DSP_SETTINGS_CANCELLED, DSP_SETTINGS_IO_ERROR,
    DSP_SETTINGS_NACK, DSP_SETTINGS_INVALID_REPLY
} omni_dsp_settings_phase;
typedef struct {
    void *context;
    int (*tx)(void *, uint8_t);
    int (*tx_complete)(void *);
} omni_dsp_settings_io;

#define OMNI_DSP_SETTINGS_MAX_VALUE 128u
#define OMNI_DSP_SETTINGS_QUEUE_MS 2000u
#define OMNI_DSP_SETTINGS_TIMEOUT_MS 500u
/* SET43 ACK precedes original DSP event27/35 with scheduler delay500.
 * Its clock units remain unproven; these are conservative host policy,
 * followed by actual mode readback, not a remote-application timing claim. */
#define OMNI_DSP_SETTINGS_MODE_SETTLE_MS 550u
#define OMNI_DSP_SETTINGS_MODE_TIMEOUT_MS 1250u

/* Single cooperative main-loop owner, no UART access here. Nonzero token is
 * idempotent ONLY with identical ID/value. While QUEUED a newer token for
 * the SAME ID replaces its pending value; active sends cannot be replaced.
 * Validation failure leaves all state untouched. No implicit retry or save.
 * Master gain and lineout47 remain with native_gain/mixer ownership. Mode43
 * requests must come from that owner; ACCEPTED requires SET ACK then exact
 * the mode-specific settling guard, then GET readback+ACK. Other setters
 * distinguish ACK from observed readback and keep the 500ms deadline.
 * HOME_MODE's ACCEPTED/ack cache means local dispatch only, not peer acceptance.
 * No pairing, factory reset, raw opcode, bootloader or power-off API. */
bool omni_dsp_settings_request(uint32_t token, unsigned control,
                               const uint8_t *value, size_t length, uint32_t now);
/* Central monotonic token source for EVERY internal producer (native-gain mode
 * owner, settings menu, mixer home-mode). Returns nonzero and always bit31-SET,
 * so it can never alias a host diagnostic token (those are forced bit31-CLEAR,
 * <0x80000000). One source keeps two producers' distinct in-flight requests
 * from colliding on the idempotence guard above. Cooperative main-loop only. */
uint32_t omni_dsp_settings_next_token(void);
/* Pure bounded validation, safe for an IRQ admission mailbox. It does not
 * initialize or access transaction state and does not transmit anything. */
bool omni_dsp_settings_valid(unsigned control, const uint8_t *value, size_t length);
/* Build deterministic preset/custom payloads without captured device data.
 * EQ custom slot4/8/4; parametric: ten freqLE16,type,gainInt8,QLE16 records.
 * Names occupy 6+61 bytes and need not be NUL terminated. Returns 0 on failure.
 * Built-ins use names/coefficients extracted by original MCU1 instructions;
 * 0/1 match live capture in each channel. Custom selects native flat data.
 * Unsupported wireless index5 is rejected. Built-in caller blobs must match
 * extracted data; edits belong in custom4/8/4. eq_flat is a compatibility name
 * for this same preset builder; eq_preset describes the behavior clearly. */
size_t omni_dsp_settings_eq_flat(unsigned control, unsigned preset,
                                uint8_t out[OMNI_DSP_SETTINGS_MAX_VALUE]);
size_t omni_dsp_settings_eq_preset(unsigned control, unsigned preset,
                                  uint8_t out[OMNI_DSP_SETTINGS_MAX_VALUE]);

bool omni_dsp_settings_busy(void);
bool omni_dsp_settings_active(void);
bool omni_dsp_settings_transport_fault(void);
bool omni_dsp_settings_frame_pending(void);
void omni_dsp_settings_expire(uint32_t now);
/* Forward the SAME RX bytes consumed by gain/battery. Observe also when idle
 * to retain exact known DB fields and raw E4 link-state reports. */
void omni_dsp_settings_observe(uint8_t byte, uint32_t now);
/* can_start must reflect RX exhaustion/frame boundary/TXidle spacing/no OTHER
 * owner, including during VERIFY_QUEUED. That stage retains UART ownership. */
void omni_dsp_settings_poll(uint32_t now, bool can_start, omni_dsp_settings_io io);
void omni_dsp_settings_yield(uint32_t now);
/* release requires owner has stopped/resynchronized hardware. acquire clears
 * fault only at a newly established UART boundary; it does not send defaults. */
void omni_dsp_settings_release(uint32_t now);
void omni_dsp_settings_acquire(void);
void omni_dsp_settings_io_error(uint32_t now);
bool omni_dsp_settings_link_state(uint8_t *raw, uint32_t *observed_ms);

/* Status0: version,page,token,control,phase,flags,queued,started,finished,
 * txbytes,ackcount,unrelated,invalid,peerstatus,coalesced.
 * Status1: version,page,token,control,phase,drained,rxbytes,parserframes,
 * parsermalformed,parserexpired,timeouts,requestlength,valueLength,0,0.
 * flags bit0busy/1active/2transportfault/3cancel/4ack.
 * ACCEPTED requires matching DD03 opcode00, not independent DSP readback,
 * remote acoustic application, persistence or matching a transaction ID. */
bool omni_dsp_settings_status(unsigned page, uint32_t out[15]);
/* Cache: six LE words version,control,page,totalLength,flags,observed_ms,
 * then36 value bytes. flags1 valid,2 DD accepted,4 DB observed.
 * Unknown fields are absent. DB reports are passive observations, not proof
 * of a reply to our last SET. Values remain snapshots until invalidated. */
bool omni_dsp_settings_value(unsigned control, unsigned page, uint8_t out[60]);
/* Last accepted complete custom curve in this boot, retained across preset
 * selection. This is a local copy, not nonvolatile storage or a remote GET. */
size_t omni_dsp_settings_custom(unsigned control,uint8_t out[128]);
#define DSP_EQ_NVM_PAYLOAD 287u
uint32_t omni_dsp_settings_eq_generation(void);
size_t omni_dsp_settings_eq_serialize(uint8_t out[DSP_EQ_NVM_PAYLOAD]);
void omni_dsp_settings_eq_deserialize(const uint8_t *in,size_t length,uint32_t now);
#endif
