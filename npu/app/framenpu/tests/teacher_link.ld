ENTRY(tl_entry)
SECTIONS {
 . = 0xf1ff0000;
 .payload : { *(.text.rg_entry) *(.text*) *(.rodata*) }
 .data : { *(.data*) *(.sdata*) }
 .bss : { *(.bss*) *(.sbss*) *(COMMON) }
 ASSERT(SIZEOF(.payload) <= 0x3f80, "teacher image overlaps its seal vector")
 ASSERT(SIZEOF(.data) == 0, "teacher image must be read-only")
 ASSERT(SIZEOF(.bss) == 0, "teacher image must not depend on BSS")
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
