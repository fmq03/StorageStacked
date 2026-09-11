/* Three independent rounds exercise more than 1023 transactions. */
#define main one_round
#include "memory_check.c"
#undef main
int main(void) {
    for (unsigned round = 0; round < 3; ++round) {
        int result = one_round();
        if (result) return result;
    }
    puts("CPU AXI ID-WRAP PASS rounds=3");
    return 0;
}
