#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "../src/home_ui.c"
int main(void)
{
    assert(eq_x(20)==22 && eq_x(20000)==125);
    unsigned previous=0;
    for(unsigned hz=20;hz<=20000;++hz) {unsigned x=eq_x(hz);assert(x>=previous && x>=22 && x<=125);previous=x;}
    assert(eq_x(1000)>=79 && eq_x(1000)<=81);
    assert(eq_y(0)==39 && eq_y(120)==25 && eq_y(240)==11);
    struct {uint8_t before[16],frame[1024],after[16];} guarded;
    memset(&guarded,0xa5,sizeof(guarded));
    omni_eq_view v={.title="WIRELESS EQ",.parametric=true};
    strcpy(v.values[0],"-12.0dB");strcpy(v.values[1],"20000Hz");
    strcpy(v.values[2],"10.000");strcpy(v.values[3],"HI SHLF");
    for(unsigned i=0;i<10;++i){v.known[i]=true;v.gain[i]=i%2?240:0;v.frequency[i]=20000u-i*1900u;}
    for(unsigned selected=0;selected<=10;++selected)for(unsigned field=0;field<5;++field)for(unsigned edit=0;edit<2;++edit) {
        v.selected=selected;v.field=field;v.editing=edit!=0;v.apply=selected==10;
        omni_eq_ui_render(guarded.frame,&v);
        for(unsigned j=0;j<16;++j)assert(guarded.before[j]==0xa5 && guarded.after[j]==0xa5);
    }
    v.frequency[0]=20001;v.frequency[1]=UINT_MAX;v.gain[2]=UINT_MAX;v.known[3]=false;
    v.apply=false;v.selected=0;omni_eq_ui_render(guarded.frame,&v);
    v.parametric=false;v.status="SETTING FAILED";omni_eq_ui_render(guarded.frame,&v);
    omni_eq_ui_render(guarded.frame,0);for(unsigned i=0;i<1024;++i)assert(guarded.frame[i]==0);
    puts("EQ graph log axis, bounds, disabled bands and rendering guards pass");return 0;
}
