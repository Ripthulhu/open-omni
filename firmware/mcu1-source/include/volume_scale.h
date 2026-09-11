#ifndef OMNI_VOLUME_SCALE_H
#define OMNI_VOLUME_SCALE_H
#include <stdint.h>
/* UI percentage profile; USB state remains signed 1/256 dB. */
unsigned omni_volume_percent(int16_t db);
int16_t omni_volume_percent_db(unsigned percent);
#endif
