.section .text
.global main
.global is_prime
main:
    li      s0, 1
    li      s1, 201
    la      s2, prime_results

check_next:
    mv      a0, s0
    call    is_prime
    sb      a0, 0(s2)
    addi    s0, s0, 1
    addi    s2, s2, 1
    blt     s0, s1, check_next

halt:
    j       halt

# 输入：a0，输出：a0, 1表示为质数
is_prime:
    li      t0, 2
    blt     a0, t0, not_prime

    li      t1, 2
test_divisor:
    mul     t2, t1, t1
    blt     a0, t2, prime
    remu    t2, a0, t1 # t2=a0%t1
    beqz    t2, not_prime
    addi    t1, t1, 1
    j       test_divisor

prime:
    li      a0, 1
    ret

not_prime:
    li      a0, 0
    ret

# 预留200bits空间
.section .bss
.align 3
.global prime_results
prime_results:
    .space  200
