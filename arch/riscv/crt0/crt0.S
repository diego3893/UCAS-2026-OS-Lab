#include <asm.h>

.section ".entry_function","ax"
ENTRY(_start)

    /* TODO: [p1-task3] setup C runtime environment for the user program */
    addi sp, sp, -16
    sd ra, 8(sp)

    la t0, __bss_start
    la t1, __BSS_END__
clear_bss:
    bgeu t0, t1, clear_bss_done
    sd zero, 0(t0)
    addi t0, t0, 8
    j clear_bss
clear_bss_done:

    /* TODO: [p1-task3] enter main function */
    call main


    /* TODO: [p1-task3] finish task and return to the kernel, replace this in p3-task2! */
    ld ra, 8(sp)
    addi sp, sp, 16
    ret

    /************************************************************/
	/* Do not touch this comment. Reserved for future projects. */
	/************************************************************/
// while(1) loop, unreachable here
loop:
    wfi
    j loop

END(_start)
