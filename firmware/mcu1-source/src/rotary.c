#include "rotary.h"
bool omni_rotary_init(omni_rotary *r,uint8_t phase,uint8_t edges)
{
    if (!r || phase>3 || (edges!=2 && edges!=4)) return false;
    r->previous=phase; r->edges=edges; r->accumulated=0; r->invalid=0; return true;
}
int omni_rotary_sample(omni_rotary *r,uint8_t phase)
{
    static const int8_t delta[16]={0,1,-1,0,-1,0,0,1,1,0,0,-1,0,-1,1,0};
    if (phase>3) { ++r->invalid; r->accumulated=0; return 0; }
    uint8_t previous=r->previous; r->previous=phase;
    if ((previous^phase)==3) { ++r->invalid; r->accumulated=0; return 0; }
    r->accumulated=(int8_t)(r->accumulated+delta[previous*4U+phase]);
    if (r->accumulated>=(int8_t)r->edges) { r->accumulated=0; return 1; }
    if (r->accumulated<=-(int8_t)r->edges) { r->accumulated=0; return -1; }
    return 0;
}
