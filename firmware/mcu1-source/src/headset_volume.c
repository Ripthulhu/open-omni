#include "headset_volume.h"
bool omni_headset_volume_step(int16_t db,bool muted,uint8_t *step)
{
    if(!step || db<-55*256 || db>0 || db%256) return false;
    *step=(uint8_t)(muted?0:56+db/256);return true;
}
bool omni_headset_volume_db(uint8_t step,int16_t remembered_db,
                            int16_t *db,uint8_t *muted)
{
    if(!db || !muted || step>56u || remembered_db<-55*256 ||
       remembered_db>0 || remembered_db%256) return false;
    *db=step?(int16_t)(((int)step-56)*256):(int16_t)(-55*256);
    *muted=(uint8_t)(step==0u);return true;
}
