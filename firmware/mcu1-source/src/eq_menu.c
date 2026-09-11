#include "eq_menu.h"
#include "home_ui.h"
#include <string.h>

static omni_settings_menu_io io;
static unsigned control,depth,row,band,value,saved_value,confirm_choice;
static bool opened,editing,feedback,dirty,confirm;
static const char *message;
static unsigned base(void) {return OMNI_EQ_FIELD_BASE+(control-12u)*OMNI_EQ_FIELD_STRIDE;}
static unsigned field(void) {return base()+band+(depth==2u?row*10u:0u);}
static unsigned count(void) {return depth==0u?3u:depth==1u?11u:4u;}
static unsigned selected_id(void) {return depth?field():control;}
static void limits(unsigned *min,unsigned *max)
{
    *min=0;*max=depth?247u:control==13u?9u:4u;
    if(depth==2u && row==1u) {*min=20;*max=20001;}
    if(depth==2u && row==2u) {*min=200;*max=10000;}
    if(depth==2u && row==3u) {*min=1;*max=6;}
}
void omni_eq_menu_begin(unsigned id,omni_settings_menu_io callbacks)
{
    control=id;io=callbacks;opened=id>=12u && id<=14u;
    depth=row=band=0;editing=feedback=dirty=confirm=false;message=0;
}
void omni_eq_menu_close(void) {opened=false;editing=false;}
bool omni_eq_menu_open(void) {return opened;}
void omni_eq_menu_event(omni_control_kind_t kind)
{
    if(kind==OMNI_CONTROL_BACK) {
        if(confirm) {confirm=false;message=0;return;}
        if(editing) {if(depth && value!=saved_value) (void)io.write(selected_id(),saved_value);editing=false;}
        else if(depth==2u) {depth=1;row=band;}
        else if(depth==1u) {depth=0;row=1;}
        else if(dirty) {confirm=true;confirm_choice=0;message=0;return;}
        else opened=false;
        feedback=false;message=0;return;
    }
    if(kind!=OMNI_CONTROL_SELECT) return;
    if(confirm) {
        (void)io.write(base()+(confirm_choice?OMNI_EQ_DISCARD:OMNI_EQ_APPLY),0);
        confirm=false;dirty=false;opened=false;return;
    }
    message=0;
    if(editing) {
        if(io.write(selected_id(),value)) {editing=false;feedback=true;if(depth)dirty=true;}
        else message="UNAVAILABLE";
        return;
    }
    if(depth==0u && row) {
        if(io.write(base()+(row==1u?OMNI_EQ_BEGIN:OMNI_EQ_FLAT),0)) {
            depth=1;row=0;feedback=false;dirty=true;
        } else message="USE NEW FLAT";
        return;
    }
    if(depth==1u) {
        if(row==10u) {
            if(io.write(base()+OMNI_EQ_APPLY,0)) {feedback=true;dirty=false;}
            else message="UNAVAILABLE";
            return;
        }
        band=row;
        if(control==12u) {depth=2;row=0;feedback=false;return;}
    }
    if(!io.read(selected_id(),&value)) {message="NOT LOADED";return;}
    unsigned min,max;limits(&min,&max);
    if(value<min || value>max) {message="INVALID VALUE";return;}
    editing=true;feedback=false;saved_value=value;
}
void omni_eq_menu_dial(int step)
{
    if(!step) return;
    if(confirm) {confirm_choice=step<0?1u:0u;return;}
    message=0;
    if(!editing) {row=(row+(step<0?1u:count()-1u))%count();return;}
    unsigned min,max;limits(&min,&max);
    int64_t amount=-(int64_t)step;
    if(depth) {
        if(depth==1u || row==0u) amount*=5; /* Half-dB detents. */
        else if(row==1u) amount*=value<100u?1:value<1000u?10:100;
        else if(row==2u) amount*=50;
    }
    int64_t next=(int64_t)value+amount;
    value=next<(int64_t)min?min:next>(int64_t)max?max:(unsigned)next;
    if(depth) {(void)io.write(selected_id(),value);if(value!=saved_value)dirty=true;}
}
static void number(char *out,unsigned n)
{
    char digits[10];unsigned count=0;
    do {digits[count++]=(char)('0'+n%10u);n/=10u;} while(n);
    for(unsigned i=0;i<count;++i) out[i]=digits[count-1u-i];
    out[count]=0;
}
static void gain(char out[12],unsigned n)
{
    int signed_gain=(int)n-120;
    unsigned magnitude=(unsigned)(signed_gain<0?-signed_gain:signed_gain);
    out[0]=signed_gain<0?'-':'+';number(out+1,magnitude/10u);
    size_t at=strlen(out);out[at++]='.';out[at++]=(char)('0'+magnitude%10u);
    memcpy(out+at,"dB",3);
}
static void parameter(char out[12],unsigned kind,unsigned n)
{
    static const char *const filters[]={"PEAK","LO PASS","HI PASS","LO SHLF","HI SHLF","TYPE 6"};
    if(kind==0u) gain(out,n);
    else if(kind==1u) {
        if(n==20001u) strcpy(out,"OFF");
        else {number(out,n);strcat(out,"Hz");}
    } else if(kind==2u) {
        number(out,n/1000u);size_t at=strlen(out);out[at++]='.';
        out[at++]=(char)('0'+n/100u%10u);out[at++]=(char)('0'+n/10u%10u);
        out[at++]=(char)('0'+n%10u);out[at]=0;
    } else strcpy(out,n>=1u && n<=6u?filters[n-1u]:"--");
}
static void preset(char out[12],unsigned n)
{
    static const char *const eq[]={"FLAT","BASS","FOCUS","SMILEY","CUSTOM"};
    static const char *const mic[]={"FLAT","BALANCE","BCAST-H","BCAST-L","CLAR-L","CLAR-H","DEEP","NASAL","WALKIE","CUSTOM"};
    strcpy(out,control==13u?(n<10u?mic[n]:"--"):(n<5u?eq[n]:"--"));
}
static void graph(uint8_t frame[1024])
{
    omni_eq_view v={0};
    v.title=control==12u?"WIRELESS EQ":control==13u?"MIC EQ":"BLUETOOTH EQ";
    v.parametric=control==12u;v.editing=editing;v.apply=depth==1u && row==10u;
    v.selected=depth==1u?row:band;v.field=depth==2u?row:editing?0u:4u;
    for(unsigned i=0;i<10u;++i) {
        v.known[i]=io.read(base()+i,&v.gain[i]);
        if(v.parametric) v.known[i]=io.read(base()+10u+i,&v.frequency[i]) &&
            io.read(base()+20u+i,&v.q[i]) && io.read(base()+30u+i,&v.type[i]) && v.known[i];
        if(editing && i==v.selected) {
            if(depth==1u || row==0u)v.gain[i]=value;
            else if(row==1u)v.frequency[i]=value;
            else if(row==2u)v.q[i]=value;
            else if(row==3u)v.type[i]=value;
        }
    }
    if(!v.apply) for(unsigned kind=0;kind<4u;++kind) {
        unsigned n=0;
        bool known=io.read(base()+v.selected+10u*kind,&n);
        if(editing && kind==v.field){n=value;known=true;}
        if(known)parameter(v.values[kind],kind,n);else strcpy(v.values[kind],"--");
    }
    v.status=message?message:feedback && io.status?io.status():dirty?"DRAFT":0;
    omni_eq_ui_render(frame,&v);
}
void omni_eq_menu_render(uint8_t frame[1024])
{
    if(confirm) {
        omni_settings_view v={0};
        v.title="UNSAVED EQ";v.count=2;v.selected=confirm_choice;
        static const char *const opts[]={"SAVE","DISCARD"};
        v.labels[0]=opts[0];v.labels[1]=opts[1];
        v.status="LEAVE WITHOUT SAVING?";
        omni_settings_ui_render(frame,&v);return;
    }
    if(depth){graph(frame);return;}
    omni_settings_view v={0};
    static const char *const roots[]={"PRESET","EDIT CURVE","NEW FLAT"};
    v.title=control==12u?"WIRELESS EQ":control==13u?"MIC EQ":"BLUETOOTH EQ";
    v.count=3;v.selected=row;v.editing=editing;
    for(unsigned i=0;i<3u;++i)v.labels[i]=roots[i];
    unsigned n;bool known=io.read(control,&n);
    if(editing){n=value;known=true;}
    if(known)preset(v.values[0],n);else strcpy(v.values[0],"--");
    v.status=message?message:feedback && io.status?io.status():0;
    omni_settings_ui_render(frame,&v);
}
