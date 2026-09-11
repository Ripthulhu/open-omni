#ifndef OMNI_NATIVE_GAIN_H
#define OMNI_NATIVE_GAIN_H
#include "dsp_gain_trial.h"
#define OMNI_NATIVE_MIN_DB (-55*256)
#define OMNI_NATIVE_MAX_DB 0
#define OMNI_NATIVE_STEP_DB 256
/* USB range follows headset E085/E011. Analog E089/E05D clamps below its
 * -49dB relative floor; this function is lineout gain only. Wire0 is a finite
 * minimum (-90dB relative), used for prototype mute, NOT a proven hard mute. */
bool omni_native_gain_wire(int16_t db,bool muted,uint8_t *wire);
typedef struct {
    void *context;
    bool (*open)(void *,omni_dsp_volume_io *);
    void (*close)(void *);
} omni_native_transport;
typedef struct {
    omni_native_transport transport;
    omni_gain_trial transfer;
    uint32_t desired_revision,submitted_revision,verified_revision;
    uint32_t transactions,verified_transactions,failures,cancellations,last_verified_ms,epoch;
    uint8_t input_mask,input_levels[4],submitted_mask,submitted_levels[4],verified_mask,verified_levels[4];
    int16_t desired_db;
    uint8_t desired_muted,desired_wire,submitted_wire,verified_wire;
    bool initialized,online,owned,ready,fault,have_verified,yielding;
} omni_native_gain;
bool omni_native_gain_init(omni_native_gain *,omni_native_transport);
bool omni_native_gain_inputs(omni_native_gain *,uint8_t mask,const uint8_t levels[4]);
void omni_native_gain_poll(omni_native_gain *,uint32_t now,bool online,int16_t db,bool muted);
void omni_native_gain_status(const omni_native_gain *,unsigned page,uint32_t out[15]);
#endif
