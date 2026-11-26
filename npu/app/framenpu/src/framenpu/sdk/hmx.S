.text
.global fnpu_hmx_clear
fnpu_hmx_clear:
 .word 0xa6e0c013
 { jumpr r31 }

.global fnpu_hmx_load
fnpu_hmx_load:
 { r2 = r1 }
 { r1 = #2047 }
 .word 0x920041e2
 .word 0x9202e1ef
 { jumpr r31 }

.global fnpu_hmx_store
fnpu_hmx_store:
 { r3 = r0 }
 { r0 = r1 }
 .word 0x9200c3fe
 { r0 = #2 }
 .word 0xa6e0da10
 { r0 = r3 }
 { r1 = #0 }
 .word 0xa6e0c118
 { barrier }
 { jumpr r31 }
