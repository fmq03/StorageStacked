int main(int argc, char** argv) {
    int ret_val;
    asm volatile("li t0, 0x800; csrs mie, t0;");
    asm volatile("nop; wfi");
    ret_val = *(&argc);
    return ret_val;
}
