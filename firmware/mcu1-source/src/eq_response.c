#include "eq_response.h"
#include <stdint.h>
#include "eq_response_tables.inc"
#define ONE (1u<<20)
/* tan(pi*f/48000), linearly interpolated on a 40 Hz grid, Q24. */
static uint32_t warped(unsigned hz)
{
    if(hz>=20000u)return warp[500];
    unsigned i=hz/40u,rem=hz%40u;
    return warp[i]+(uint32_t)(((uint64_t)(warp[i+1u]-warp[i])*rem+20u)/40u);
}
static uint64_t square(int64_t n) {return (uint64_t)(n*n);}
static int logarithm(uint64_t n)
{
    if(!n)return -128*4096;
    unsigned exponent=63u-(unsigned)__builtin_clzll(n);
    uint64_t scaled=exponent>=16u?n>>(exponent-16u):n<<(16u-exponent);
    unsigned fraction=(unsigned)scaled-65536u,index=fraction>>8,rem=fraction&255u;
    return (int)(exponent*4096u+log_fraction[index]+
        ((unsigned)(log_fraction[index+1u]-log_fraction[index])*rem+128u)/256u);
}
unsigned omni_eq_response_frequency(unsigned column)
{return plot_frequency[column<104u?column:103u];}
int omni_eq_response_band(unsigned hz,unsigned gain,unsigned q,unsigned type,unsigned probe_hz)
{
    if(hz<20u || hz>20000u || probe_hz<20u || probe_hz>20000u ||
       gain>247u || q<200u || q>10000u || type<1u || type>5u)return 0;
    if((type==1u || type>=4u) && gain==120u)return 0;
    uint32_t center=warped(hz),probe=warped(probe_hz),a=amplitude[gain];
    unsigned above=probe>center;
    uint32_t r=(uint32_t)(((uint64_t)(above?center:probe)<<20)/(above?probe:center));
    uint32_t r2=(uint32_t)(((uint64_t)r*r)>>20);
    uint32_t t=(uint32_t)(((uint64_t)r*1000u)/q);
    uint64_t delta=square((int64_t)ONE-r2),num,den;
    int extra=0;
    if(type==1u) {
        num=delta+square((int64_t)(((uint64_t)t*a)>>20));
        den=delta+square((int64_t)(((uint64_t)t<<20)/a));
    } else if(type==2u || type==3u) {
        unsigned use_r4=(type==2u)?above:!above;
        num=use_r4?square(r2):square(ONE);den=delta+square(t);
    } else {
        uint64_t damping=((square(t)>>10)*a)>>10;
        int64_t ar2=(int64_t)(((uint64_t)a*r2)>>20);
        num=square(above?ar2-ONE:(int64_t)a-r2)+damping;
        den=square(above?(int64_t)r2-a:(int64_t)ONE-ar2)+damping;
        if(type==5u){uint64_t swap=num;num=den;den=swap;}
        extra=2*(logarithm(a)-20*4096);
    }
    int64_t db10=(int64_t)(logarithm(num)-logarithm(den)+extra)*301030;
    int result=(int)(db10/(4096*1000));
    return result<-30000?-30000:result>30000?30000:result;
}
