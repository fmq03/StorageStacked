#include <cstdint>

int main() {
    auto* words=reinterpret_cast<volatile uint32_t*>(0x90000ff0u);
    auto* bytes=reinterpret_cast<volatile uint8_t*>(words);
    auto* sentinel=reinterpret_cast<volatile uint32_t*>(0x90000fecu);
    auto* output=reinterpret_cast<volatile uint32_t*>(0x90010010u);
    auto* mailbox=reinterpret_cast<volatile uint32_t*>(0xc0000000u);
    *sentinel=0x76543210u;
    for(unsigned i=0;i<8;++i) words[i]=0x10203040u+i*0x01010101u;
    for(unsigned i=1;i<32;i+=3) bytes[i]=uint8_t(0xa0u+i);
    bool passed=*sentinel==0x76543210u;
    uint32_t checksum=0;
    for(unsigned i=0;i<8;++i) {
        uint32_t expected=0x10203040u+i*0x01010101u;
        for(unsigned j=0;j<4;++j) {
            const unsigned offset=4*i+j;
            if(offset%3==1) expected=(expected&~(0xffu<<(8*j)))|((0xa0u+offset)<<(8*j));
        }
        const uint32_t observed=words[i];
        passed=passed && observed==expected;
        checksum^=observed;
    }
    *output=checksum;
    passed=passed && *output==checksum;
    *mailbox=passed?0x600d0000u:0xbad00002u;
    asm volatile("wfi");
    return 0;
}
