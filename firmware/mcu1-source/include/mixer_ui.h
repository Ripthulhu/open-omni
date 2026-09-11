#ifndef OMNI_MIXER_UI_H
#define OMNI_MIXER_UI_H
#include "controls.h"
#include "source_mix.h"
bool omni_mixer_ui_configure(unsigned input,unsigned level,bool linked,bool muted);
bool omni_mixer_ui_toggle_line_mute(void);
bool omni_mixer_ui_event(omni_control_kind_t);
/* Short home click switches master/source-bias control; long hold opens the
 * menu. Remote D209 short clicks are consumed only at home. The line-output
 * list omits unavailable routes; selecting a route starts level editing.
 * Click finishes editing; Back finishes editing, then returns one page. */
bool omni_mixer_ui_control(const omni_control_event_t *event);
bool omni_mixer_ui_dial(int step);
bool omni_mixer_ui_render(uint8_t frame[1024]);
uint32_t omni_mixer_ui_revision(void);
bool omni_mixer_ui_open(void);
void omni_mixer_ui_targets(int16_t master_db,bool muted,uint8_t levels[4]);
void omni_mixer_ui_status(int16_t master_db,bool muted,uint32_t out[15]);
/* Single main-loop owner, independent of physical/DSP transport. */
bool omni_mixer_ui_bias_set(bool enabled,unsigned position);
void omni_mixer_ui_bias_snapshot(omni_source_mix *state,omni_source_mix_gains *gains);
#endif
