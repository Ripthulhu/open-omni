#ifndef OMNI_AUDIO_PROBE_H
#define OMNI_AUDIO_PROBE_H
#include <stdint.h>
#include <stdbool.h>
#include "audio_format.h"
#include "audio_usb_layout.h"
extern uint8_t omni_audio_config[OMNI_AUDIO_CONFIG_LENGTH];
void audio_probe_poll(void);
/* Synchronous packet delivery from the dedicated, already-rearmed ISO bank. */
void audio_probe_iso_receive(const uint8_t *pcm,uint16_t length);
void audio_probe_iso_receive_format(const uint8_t *pcm,uint16_t length,uint32_t epoch);
/* Always fills the complete desired format; result says configured/open.
 * Callers compare epoch even if stop/reopen occurred between main-loop polls. */
bool audio_probe_playback_format(omni_audio_format *out);
void audio_probe_format_status(uint32_t out[15]);
void audio_probe_status(uint8_t out[32]);
int audio_probe_local(int16_t volume, uint8_t mute);
int audio_probe_alternate(uint8_t interface_number);
/* Latched receive ownership/rearm failure; cleared by reset or next alt-open. */
int audio_probe_playback_failed(void);
void audio_probe_request_trace(uint8_t index, uint8_t out[48]);
void audio_probe_error_trace(uint8_t index, uint8_t out[60]);
void audio_probe_clock_snapshot(uint8_t out[60]);
int audio_probe_endpoint_snapshot(uint8_t endpoint,uint8_t page,uint8_t out[60]);
void audio_probe_volume_snapshot(int16_t *db,uint8_t *mute);
/* Main-loop peer updates compare the captured master revision atomically;
 * a newer USB/local change wins over an older asynchronous peer report. */
void audio_probe_master_snapshot(int16_t *db,uint8_t *mute,uint32_t *revision);
int audio_probe_peer_volume(int16_t db,uint8_t mute,uint32_t expected_revision);
void audio_probe_dial(int step);
#endif
