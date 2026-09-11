#include "eq_response.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static double reference(unsigned hz,unsigned gain,unsigned q,unsigned type,unsigned probe)
{
    double pi=3.14159265358979323846,w0=2*pi*hz/48000.0,w=2*pi*probe/48000.0;
    double a=pow(10,((double)gain-120)/400),c=cos(w0),alpha=sin(w0)/(2*(double)q/1000);
    double b0,b1,b2,a0=1+alpha,a1=-2*c,a2=1-alpha;
    if(type==1){b0=1+alpha*a;b1=-2*c;b2=1-alpha*a;a0=1+alpha/a;a2=1-alpha/a;}
    else if(type==2){b0=(1-c)/2;b1=1-c;b2=b0;}
    else if(type==3){b0=(1+c)/2;b1=-(1+c);b2=b0;}
    else {
        double t=2*sqrt(a)*alpha;
        if(type==4){b0=a*((a+1)-(a-1)*c+t);b1=2*a*((a-1)-(a+1)*c);b2=a*((a+1)-(a-1)*c-t);a0=(a+1)+(a-1)*c+t;a1=-2*((a-1)+(a+1)*c);a2=(a+1)+(a-1)*c-t;}
        else{b0=a*((a+1)+(a-1)*c+t);b1=-2*a*((a-1)+(a+1)*c);b2=a*((a+1)+(a-1)*c-t);a0=(a+1)-(a-1)*c+t;a1=2*((a-1)-(a+1)*c);a2=(a+1)-(a-1)*c-t;}
    }
    double nr=b0+b1*cos(w)+b2*cos(2*w),ni=b1*sin(w)+b2*sin(2*w);
    double dr=a0+a1*cos(w)+a2*cos(2*w),di=a1*sin(w)+a2*sin(2*w);
    return 10*log10((nr*nr+ni*ni)/(dr*dr+di*di));
}
int main(void)
{
    static const unsigned frequencies[]={20,32,100,1000,8000,18000,20000},qs[]={200,707,1414,3000,10000};
    double worst=0;unsigned cases=0;
    for(unsigned t=1;t<=5;++t)for(unsigned f=0;f<7;++f)for(unsigned g=0;g<=240;g+=60)for(unsigned q=0;q<5;++q)for(unsigned x=0;x<104;++x) {
        unsigned hz=omni_eq_response_frequency(x);
        double expected=reference(frequencies[f],g,qs[q],t,hz);
        double actual=(double)omni_eq_response_band(frequencies[f],g,qs[q],t,hz)/100;
        if(expected>=-24 && expected<=24) {double error=fabs(actual-expected);if(error>worst)worst=error;assert(error<0.15);}
        ++cases;
    }
    assert(omni_eq_response_band(1000,180,1000,1,1000)>=599);
    assert(omni_eq_response_band(1000,180,5000,1,1500)<omni_eq_response_band(1000,180,500,1,1500));
    int cut=omni_eq_response_band(100,120,707,2,1600)-omni_eq_response_band(100,120,707,2,800);
    assert(cut < -1190 && cut > -1240); /* Second-order low-pass asymptote, 12 dB/oct. */
    assert(omni_eq_response_band(20001,180,1000,1,1000)==0);
    assert(omni_eq_response_band(1000,180,1000,6,1000)==0);
    printf("%u independent biquad comparisons; worst visible-range error %.4f dB\n",cases,worst);return 0;
}
