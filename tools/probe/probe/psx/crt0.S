# The probe's start-up on the console (docs/probe.md, "Memory map"): a stack at
# the top of RAM, interrupts off (the probe polls; no handler of the BIOS's
# or a loader's may take I_STAT bits from it), BSS zeroed, then main.
# MIPS I, little-endian.

    .set noreorder
    .set noat

    .section .text.start, "ax"
    .globl _start
    .ent _start
_start:
    lui $sp, 0x801f
    ori $sp, $sp, 0xfff0
    move $fp, $sp

    # COP0 SR (r12): IEc (bit 0) and IEp (bit 2) clear.
    mfc0 $t0, $12
    li $t1, ~5
    and $t0, $t0, $t1
    mtc0 $t0, $12
    nop
    nop

    la $t0, __bss_start
    la $t1, __bss_end
1:
    beq $t0, $t1, 2f
    nop
    sw $zero, 0($t0)
    b 1b
    addiu $t0, $t0, 4
2:
    jal main
    nop
3:
    b 3b
    nop
    .end _start

# COP0 PRId (r15), for hal_identify.
    .text
    .globl psx_prid
    .ent psx_prid
psx_prid:
    mfc0 $v0, $15
    nop
    jr $ra
    nop
    .end psx_prid
