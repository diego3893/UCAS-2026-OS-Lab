#include <asm/biosdef.h>
#include <csr.h>

// os size location (os_size could be sectors [p1-task3] or bytes [p1-task4])
.equ os_size_loc, 0x502001fc

// kernel address (move kernel to here, and jmp here to start kernel)
.equ kernel, 0x50202000

// BIOS function entry (jump here to use BIOS APIs)
.equ bios_func_entry, 0x50150000

// App Info
.equ app_info_offset_loc, 0x502001f4

// APP数量
.equ task_num_loc, 0x502001fa

.text
.global main

main:
	// fence on all memory and I/O
	fence
    // a0 is mhartid
	csrw CSR_SSCRATCH, a0
	bnez a0, secondary

	// TODO: [p1-task1] call BIOS to print string "It's bootblock!"
	li a7, BIOS_PUTSTR 
	la a0, msg
	jal bios_func_entry

	// TODO: [p1-task2] call BIOS to read kernel in SD card
	# li a0, kernel
	# li t0, os_size_loc
	# lhu a1, 0(t0)
	# li a2, 1
	# li a7, BIOS_SDREAD
	# jal bios_func_entry
	// 改为分批次读取，每次最多4个sector
	li s0, kernel
    li t0, os_size_loc
    lhu s1, 0(t0)
    li s2, 1
	// s1: 剩余扇区数，s2: 下一批的起始sector，s3: 本批次sector数量，s0: 下一批数据存放地址

read_kernel_loop:
    beqz s1, read_kernel_done

    li s3, 64
    bgeu s1, s3, read_kernel_chunk_ready
    mv s3, s1

read_kernel_chunk_ready:
    mv a0, s0
    mv a1, s3
    mv a2, s2
    li a7, BIOS_SDREAD
    jal bios_func_entry

    slli t0, s3, 9 // 扇区转字节数
    add s0, s0, t0
    sub s1, s1, s3
    add s2, s2, s3
    j read_kernel_loop

read_kernel_done:

	// TODO: [p1-task4] load task-related arguments and pass them to kernel
	li t0, app_info_offset_loc
	lwu a0, 0(t0)
	li t0, task_num_loc
	lhu a1, 0(t0)

	// TODO: [p1-task2] jump to kernel to start UCAS-OS
	fence.i
	j kernel

	/************************************************************/
secondary:
	/* TODO [P3-task3]: 
	 * 1. Mask all interrupts
	 * 2. let stvec pointer to kernel_main
	 * 3. enable software interrupt for ipi
	 */
	li t0, SR_SIE
    csrc CSR_SSTATUS, t0
    csrw CSR_SIE, zero // 关中断

    la t0, secondary_enter
    csrw CSR_STVEC, t0 // 写从核中断入口

    li t0, SIE_SSIE
    csrw CSR_SIE, t0 // 只开软件中断

    li t0, SR_SIE
    csrs CSR_SSTATUS, t0 // 开全局中断

wait_for_wakeup:
	wfi
	j wait_for_wakeup
	
	.balign 4 // 4字节对齐
secondary_enter:
    li t0, SR_SIE
    csrc CSR_SSTATUS, t0
    csrw CSR_SIE, zero
    csrw CSR_SIP, zero // 关中断，进内核

    fence
    fence.i
    j kernel

	/************************************************************/
// while(1) --> stop here
stop:
	j stop
	nop

.data

msg: .string "It's zhangchenxuan's bootloader...\n\r"

