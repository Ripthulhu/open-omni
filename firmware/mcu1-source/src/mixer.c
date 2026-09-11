#include "mixer.h"
void omni_mixer_init(omni_mixer *m)
{
    *m=(omni_mixer){0};
    for(unsigned i=0;i<OMNI_MIX_COUNT;i++) m->input[i].level=100;
    m->input[OMNI_MIX_USB1].linked=true;
}
bool omni_mixer_available(unsigned i) { return i==OMNI_MIX_USB1 || i==OMNI_MIX_LINE; }
bool omni_mixer_set(omni_mixer *m,unsigned i,unsigned level,bool linked,bool muted)
{
    if(!m || !omni_mixer_available(i) || level>100) return false;
    omni_mix_input *s=&m->input[i];
    if(s->level!=level || s->linked!=linked || s->muted!=muted) {
        s->level=(uint8_t)level;s->linked=linked;s->muted=muted;++m->revision;
    }
    return true;
}
unsigned omni_mixer_effective(const omni_mixer *m,unsigned i,unsigned master,bool muted)
{
    if(!m || !omni_mixer_available(i) || master>100) return 0;
    const omni_mix_input *s=&m->input[i];
    if(s->muted || (s->linked && muted)) return 0;
    return s->linked ? (s->level*master+50u)/100u : s->level;
}
