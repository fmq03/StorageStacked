#include <stdint.h>
#define REG ((volatile uint32_t*)0xf000)
#define META ((volatile uint32_t*)0xf400)
#define IN ((volatile uint32_t*)0xf500)
#define OUT ((volatile uint32_t*)0xf600)
#define COMPLETION ((volatile uint32_t*)0xf300)
static float f16(uint16_t x) {union {float f;uint32_t u;}v;v.u=(uint32_t)x<<16;return v.f;}
static uint16_t b16(float x) {union{float f;uint32_t u;}v;v.f=x;return (v.u+0x7fff+((v.u>>16)&1))>>16;}
static int wait_job(void) {
    uint32_t status=COMPLETION[0];
    return (status&6)==2;
}
void kernel_main(void) {
    uint32_t n=META[1],lg=META[2],k=META[3],nq=META[4],baseline=META[5];
    volatile uint16_t* keys=(volatile uint16_t*)(uintptr_t)META[6];
    volatile uint16_t* queries=(volatile uint16_t*)(uintptr_t)META[7];
    uint16_t means[32][128];
    if(baseline) {
        for(unsigned c=0;c<n;c++) {
            float pool[128];
            for(unsigned d=0;d<128;d++)pool[d]=0;
            for(unsigned t=0;t<(1u<<lg);t++)
                for(unsigned d=0;d<128;d++)pool[d]=pool[d]+f16(keys[(c*(1u<<lg)+t)*128+d]);
            for(unsigned d=0;d<128;d++)means[c][d]=b16(pool[d]/(float)(1u<<lg));
        }
    } else {
        REG[1]=(uint32_t)(uintptr_t)keys;REG[2]=(uint32_t)(uintptr_t)queries;
        REG[3]=n;REG[4]=lg;REG[5]=k;REG[6]=n-1;REG[7]=1;
        REG[0]=1;
        if(!wait_job()) {META[8]=0xbad00001;return;}
        META[12]=REG[9];META[13]=REG[13];
    }
    for(unsigned q=0;q<nq;q++) {
        unsigned current=IN[q*2];uint32_t mask=1u<<current,count=1;
        if(baseline) {
            float scores[32];
            for(unsigned c=0;c<current;c++) {
                float sum=0;
                for(unsigned d=0;d<128;d++)sum=sum+f16(means[c][d])*f16(queries[q*128+d]);
                scores[c]=sum;
            }
            for(unsigned rank=1;rank<k&&rank<=current;rank++) {
                unsigned best=32;
                for(unsigned c=0;c<current;c++)if(!(mask&(1u<<c)))
                    if(best==32||scores[c]>scores[best])best=c;
                mask|=1u<<best;++count;
            }
        } else {
            REG[2]=(uint32_t)(uintptr_t)(queries+q*128);REG[6]=current;REG[0]=2;
            if(!wait_job()){META[8]=0xbad00002;return;}
            mask=COMPLETION[1];count=COMPLETION[2];
            OUT[q*4+2]=COMPLETION[3];OUT[q*4+3]=COMPLETION[4];
        }
        OUT[q*4]=mask;OUT[q*4+1]=count;
        if(mask!=IN[q*2+1]){META[8]=0xbad00003;return;}
        META[9]=q+1;
    }
    META[8]=0x600dd00d;
}
