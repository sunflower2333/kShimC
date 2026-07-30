#include <ld/arm64_ld.h>

ENTRY(_start)
SECTIONS
{
    /*. = 0x40200000;*/
    . = LD_START_ADDRESS;
    __image_start_address__ = .;
    . = ALIGN(8);
    .text.boot : {*(.text.boot)}
    .text : {*(.text .text.*)}

    . = ALIGN(8);
    .rodata : {*(.rodata .rodata.* .ro_data .ro_data.*)}
    .data : { 
        *(.data .data.*);
    }

    .bss (NOLOAD) : {
        __bss_start__ = .;
        *(.bss*);
        *(COMMON);
        . = ALIGN(0x40);
        __bss_end__ = .;
    }

    . = ALIGN(8);

    .gots ALIGN(4) : {
        __gots_start__ = .;
        *(.got);
        *(.got.*);
        __gots_end__ = .;
        . = ALIGN(8);
    }

    .rels ALIGN(8) : {
        __reldyn_start__ = .;
        *(.rel);
        *(.rel.*);
        __reldyn_end__ = .;
        . = ALIGN(8);
    }

    .relas ALIGN(8) : {
        __reladyn_start__ = .;
        *(.rela);
        *(.rela.*);
        __reladyn_end__ = .;
        . = ALIGN(8);
    }

    .heap (NOLOAD) : {
        . = ALIGN(16);
        __heap_start = .;
        . = . + 0x10000;
        __heap_end = .;
    }

    .stack (NOLOAD) : {
        . = ALIGN(0x40);
        __stack_base__ = .;
        /* primary stack */
        . = . + LD_STACK_SIZE;
        . = ALIGN(0x40);
        __stack_top__ = .;
    }

    . = ALIGN(8);
    /DISCARD/ : { *(.note.GNU-stack) }
    __image_end_address__ = .;
}