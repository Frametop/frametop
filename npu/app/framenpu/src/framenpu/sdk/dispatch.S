/* ABI 9 CPU-request cases, linked at the verified 0x870005a0 window.
 * An odd callback is a FrameNPU direct request; clear the tag before calling.
 * Even callbacks retain the stock FLOPS worker path. No reserved storage is
 * referenced unless the owner publishes a tagged callback for that operation.
 */
.text
.global dispatch_start
dispatch_start:
 .set .Lfeature, dispatch_start + 0x60
 .set .Lcomplete, dispatch_start + 0x100
 { r0 = memw(r22+#4) }
 { p0 = cmp.eq(r0,#2); if (p0.new) jump:nt .Lmemory }
 { p0 = cmp.eq(r0,#4); if (p0.new) jump:nt .Lcustom }
 { p0 = cmp.eq(r0,#8); if (p0.new) jump:nt .Lfeature }
 { p0 = cmp.eq(r0,#32) }
 { if (!p0) jump .Lcomplete }
 { r0 = ##0x87ad1080; call stock_metadata }
 { r0 = ##0x87ad1080; r1 = #1024; call stock_clean }
 { barrier }
 { jump .Lcomplete }
.Lcustom:
 { r0 = memw(r21+#24) }
 { p0 = tstbit(r0,#0); if (!p0.new) jump:nt .Lbenchmark }
 { r0 = clrbit(r0,#0) }
 { callr r0 }
 { jump .Lcomplete }
.Lbenchmark:
 { call stock_flops }
 { jump .Lcomplete }
.Lmemory:
 { call stock_memory }
 { jump .Lcomplete }
.global dispatch_end
dispatch_end:
