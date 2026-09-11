#include "native_gain.h"
/* Generated from hash-checked AB15 E089/E05D, stock DSP 320dace4ddd866e30787c2ac4220f0b5b4e6d51c48dc2568ce3f6566c5b2c5f2. */
static const uint8_t wire_by_db[]={
    1,3,7,10,14,17,21,25,26,28,
    30,32,34,35,37,39,41,42,44,46,
    48,50,51,53,55,57,59,60,62,64,
    66,67,69,71,73,75,76,78,80,82,
    84,85,87,89,91,92,94,96,98,100
};
bool omni_native_gain_wire(int16_t db,bool muted,uint8_t *wire)
{
    if(!wire || db<OMNI_NATIVE_MIN_DB || db>0 || db%256) return false;
    /* The independent lineout curve has six fewer low whole-dB steps. */
    int index=(db+49*256)/256;if(index<0) index=0;
    *wire=muted?0u:wire_by_db[index];return true;
}
