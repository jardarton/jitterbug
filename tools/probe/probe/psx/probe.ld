/*
 * The probe's memory map (docs/probe.md, "Memory map"). Main RAM is 2 MB at
 * 80000000h (KSEG0); the kernel keeps the first 64 KB. The probe loads at
 * 80010000h, its buffers follow in BSS, and the stack grows down from
 * 801FFFF0h with at least 64 KB to itself.
 */
OUTPUT_FORMAT("elf32-tradlittlemips")
OUTPUT_ARCH(mips)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80010000, LENGTH = 0x801F0000 - 0x80010000
}

SECTIONS
{
    .text : {
        __load_start = .;
        KEEP(*(.text.start))
        *(.text .text.*)
    } > RAM
    .rodata : { *(.rodata .rodata.*) } > RAM
    .data : { *(.data .data.* .sdata .sdata.*) } > RAM
    . = ALIGN(4);
    __load_end = .;
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM
    /DISCARD/ : {
        *(.MIPS.abiflags) *(.reginfo) *(.comment) *(.pdr) *(.gnu.attributes)
        *(.note .note.*) *(.eh_frame .eh_frame_hdr) *(.mdebug.*)
    }
}

ASSERT(__bss_end <= 0x801F0000, "the probe and its buffers must leave 64 KB below 801FFFF0h for the stack")
