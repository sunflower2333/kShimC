/* SPDX-License-Identifier: MIT */
ENTRY(_start)
SECTIONS
{
    . = 0x40080000;
    .text : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;
    . = ALIGN(16);
    . = . + 0x20000;
    __stack_top = .;
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
