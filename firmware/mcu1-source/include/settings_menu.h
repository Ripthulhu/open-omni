#ifndef OMNI_SETTINGS_MENU_H
#define OMNI_SETTINGS_MENU_H
#include <stdbool.h>
#include <stdint.h>
#include "controls.h"
typedef struct {
    bool (*read)(unsigned id, unsigned *value);
    bool (*write)(unsigned id, unsigned value);
    const char *(*status)(void);
} omni_settings_menu_io;
void omni_settings_menu_bind(omni_settings_menu_io io);
void omni_settings_menu_bind_native(void);
bool omni_settings_menu_enabled(void);
bool omni_settings_menu_open(void);
void omni_settings_menu_begin(void);
void omni_settings_menu_close(void);
bool omni_settings_menu_event(omni_control_kind_t kind);
void omni_settings_menu_dial(int step);
void omni_settings_menu_render(uint8_t frame[1024]);
#endif
