#include <stdint.h>
static long syscall3(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}
static void quit(int code){syscall3(60,code,0,0);__builtin_unreachable();}
void _start(void){
 volatile uint8_t* p=(volatile uint8_t*)(uintptr_t)0x90000000;
 for(unsigned i=0;i<192;++i)p[i]=(i*29+17)&255;
 for(unsigned i=1;i<192;i+=3)p[i]=((i*29+17)&255)^0x5a;
 unsigned sum=0;
 for(unsigned i=0;i<192;++i){unsigned e=(i*29+17)&255;if(i%3==1)e^=0x5a;unsigned got=p[i];if(got!=e)quit(1);sum+=got;}
 volatile uint64_t* q=(volatile uint64_t*)(uintptr_t)0x90000ff8;
 q[0]=0x0123456789abcdefULL;q[1]=0xfedcba9876543210ULL;
 if(q[0]!=0x0123456789abcdefULL||q[1]!=0xfedcba9876543210ULL||sum!=24416)quit(2);
 const char msg[]="CPU MEMSIM PASS checksum=24416 boundary64=ok\n";
 syscall3(1,1,(long)msg,sizeof(msg)-1);quit(0);
}
