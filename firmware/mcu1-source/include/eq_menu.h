#ifndef OMNI_EQ_MENU_H
#define OMNI_EQ_MENU_H
#include "settings_menu.h"
/* Menu-local field IDs; never DSP or HID command IDs. */
#define OMNI_EQ_FIELD_BASE 128u
#define OMNI_EQ_FIELD_STRIDE 64u
#define OMNI_EQ_BEGIN 40u
#define OMNI_EQ_APPLY 41u
#define OMNI_EQ_FLAT 42u
#define OMNI_EQ_DISCARD 43u
void omni_eq_menu_begin(unsigned control,omni_settings_menu_io io);
void omni_eq_menu_close(void);
bool omni_eq_menu_open(void);
void omni_eq_menu_event(omni_control_kind_t kind);
void omni_eq_menu_dial(int step);
void omni_eq_menu_render(uint8_t frame[1024]);
#endif
