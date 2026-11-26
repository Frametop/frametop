/* Experimental cached-code entry, derived from sdk/entry.S, with cache cleanup
 * at recover and complete frame unwinding for guarded synchronous faults.
 * Worker-0 status word 14 arms cleanup before the first cached store;
 * word 15 confirms completion before VTCM/register restoration and publication.
 * Host validation must require these markers before reclaiming code or power.
 * Recovery reconstructs the entry FP as saved SP + the allocframe size;
 * restoring SP alone leaves deallocframe using an abandoned C frame.
 * This does not recover a pipeline stall or a fault in cache cleanup itself.
 * Public SDK entry and runtime are unchanged. */
#define CACHE_ENTRY_FRAME_BYTES 80
.section .text.entry,"ax",@progbits
.global custom_job
custom_job:
 { allocframe(#CACHE_ENTRY_FRAME_BYTES) }
 { memd(r29+#0) = r17:16 }
 { memd(r29+#8) = r19:18 }
 { memd(r29+#16) = r21:20 }
 { memd(r29+#24) = r23:22 }
 { memd(r29+#32) = r25:24 }
 { memd(r29+#40) = r27:26 }
 { r0 = htid }
 { p0 = cmp.eq(r0,#4) }
 { if (p0) jump direct_master }
 { p0 = cmp.gtu(r0,#2) }
 { if (p0) jump skip }
 { r16 = ##0xf0a93400 }
 { r16 = addasl(r16,r0,#6) }
 { memw(r16+#4) = r0 }
 { r17 = ssr }
 { memw(r16+#28) = r17 }
 { p0 = cmp.eq(r0,#0) }
 { if (!p0) jump finish }
 { r18 = evb }
 { memw(r16+#32) = r18 }
 { memw(r16+#24) = r29 }
 { r0 = ##vectors }
 { evb = r0 }
 { isync }
 { r19 = ##0xf0a93000 }
 { r0 = memw(r19+#12) }
 { p0 = tstbit(r0,#0) }
 { if (!p0) jump execute }
 { r0 = ##0x31000000 }
 { r1 = ##0xf0a94000 }
 { r2 = #64 }
backup_loop:
 { v0 = vmem(r0+#0) }
 { vmem(r1+#0) = v0 }
 { r0 = add(r0,#128) }
 { r1 = add(r1,#128) }
 { r2 = add(r2,#-1) }
 { p0 = cmp.eq(r2,#0) }
 { if (!p0) jump backup_loop }
 { barrier }
 { r0 = #1 }
 { memw(r16+#36) = r0 }
 { r0 = setbit(r17,#26) }
 { ssr = r0 }
 { isync }
execute:
 { r1:0 = c31:30 }
 { memd(r16+#40) = r1:0 }
 { r0 = r19 }
 { call frame_job }
 { memw(r16+#8) = r0 }
recover:
 { r16 = ##0xf0a93400 }
 /* Both normal return and the synchronous fault handler arrive here uncached.
  * No stack or C return address is needed for cache cleanup. Leave the SDK
  * guarded window and HMX enable intact until every code line is clean. */
 { r0 = memw(r16+#56) }
 { p0 = cmp.eq(r0,#0) }
 { if (p0) jump cache_recovered }
 { r0 = ##0x87a92000 }
 { r1 = #128 }
cache_recover_loop:
 { dccleaninva(r0) }
 { syncht }
 { icinva(r0) }
 { isync }
 { r0 = add(r0,#32) }
 { r1 = add(r1,#-1) }
 { p0 = cmp.eq(r1,#0) }
 { if (!p0) jump cache_recover_loop }
 { barrier }
 { memw(r16+#60) = #1 }
cache_recovered:
 { r1:0 = c31:30 }
 { memd(r16+#48) = r1:0 }
 { r29 = memw(r16+#24) }
 { r30 = add(r29,#CACHE_ENTRY_FRAME_BYTES) }
 { r17 = memw(r16+#28) }
 { r18 = memw(r16+#32) }
 { r0 = memw(r16+#36) }
 { p0 = cmp.eq(r0,#0) }
 { if (p0) jump registers }
 { r0 = ##0x31000000 }
 { r1 = ##0xf0a94000 }
 { r2 = #64 }
restore_loop:
 { v0 = vmem(r1+#0) }
 { vmem(r0+#0) = v0 }
 { r0 = add(r0,#128) }
 { r1 = add(r1,#128) }
 { r2 = add(r2,#-1) }
 { p0 = cmp.eq(r2,#0) }
 { if (!p0) jump restore_loop }
 { barrier }
registers:
 { evb = r18 }
 { ssr = r17 }
 { isync }
finish:
 { barrier }
 { r0 = r16 }
 { r1 = ##0xf0a93000 }
 { r1 = memw(r1+#0) }
 { r17:16 = memd(r29+#0) }
 { r19:18 = memd(r29+#8) }
 { r21:20 = memd(r29+#16) }
 { r23:22 = memd(r29+#24) }
 { r25:24 = memd(r29+#32) }
 { r27:26 = memd(r29+#40) }
 { deallocframe }
 { memw(r0+#0) = r1; jumpr r31 }
/* The patched CPU-request case calls here on context 4. Keep HVX/HMX on
 * worker 0: the master only wakes workers in the callback-only mode and joins
 * their final return packets before publishing stock completion. Never return
 * just because the host went away or a deadline elapsed while work is active.
 */
direct_master:
 { r16 = ##0xf0b12000 }
 { r17 = ##0xf0a93400 }
 { r18 = ##0xf0a93000 }
 { r19 = memw(r18+#0) }
 { r0 = memw(r16+#24) }
 { r0 = clrbit(r0,#0) }
 { memw(r16+#24) = r0 }
 { memw(r16+#0) = #6 }
 { barrier }
 { r0 = #7 }
 { resume(r0) }
direct_join:
 { r0 = memw(r17+#0) }
 { p0 = cmp.eq(r0,r19) }
 { if (!p0) jump direct_pause }
 { r0 = memw(r17+#64) }
 { p0 = cmp.eq(r0,r19) }
 { if (!p0) jump direct_pause }
 { r0 = memw(r17+#128) }
 { p0 = cmp.eq(r0,r19) }
 { if (!p0) jump direct_pause }
 { barrier }
 { memw(r16+#0) = #3 }
 { jump skip }
direct_pause:
 { pause(#64) }
 { jump direct_join }
skip:
 { r17:16 = memd(r29+#0) }
 { r19:18 = memd(r29+#8) }
 { r21:20 = memd(r29+#16) }
 { r23:22 = memd(r29+#24) }
 { r25:24 = memd(r29+#32) }
 { r27:26 = memd(r29+#40) }
 { dealloc_return }
.balign 256
vectors:
 { jump forward0 }
 { jump forward1 }
 { jump fault }
 { jump forward3 }
 { jump fault }
 { jump forward5 }
 { jump fault }
 { jump forward7 }
 { jump fault }
 { jump forward9 }
 { jump forward10 }
 { jump forward11 }
 { jump forward12 }
 { jump forward13 }
 { jump forward14 }
 { jump forward15 }
 { jump forward16 }
 { jump forward17 }
 { jump forward18 }
 { jump forward19 }
 { jump forward20 }
 { jump forward21 }
 { jump forward22 }
 { jump forward23 }
 { jump forward24 }
 { jump forward25 }
 { jump forward26 }
 { jump forward27 }
 { jump forward28 }
 { jump forward29 }
 { jump forward30 }
 { jump forward31 }
 { jump forward32 }
 { jump forward33 }
 { jump forward34 }
 { jump forward35 }
 { jump forward36 }
 { jump forward37 }
 { jump forward38 }
 { jump forward39 }
 { jump forward40 }
 { jump forward41 }
 { jump forward42 }
 { jump forward43 }
 { jump forward44 }
 { jump forward45 }
 { jump forward46 }
 { jump forward47 }
 { jump forward48 }
 { jump forward49 }
 { jump forward50 }
 { jump forward51 }
 { jump forward52 }
 { jump forward53 }
 { jump forward54 }
 { jump forward55 }
 { jump forward56 }
 { jump forward57 }
 { jump forward58 }
 { jump forward59 }
 { jump forward60 }
 { jump forward61 }
 { jump forward62 }
 { jump forward63 }
forward0:
 { r0 = ##0x87000000 }
 { jumpr r0 }
forward1:
 { r0 = ##0x87000004 }
 { jumpr r0 }
forward2:
 { r0 = ##0x87000008 }
 { jumpr r0 }
forward3:
 { r0 = ##0x8700000c }
 { jumpr r0 }
forward4:
 { r0 = ##0x87000010 }
 { jumpr r0 }
forward5:
 { r0 = ##0x87000014 }
 { jumpr r0 }
forward6:
 { r0 = ##0x87000018 }
 { jumpr r0 }
forward7:
 { r0 = ##0x8700001c }
 { jumpr r0 }
forward8:
 { r0 = ##0x87000020 }
 { jumpr r0 }
forward9:
 { r0 = ##0x87000024 }
 { jumpr r0 }
forward10:
 { r0 = ##0x87000028 }
 { jumpr r0 }
forward11:
 { r0 = ##0x8700002c }
 { jumpr r0 }
forward12:
 { r0 = ##0x87000030 }
 { jumpr r0 }
forward13:
 { r0 = ##0x87000034 }
 { jumpr r0 }
forward14:
 { r0 = ##0x87000038 }
 { jumpr r0 }
forward15:
 { r0 = ##0x8700003c }
 { jumpr r0 }
forward16:
 { r0 = ##0x87000040 }
 { jumpr r0 }
forward17:
 { r0 = ##0x87000044 }
 { jumpr r0 }
forward18:
 { r0 = ##0x87000048 }
 { jumpr r0 }
forward19:
 { r0 = ##0x8700004c }
 { jumpr r0 }
forward20:
 { r0 = ##0x87000050 }
 { jumpr r0 }
forward21:
 { r0 = ##0x87000054 }
 { jumpr r0 }
forward22:
 { r0 = ##0x87000058 }
 { jumpr r0 }
forward23:
 { r0 = ##0x8700005c }
 { jumpr r0 }
forward24:
 { r0 = ##0x87000060 }
 { jumpr r0 }
forward25:
 { r0 = ##0x87000064 }
 { jumpr r0 }
forward26:
 { r0 = ##0x87000068 }
 { jumpr r0 }
forward27:
 { r0 = ##0x8700006c }
 { jumpr r0 }
forward28:
 { r0 = ##0x87000070 }
 { jumpr r0 }
forward29:
 { r0 = ##0x87000074 }
 { jumpr r0 }
forward30:
 { r0 = ##0x87000078 }
 { jumpr r0 }
forward31:
 { r0 = ##0x8700007c }
 { jumpr r0 }
forward32:
 { r0 = ##0x87000080 }
 { jumpr r0 }
forward33:
 { r0 = ##0x87000084 }
 { jumpr r0 }
forward34:
 { r0 = ##0x87000088 }
 { jumpr r0 }
forward35:
 { r0 = ##0x8700008c }
 { jumpr r0 }
forward36:
 { r0 = ##0x87000090 }
 { jumpr r0 }
forward37:
 { r0 = ##0x87000094 }
 { jumpr r0 }
forward38:
 { r0 = ##0x87000098 }
 { jumpr r0 }
forward39:
 { r0 = ##0x8700009c }
 { jumpr r0 }
forward40:
 { r0 = ##0x870000a0 }
 { jumpr r0 }
forward41:
 { r0 = ##0x870000a4 }
 { jumpr r0 }
forward42:
 { r0 = ##0x870000a8 }
 { jumpr r0 }
forward43:
 { r0 = ##0x870000ac }
 { jumpr r0 }
forward44:
 { r0 = ##0x870000b0 }
 { jumpr r0 }
forward45:
 { r0 = ##0x870000b4 }
 { jumpr r0 }
forward46:
 { r0 = ##0x870000b8 }
 { jumpr r0 }
forward47:
 { r0 = ##0x870000bc }
 { jumpr r0 }
forward48:
 { r0 = ##0x870000c0 }
 { jumpr r0 }
forward49:
 { r0 = ##0x870000c4 }
 { jumpr r0 }
forward50:
 { r0 = ##0x870000c8 }
 { jumpr r0 }
forward51:
 { r0 = ##0x870000cc }
 { jumpr r0 }
forward52:
 { r0 = ##0x870000d0 }
 { jumpr r0 }
forward53:
 { r0 = ##0x870000d4 }
 { jumpr r0 }
forward54:
 { r0 = ##0x870000d8 }
 { jumpr r0 }
forward55:
 { r0 = ##0x870000dc }
 { jumpr r0 }
forward56:
 { r0 = ##0x870000e0 }
 { jumpr r0 }
forward57:
 { r0 = ##0x870000e4 }
 { jumpr r0 }
forward58:
 { r0 = ##0x870000e8 }
 { jumpr r0 }
forward59:
 { r0 = ##0x870000ec }
 { jumpr r0 }
forward60:
 { r0 = ##0x870000f0 }
 { jumpr r0 }
forward61:
 { r0 = ##0x870000f4 }
 { jumpr r0 }
forward62:
 { r0 = ##0x870000f8 }
 { jumpr r0 }
forward63:
 { r0 = ##0x870000fc }
 { jumpr r0 }
.balign 16
fault:
 { r0 = ##0xf0a93400 }
 { r1 = #2 }
 { memw(r0+#8) = r1 }
 { r1 = ssr }
 { memw(r0+#12) = r1 }
 { r1 = elr }
 { memw(r0+#16) = r1 }
 { r1 = badva }
 { memw(r0+#20) = r1 }
 { r1 = ##cache_fault_return }
 { elr = r1 }
 { r0 = #2 }
 { rte }
.balign 16
cache_fault_return:
 /* Reestablish the exception result at a dedicated continuation. Do not leave
  * the status pointer in the C return-value register while abandoning frames. */
 { r16 = ##0xf0a93400 }
 { r0 = #2 }
 { memw(r16+#8) = r0 }
 { jump recover }
