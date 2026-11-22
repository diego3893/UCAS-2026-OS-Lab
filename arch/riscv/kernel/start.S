/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 Regents of the University of California
 */

#include <asm.h>
#include <csr.h>

/* maybe you need another stack for booting? */
# .equ boot_stack_top_base, 0x50202000
.equ boot_stack_top_base, 0x50f00000

.section ".bootkernel_entry","ax"
ENTRY(_boot)
  /*
   * this is the entrypoint of the kernel. 
   * Don't clear BSS here
   */

  /* Mask all interrupts */
  li t0, SR_SIE
  csrc CSR_SSTATUS, t0
  csrw CSR_SIE, zero
  csrw CSR_SIP, zero

  # csrr a0, CSR_MHARTID 
  csrr a0, CSR_SSCRATCH

  li sp, boot_stack_top_base
  slli t0, a0, 12
  sub sp, sp, t0

  // this will never return
  call boot_kernel


loop:
  wfi
  j loop

END(_boot)
