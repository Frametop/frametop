ENTRY(rg_batch)
SECTIONS {
 . = 0xf1ff0000;
 .payload : { *(.text.rg_entry) *(.text*) *(.rodata*) }
 .data : { *(.data*) *(.sdata*) }
 .bss : { *(.bss*) *(.sbss*) *(COMMON) }
 ASSERT(SIZEOF(.payload) <= rg_code_bytes, "linked resident image exceeds its DDR reservation")
 ASSERT(SIZEOF(.data) == 0, "linked resident image must be read-only")
 ASSERT(SIZEOF(.bss) == 0, "linked resident image must not depend on BSS")
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
