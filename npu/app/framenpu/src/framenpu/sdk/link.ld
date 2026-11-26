ENTRY(custom_job)
SECTIONS {
 . = 0xf0a92000;
 .payload : { *(.text.entry) *(.text*) *(.rodata*) *(.data*) *(.sdata*) }
 .bss : { *(.bss*) *(.sbss*) *(COMMON) }
 ASSERT(SIZEOF(.payload) <= 0x1000, "DSP payload exceeds 4 KiB code reservation")
 ASSERT(SIZEOF(.bss) == 0, "DSP kernels must not depend on BSS initialization")
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
