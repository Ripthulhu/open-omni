#ifndef OMNI_HOME_UI_H
#define OMNI_HOME_UI_H
#include <stdbool.h>
#include <stdint.h>

/* Pure, bounded 128x64 OLED renderer. Bytes are page-major, bit0 top pixel.
 * Snapshots are caller-owned; this module never accesses hardware/state. */
typedef struct {
    char label[7];                  /* At most six uppercase characters. */
    bool known;                    /* False: dash, never fabricated activity. */
    uint8_t level;                  /* Real meter magnitude, normalized0..100. */
} omni_home_meter;
typedef struct {
    char source[6];                 /* USB1 / USB2 / USB3 / LINE / --. */
    bool link_known, connected;
    bool bluetooth_known;
    uint8_t bluetooth_state;        /* Exact passive14/03 raw0x30..0x35. */
    bool battery_known;
    uint8_t battery_percent;
    uint8_t spare_state;            /*0 absent,1 present,2 charging,3 full,4 fault. */
    uint16_t spare_millivolts;       /* Fresh stock ADC conversion;0 unavailable. */
    uint8_t percent;
    int16_t db_x256;
    bool muted;
    uint32_t sample_rate;           /* Actual format;0 means unknown. */
    uint8_t sample_bits;
    bool stereo_view;
    omni_home_meter input[4];
    bool stereo_known;
    uint8_t left, right;
    bool bias_mode, bias_known;
    uint8_t bias;                   /* Position0..24;12center. + favors current MCU2 USB2/3. */
    uint8_t secondary_source;       /* Observed active port2/3;0 means unknown. */
    uint16_t bias_q14[2];           /* Desired linear gains0..16384, not detent percentages. */
    bool gain_known, gain_ready, gain_fault;
} omni_home_view;

typedef struct {
    uint8_t level;
    bool available, linked, muted;
} omni_menu_input;
typedef struct {
    omni_menu_input input[4];        /* USB1, USB2, USB3, LINE. */
    uint8_t selected;               /*0..3; invalid clamps to0. */
    bool detail;
    uint8_t field;                  /*0 level,1 master link,2 mute. */
    bool editing;
} omni_menu_view;

typedef struct {
    const char *title,*labels[4],*status;
    char values[4][12];
    unsigned selected,count;
    bool editing;
} omni_settings_view;
void omni_settings_ui_render(uint8_t frame[1024],const omni_settings_view *view);
typedef struct {
    const char *title,*status;
    unsigned gain[10],frequency[10]; /* Gain offset: 120 = 0 dB; frequency in Hz. */
    bool known[10],parametric,editing,apply;
    unsigned selected,field; /* Field 0 gain,1 frequency,2 Q,3 filter;4 overview. */
    char values[4][12];
} omni_eq_view;
void omni_eq_ui_render(uint8_t frame[1024],const omni_eq_view *view);

void omni_home_ui_render(uint8_t frame[1024], const omni_home_view *view);
void omni_menu_ui_render(uint8_t frame[1024], const omni_menu_view *view);
#endif
