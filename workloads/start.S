.section .text.start
.global _start
_start:
    li sp, 0x8000f000
    call kernel_main
    .insn r 0x0b, 0, 0, x0, x0, x0
1:  j 1b
