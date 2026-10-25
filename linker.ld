ENTRY(_start)

SECTIONS {
    . = 0x00100000;

    .multiboot : {
        *(.multiboot)
    }

    .text ALIGN(4096) : {
        *(.text*)
    }

    .rodata ALIGN(4096) : {
        *(.rodata*)
    }

    .data ALIGN(4096) : {
        *(.data*)
    }

    .bss ALIGN(4096) : {
        *(COMMON)
        *(.bss*)
    }

    . = ALIGN(4096);
    _kernel_end = .;
}

ASSERT(_kernel_end <= 0x400000, "kernel image grew into 0x400000, where user programs load")
