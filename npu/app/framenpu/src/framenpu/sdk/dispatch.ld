ENTRY(dispatch_start)
stock_feature = 0x87000600;
stock_complete = 0x870006a0;
stock_metadata = 0x8700b060;
stock_clean = 0x870030d0;
stock_flops = 0x87003850;
stock_memory = 0x870039d0;
SECTIONS {
 .text 0x870005a0 : { *(.text) }
 /DISCARD/ : { *(.comment) *(.note*) *(.hexagon.attributes) }
}
ASSERT(SIZEOF(.text) == 96, "dispatcher must fill exactly the ABI 9 case window")
