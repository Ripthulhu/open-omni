#include "home_ui.h"
#include "eq_response.h"
#include <stddef.h>
#include <string.h>

static const uint8_t letters[26][5]={
 {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
 {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
 {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
 {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
 {62,65,81,33,94},{127,9,25,41,70},{70,73,73,73,49},{1,1,127,1,1},
 {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
 {7,8,112,8,7},{97,81,73,69,67}};
static const uint8_t digits[10][5]={
 {62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},{33,65,69,75,49},
 {24,20,18,127,16},{39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},
 {54,73,73,73,54},{6,73,73,41,30}};
static unsigned limit(unsigned v) {return v>100u?100u:v;}
static void pixel(uint8_t *f,unsigned x,unsigned y,bool on)
{
    if(x>=128u || y>=64u) return;
    uint8_t *p=&f[(y/8u)*128u+x],mask=(uint8_t)(1u<<(y%8u));
    if(on) *p|=mask; else *p&=(uint8_t)~mask;
}
static void box(uint8_t *f,unsigned x,unsigned y,unsigned w,unsigned h,bool on)
{
    for(unsigned r=0;r<h && y+r<64u;r++)
        for(unsigned c=0;c<w && x+c<128u;c++) pixel(f,x+c,y+r,on);
}
static uint8_t glyph(char ch,unsigned col)
{
    if(ch>='A' && ch<='Z') return letters[(unsigned)(ch-'A')][col];
    if(ch>='0' && ch<='9') return digits[(unsigned)(ch-'0')][col];
    switch(ch) {
    case 'z': {static const uint8_t g[]={68,100,84,76,68};return g[col];}
    case 'd': {static const uint8_t g[]={56,68,68,72,127};return g[col];}
    case '-':return 8;
    case ':':return col==2u?36u:0u;
    case '.':return col==2u?64u:0u;
    case '/':return (uint8_t)(32u>>col);
    case '+':return col==2u?62u:8u;
    case '~': {static const uint8_t g[]={8,4,8,16,8};return g[col];}
    case '%': {static const uint8_t g[]={99,19,8,100,99};return g[col];}
    case '?': {static const uint8_t g[]={2,1,81,9,6};return g[col];}
    case '>': {static const uint8_t g[]={0,65,34,20,8};return g[col];}
    default:return 0;
    }
}
static void text(uint8_t *f,unsigned x,unsigned y,const char *s,unsigned max,
                 unsigned scale,bool on)
{
    if(!s) return;
    for(unsigned n=0;n<max && s[n] && x+5u*scale<=128u;n++,x+=6u*scale)
        for(unsigned c=0;c<5u;c++) for(unsigned r=0;r<7u;r++)
            if(glyph(s[n],c)&(1u<<r)) box(f,x+c*scale,y+r*scale,scale,scale,on);
}
static unsigned decimal(char s[4],unsigned value)
{
    if(value>999u) value=999u;
    unsigned n=0;
    if(value>=100u) s[n++]=(char)('0'+value/100u);
    if(value>=10u) s[n++]=(char)('0'+value/10u%10u);
    s[n++]=(char)('0'+value%10u);s[n]=0;return n;
}
static void number(uint8_t *f,unsigned x,unsigned y,unsigned value,bool on)
{
    char s[4];(void)decimal(s,value);text(f,x,y,s,3,1,on);
}
static void percent(uint8_t *f,unsigned x,unsigned y,unsigned value,bool on)
{
    char s[4];unsigned n=decimal(s,limit(value));text(f,x,y,s,3,1,on);
    text(f,x+n*6u,y,"%",1,1,on);
}
static void meter(uint8_t *f,unsigned x,unsigned y,unsigned w,unsigned h,
                  bool known,unsigned level)
{
    box(f,x,y,w,1,true);box(f,x,y+h-1,w,1,true);
    box(f,x,y,1,h,true);box(f,x+w-1,y,1,h,true);
    if(!known) {box(f,x+w/2-3,y+h/2,6,1,true);return;}
    unsigned fill=(limit(level)*(w-2u)+99u)/100u;
    for(unsigned c=0;c<fill;c++) if(c%4u!=3u) box(f,x+1u+c,y+2u,1,h-4u,true);
}
static void battery(uint8_t *f,const omni_home_view *v)
{
    /* Headphone silhouette distinguishes the worn pack from the slot battery. */
    box(f,95,0,5,1,true);box(f,93,1,2,5,true);box(f,100,1,2,5,true);
    box(f,93,3,3,4,true);box(f,99,3,3,4,true);
    if(v->link_known && !v->connected) {
        pixel(f,96,3,true);pixel(f,98,3,true);pixel(f,97,4,true);
        pixel(f,96,5,true);pixel(f,98,5,true);
    }
    if(v->battery_known && (!v->link_known || v->connected)) {
        char s[4];unsigned n=decimal(s,limit(v->battery_percent));
        text(f,128u-(n+1u)*6u,0,s,3,1,true);text(f,122,0,"%",1,1,true);
    } else text(f,104,0,"--%",3,1,true);
}
static void bluetooth(uint8_t *f,const omni_home_view *v)
{
    /* Show only observed active states. Absence is not a claim of BT off.
     * The passive cache cannot establish current state after MCU1 restart. */
    unsigned state=v->bluetooth_state;
    if(!v->bluetooth_known || state<0x32u || state>0x35u) return;
    static const uint8_t rune[5]={34,20,127,42,20};
    for(unsigned x=0;x<5u;++x) for(unsigned y=0;y<7u;++y)
        if(rune[x]&(1u<<y)) pixel(f,28+x,y,true);
    if(state==0x32u) {pixel(f,35,2,true);pixel(f,35,5,true);}
}
static void spare(uint8_t *f,const omni_home_view *v)
{
    unsigned state=v->spare_state;
    if(!state || state>4u) return;
    box(f,39,0,11,1,true);box(f,39,6,11,1,true);
    box(f,39,0,1,7,true);box(f,49,0,1,7,true);box(f,50,2,2,3,true);
    if(state==2u) {
        pixel(f,45,1,true);pixel(f,44,2,true);box(f,43,3,3,1,true);
        pixel(f,44,4,true);pixel(f,43,5,true);
    } else if(state==3u) box(f,41,2,7,3,true);
    else if(state==4u) {
        for(unsigned i=0;i<5u;i++) {pixel(f,42+i,1+i,true);pixel(f,46-i,1+i,true);}
    }
    /* Charging raises terminal voltage. It cannot supply a charge estimate. */
    if(state==2u) text(f,60,0,"CHG",3,1,true);
    else if(state==3u && v->spare_millivolts) percent(f,54,0,100u,true);
    else text(f,60,0,"--%",3,1,true);
}
static void db(uint8_t *f,unsigned x,unsigned y,int16_t value)
{
    int n=value/256;
    if(n<-99) n=-99;
    if(n>99) n=99;
    if(n<0) {text(f,x,y,"-",1,1,true);x+=6;n=-n;}
    else if(n>0) {text(f,x,y,"+",1,1,true);x+=6;}
    char s[4];unsigned len=decimal(s,(unsigned)n);text(f,x,y,s,3,1,true);
    text(f,x+len*6u,y,"dB",2,1,true);
}
void omni_home_ui_render(uint8_t f[1024],const omni_home_view *v)
{
    if(!f) return;
    memset(f,0,1024);
    if(!v) return;
    text(f,0,0,v->source,4,1,true);
    bluetooth(f,v);spare(f,v);battery(f,v);
    box(f,0,9,128,1,true);box(f,79,12,1,40,true);
    if(v->bias_mode) {
        const char *sources[]={"USB1",v->secondary_source==2u?"USB2":v->secondary_source==3u?"USB3":"USB2/3"};
        for(unsigned i=0;i<2u;++i) {
            unsigned y=13u+i*21u;
            unsigned q=v->bias_q14[i]>16384u?16384u:v->bias_q14[i];
            unsigned gain=(q*100u+8192u)/16384u;
            text(f,0,y,sources[i],6,1,true);
            if(v->bias_known) percent(f,44,y,gain,true);
            else text(f,44,y,"--%",3,1,true);
            meter(f,0,y+9u,74,10,v->bias_known,gain);
        }
    } else if(v->stereo_view) {
        if(v->sample_rate && v->sample_bits) {
            unsigned khz=v->sample_rate/1000u;char s[4];unsigned n=decimal(s,khz);
            text(f,0,13,s,3,1,true);text(f,n*6u,13,"K/",2,1,true);
            number(f,(n+2u)*6u,13,v->sample_bits,true);
        } else text(f,0,13,"--K/--",6,1,true);
        text(f,54,13,"OUT",3,1,true);
        text(f,0,27,"L",1,1,true);text(f,0,42,"R",1,1,true);
        meter(f,10,26,64,10,v->stereo_known,v->left);
        meter(f,10,41,64,10,v->stereo_known,v->right);
    } else for(unsigned i=0;i<4;i++) {
        unsigned y=13u+i*10u;text(f,0,y,v->input[i].label,6,1,true);
        meter(f,39,y,35,7,v->input[i].known,v->input[i].level);
    }
    char n[4];unsigned len=decimal(n,limit(v->percent));
    unsigned x=126u-(len*12u+5u);
    text(f,x,13,n,3,2,true);text(f,122,20,"%",1,1,true);
    db(f,83,32,v->db_x256);
    meter(f,83,43,45,8,true,v->muted?0u:v->percent);
    box(f,0,54,128,1,true);
    /* The numerical gain is amplitude Q14, never a percentage of detents.
     * Center keeps both sources at unity rather than halving either source. */
    bool centered=v->bias_mode && v->bias_known && v->bias==12u;
    if(v->bias_mode) text(f,0,57,centered?"BOTH FULL":"MIX",9,1,true);
    if(!centered && !v->stereo_view && v->sample_rate && v->sample_bits && v->sample_bits<100u) {
        char rate[4];unsigned n=decimal(rate,v->sample_rate/1000u);
        text(f,44,57,rate,3,1,true);text(f,44u+n*6u,57,"K/",2,1,true);
        number(f,44u+(n+2u)*6u,57,v->sample_bits,true);
    }
    if(v->gain_fault) {box(f,92,56,36,8,true);text(f,93,57,"FAULT",5,1,false);}
    else if(v->muted) {box(f,96,56,32,8,true);text(f,97,57,"MUTE",4,1,false);}
    else if(v->gain_known && !v->gain_ready) text(f,98,57,"SYNC",4,1,true);
}
static const char *const names[]={"USB1","USB2","USB3","LINE IN"};
void omni_menu_ui_render(uint8_t f[1024],const omni_menu_view *v)
{
    if(!f) return;
    memset(f,0,1024);
    if(!v) return;
    unsigned selected=v->selected<4u?v->selected:0u,count=0,ordinal=0;
    for(unsigned i=0;i<4u;++i) if(v->input[i].available) {
        if(i==selected) ordinal=count;
        ++count;
    }
    if(!v->input[selected].available && count) {
        for(selected=0;selected<4u && !v->input[selected].available;++selected) {}
        ordinal=0;
    }
    text(f,0,0,v->detail?names[selected]:"LINEOUT MIX",11,1,true);
    if(v->detail) text(f,92,0,"L.OUT",5,1,true);
    else {number(f,110,0,count?ordinal+1u:0u,true);text(f,116,0,"/",1,1,true);number(f,122,0,count,true);}
    box(f,0,9,128,1,true);
    if(!v->detail) {
        unsigned row=0;
        for(unsigned i=0;i<4;i++) {
            const omni_menu_input *s=&v->input[i];
            if(!s->available) continue;
            /* Current backend has two routes. Keep a future four-route
             * snapshot bounded without drawing through the footer. */
            bool compact=count>2u;
            unsigned y=13u+row++*(compact?12u:27u);bool on=i!=selected;
            if(!on) box(f,0,y-1u,128,compact?9u:17u,true);
            text(f,2,y,names[i],7,1,on);percent(f,98,y,s->level,on);
            if(compact) text(f,58,y,s->muted?"MUTE":s->linked?"LINK":"FREE",4,1,on);
            else {
                text(f,2,y+9u,s->muted?"MUTED":s->linked?"FOLLOWS VOLUME":"INDEPENDENT",14,1,on);
                meter(f,2,y+18u,124,6,true,s->muted?0u:s->level);
            }
        }
        if(!count) text(f,0,24,"NO ROUTES READY",15,1,true);
        return;
    }
    const omni_menu_input *s=&v->input[selected];
    if(!s->available) {
        text(f,0,16,"NOT READY",9,1,true);
        text(f,0,30,"ROUTING PENDING",15,1,true);
        text(f,0,44,"NO LEVEL CONTROL",16,1,true);
    } else {
        const char *const fields[]={"LEVEL","FOLLOW VOL","MUTE"};
        unsigned field=v->field<3u?v->field:0u;
        for(unsigned i=0;i<3;i++) {
            unsigned y=13+i*13u;bool on=i!=field;
            if(!on) box(f,0,y-1,128,10,true);
            text(f,2,y,fields[i],11,1,on);
            if(i==0) percent(f,91,y,s->level,on);
            else text(f,103,y,i==1?(s->linked?"YES":"NO"):(s->muted?"ON":"OFF"),3,1,on);
        }
    }
    /* The lower strip contains the retained source level/mute, not help text.
     * A distinct marker identifies an actively edited level. */
    if(s->available) {
        if(v->editing && v->field==0u) text(f,78,13,">",1,1,false);
        meter(f,2,53,124,11,true,s->muted?0u:s->level);
    }
}

void omni_settings_ui_render(uint8_t f[1024],const omni_settings_view *v)
{
    if(!f) return;
    memset(f,0,1024);if(!v) return;
    text(f,0,0,v->title,15,1,true);
    char position[9],total[4];
    unsigned length=decimal(position,v->selected+1u);position[length++]='/';position[length]=0;
    (void)decimal(total,v->count);strcat(position,total);
    text(f,128u-(unsigned)strlen(position)*6u,0,position,8,1,true);
    box(f,0,9,128,1,true);
    for(unsigned i=0;i<4u;++i) {
        if(!v->labels[i]) continue;
        unsigned y=13u+i*10u;bool selected=i==v->selected%4u;
        if(selected) box(f,0,y-1u,128,9,true);
        text(f,1,y,v->labels[i],13,1,!selected);
        unsigned n=(unsigned)strlen(v->values[i]);if(n>7u)n=7u;
        unsigned x=128u-n*6u;
        if(selected && v->editing) box(f,x-1u,y-1u,128u-x+1u,9,false);
        text(f,x,y,v->values[i],7,1,selected?v->editing:true);
    }
    box(f,0,54,128,1,true);
    if(v->status) text(f,0,57,v->status,21,1,true);
}

/* Control-point plot: interpolating band gains is not a DSP response model. */
static unsigned eq_x(unsigned frequency)
{
    if(frequency<=20u)return 22u;
    if(frequency>=20000u)return 125u;
    unsigned lo=0,hi=103;
    while(hi-lo>1u){unsigned mid=(hi+lo)/2u;if(omni_eq_response_frequency(mid)<frequency)lo=mid;else hi=mid;}
    return 22u+(frequency-omni_eq_response_frequency(lo)<omni_eq_response_frequency(hi)-frequency?lo:hi);
}
static unsigned eq_y(unsigned gain)
{if(gain>240u)gain=240u;return 39u-(gain*28u+120u)/240u;}
static void eq_line(uint8_t *f,unsigned ax,unsigned ay,unsigned bx,unsigned by)
{
    int x=(int)ax,y=(int)ay,dx=(int)bx-x,dy=(int)by-y;
    int sx=dx<0?-1:1,sy=dy<0?-1:1;
    if(dx<0)dx=-dx;
    if(dy>0)dy=-dy;
    int error=dx+dy;
    for(unsigned guard=0;guard<128u;++guard) {
        pixel(f,(unsigned)x,(unsigned)y,true);
        if(x==(int)bx && y==(int)by)break;
        int twice=2*error;
        if(twice>=dy){error+=dy;x+=sx;}
        if(twice<=dx){error+=dx;y+=sy;}
    }
}
static void eq_value(uint8_t *f,unsigned x,unsigned y,const char *value,bool focus,bool editing)
{
    unsigned n=(unsigned)strlen(value);if(n>7u)n=7u;
    if(focus && editing) {
        box(f,x-1u,y-1u,n*6u+1u,1u,true);
        box(f,x-1u,y+7u,n*6u+1u,1u,true);
        box(f,x-1u,y-1u,1u,9u,true);
        box(f,x+n*6u-1u,y-1u,1u,9u,true);
    }
    text(f,x,y,value,7u,1u,true);
    if(focus && !editing && n) box(f,x,y+7u,n*6u-1u,1u,true);
}
static unsigned eq_model(const omni_eq_view *v,int16_t curve[104])
{
    static unsigned keys[10][4];
    static bool valid[10];
    static int16_t bands[10][104];
    unsigned budget=1u;bool ready=true;
    for(unsigned i=0;i<10u;++i) {
        unsigned key[4]={v->frequency[i],v->gain[i],v->q[i],v->type[i]};
        if(!v->known[i] || key[0]<20u || key[0]>20001u || key[1]>247u ||
           (key[0]!=20001u && (key[2]<200u || key[2]>10000u || key[3]<1u || key[3]>6u)))return 0;
        if(valid[i] && !memcmp(keys[i],key,sizeof(key)))continue;
        bool flat=key[0]==20001u || key[3]>5u || ((key[3]==1u || key[3]>=4u) && key[1]==120u);
        if(!flat && !budget){ready=false;continue;}
        if(!flat)--budget;
        for(unsigned x=0;x<104u;++x) bands[i][x]=flat?0:(int16_t)omni_eq_response_band(
            key[0],key[1],key[2],key[3],omni_eq_response_frequency(x));
        memcpy(keys[i],key,sizeof(key));valid[i]=true;
    }
    if(!ready)return 1;
    for(unsigned x=0;x<104u;++x) {
        int sum=0;for(unsigned i=0;i<10u;++i)sum+=bands[i][x];
        curve[x]=(int16_t)(sum<-1200?-120:sum>1200?120:sum/10);
    }
    return 2;
}
void omni_eq_ui_render(uint8_t f[1024],const omni_eq_view *v)
{
    if(!f)return;
    memset(f,0,1024);if(!v)return;
    text(f,0,0,v->title,14u,1u,true);
    if(v->apply) text(f,98,0,"APPLY",5u,1u,true);
    else {
        text(f,104,0,"B",1u,1u,true);
        number(f,110,0,v->selected<10u?v->selected+1u:0u,true);
    }
    box(f,0,9,128,1,true);
    text(f,0,11,"+12",3,1,true);text(f,12,22,"0",1,1,true);text(f,0,33,"-12",3,1,true);
    for(unsigned x=22;x<=125;x+=3)pixel(f,x,25,true);
    for(unsigned i=0;i<3u;++i) {
        unsigned x=v->parametric?eq_x(i==0u?100u:i==1u?1000u:10000u):22u+i*34u;
        for(unsigned y=13;y<=39;y+=7)pixel(f,x,y,true);
    }
    unsigned xs[10],ys[10],order[10],count=0;
    for(unsigned i=0;i<10u;++i) {
        if(!v->known[i] || v->gain[i]>247u || (v->parametric && (v->frequency[i]<20u || v->frequency[i]>20000u)))continue;
        xs[i]=v->parametric?eq_x(v->frequency[i]):22u+(i*103u+4u)/9u;
        ys[i]=eq_y(v->gain[i]);unsigned at=count;
        while(at && xs[order[at-1u]]>xs[i]){order[at]=order[at-1u];--at;}
        order[at]=i;++count;
    }
    if(v->parametric) {
        int16_t curve[104];
        unsigned model=eq_model(v,curve);
        if(model==2u) {
            for(unsigned x=1;x<104u;++x)eq_line(f,21u+x,eq_y((unsigned)(curve[x-1u]+120)),
                22u+x,eq_y((unsigned)(curve[x]+120)));
        } else text(f,43,17,model?"CALC...":"NO MODEL",9u,1u,true);
    } else for(unsigned i=1;i<count;++i)eq_line(f,xs[order[i-1u]],ys[order[i-1u]],xs[order[i]],ys[order[i]]);
    for(unsigned pass=0;pass<2u;++pass) for(unsigned j=0;j<count;++j) {
        unsigned i=order[j],x=xs[i],y=ys[i];
        bool selected=!v->apply && i==v->selected;
        if(selected!=(pass==1u))continue;
        if(selected) {
            for(unsigned yy=11;yy<=39;yy+=3)pixel(f,x,yy,true);
            for(int oy=-1;oy<=1;++oy)for(int ox=-1;ox<=1;++ox) {
                int xx=(int)x+ox,yy=(int)y+oy;
                if(xx<22 || xx>127 || yy<11 || yy>39)continue;
                bool edge=ox==-1 || ox==1 || oy==-1 || oy==1;
                pixel(f,(unsigned)xx,(unsigned)yy,edge);
            }
        } else {
            /* Hollow diamonds keep overlapping control points distinct. */
            pixel(f,x,y,false);
            if(x>22u)pixel(f,x-1u,y,true);
            if(x<125u)pixel(f,x+1u,y,true);
            if(y>11u)pixel(f,x,y-1u,true);
            if(y<39u)pixel(f,x,y+1u,true);
        }
    }
    box(f,0,42,128,1,true);
    if(v->apply) {
        box(f,0,44,128,1,true);box(f,0,52,128,1,true);
        pixel(f,0,48,true);pixel(f,127,48,true);text(f,31,45,"APPLY CURVE",11,1,true);
        if(v->status)text(f,0,56,v->status,21,1,true);
    } else if(v->parametric) {
        eq_value(f,2,45,v->values[1],v->field==1u,v->editing);
        eq_value(f,86,45,v->values[0],v->field==0u,v->editing);
        text(f,2,56,"Q",1,1,true);eq_value(f,14,56,v->values[2],v->field==2u,v->editing);
        eq_value(f,86,56,v->values[3],v->field==3u,v->editing);
    } else {
        text(f,2,45,"GAIN",4,1,true);eq_value(f,86,45,v->values[0],v->field==0u,v->editing);
        text(f,22,56,"B1",2,1,true);text(f,66,56,"B5",2,1,true);text(f,110,56,"B10",3,1,true);
    }
    if(v->status && !v->apply && strcmp(v->status,"DRAFT")) {
        box(f,0,55,128,9,false);text(f,0,56,v->status,21,1,true);
    }
}
