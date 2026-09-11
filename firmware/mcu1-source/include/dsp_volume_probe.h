#ifndef OMNI_DSP_VOLUME_PROBE_H
#define OMNI_DSP_VOLUME_PROBE_H
#include <stdbool.h>
#include <stdint.h>

/* Automatic once-per-boot read-only DSP discovery after playback handshake.
 * Main calls after playback_service has released UART3. No reset/peer GPIO
 * writes, manual trigger, SET, gain application or retry. Loss of stream or
 * permission terminates an active query and preserves its partial evidence. */
void omni_dsp_volume_probe_poll(uint32_t now_ms, bool playback_running, bool allowed);
bool omni_dsp_volume_probe_busy(void);
/* Read-only HID pages0..2,15 native little-endian words, ABI version1.
 * Stage1 fields may reflect an intermediate main-loop update; IRQ masking
 * in the reader does not make those multi-store updates atomic. Stage2 freezes
 * query result/counters; desired dB/mute/revision remain coherently updated.
 * UART FIFO/STAT samples are separate observations, not a hardware snapshot.
 * Invalid page/null returns false without mutating output. */
bool omni_dsp_volume_probe_status(unsigned page, uint32_t out[15]);
/* Explicit once-per-boot attenuation/restore trial. No UART activity in request.
 * Nonzero token is idempotent; a second token is refused until reboot.
 * Status pages 0..8 are sequential observations; frozen stage3 is final.
 * Page0 flags distinguish verified attenuation and verified restoration. */
bool omni_dsp_gain_trial_request(uint32_t token);
bool omni_dsp_gain_restore_request(uint32_t token,const uint8_t expected[4],const uint8_t original[4]);
bool omni_dsp_gain_trial_status(unsigned page, uint32_t out[15]);
#endif
