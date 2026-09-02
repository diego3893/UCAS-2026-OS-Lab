.equ RESULT_ADDR, 0x51000000

.section .text
.global main
main:
    li      t0, 1
    li      t1, 0
    li      t2, 51

sum_loop:
    add     t1, t1, t0
    addi    t0, t0, 1
    blt     t0, t2, sum_loop

    la      t3, RESULT_ADDR
    sw      t1, 0(t3)

halt:
    j       halt