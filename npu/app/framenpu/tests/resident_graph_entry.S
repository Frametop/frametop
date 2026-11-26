/* Resident graph cache entry v1. Context params[0] selects 0=uncached,
 * 1=cached code; every persistent data pointer remains uncached. Public SDK
 * payloads are unchanged. Cache publication uses only already-saved VTCM.
 * Worker-0 status14 arms before cached stores; status15 confirms cleanup on
 * normal and guarded fault return, before code/power may be reclaimed.
 * Tail jumps preserve the uncached entry return address. Forwarding stubs
 * preserve the original r0 target and clobber no additional registers.
 *
 * RG_LINK_ENTRY builds the loader variant instead: the slot image always runs
 * uncached and performs no per-request publication or cleanup. Opcode 32 with
 * a sealed operator image tail-jumps to that image in owned DDR through the
 * selected alias; everything else reaches the loader's own frame_job.
 */
#define FNPU_ENTRY_FRAME_BYTES 80
#ifdef TL_LINK_ENTRY
/* The slot runs uncached, so every packet is fetched from DDR again each time
 * it executes (~0.3 us per packet). A loop spent ~3 us per vector on the 64-vector
 * VTCM backup and restore; straight-line copies pairing a load and a store per
 * packet take ~0.33 us. Copies 64 vectors from r0 to r1 (both advanced);
 * clobbers v0 and v1. Teacher linked (Mercury) loader only; the others keep the loops. */
.macro vtcm_copy64
 { v0 = vmem(r0++#1) }
 .rept 31
 { v1 = vmem(r0++#1); vmem(r1++#1) = v0 }
 { v0 = vmem(r0++#1); vmem(r1++#1) = v1 }
 .endr
 { v1 = vmem(r0++#1); vmem(r1++#1) = v0 }
 { vmem(r1++#1) = v1 }
.endm
#endif
#if defined(RG_LINK_ENTRY) && defined(TL_LINK_ENTRY) && !defined(TL_SEAL_VA)
#define TL_SEAL_VA 0xf1ff3f80 /* last vector of the 16 KiB reservation; 0xf1ff4f80 for 20 KiB */
#endif
.section .text.entry,"ax",@progbits
.global custom_job
custom_job:
 { allocframe(#FNPU_ENTRY_FRAME_BYTES) }
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
#if defined(TL_LINK_ENTRY) && defined(RG_POOL)
 { if (!p0) jump pool_worker }
#else
 { if (!p0) jump finish }
#endif
 { r18 = evb }
 { memw(r16+#32) = r18 }
 { memw(r16+#24) = r29 }
 { r0 = ##vectors }
 { evb = r0 }
 { isync }
 { r19 = ##0xf0a93000 }
#ifdef RG_STAMP_RESTORED
 /* timing instrument: entry, before the VTCM backup (worker 2's unused status words 14-15) */
 { r1:0 = c31:30 }
 { memd(r16+#184) = r1:0 }
#endif
 { r0 = memw(r19+#12) }
 { p0 = tstbit(r0,#0) }
 { if (!p0) jump execute }
 { r0 = ##0x31000000 }
 { r1 = ##0xf0a94000 }
#ifdef TL_LINK_ENTRY
 vtcm_copy64
#else
 { r2 = #64 }
backup_loop:
 { v0 = vmem(r0+#0) }
 { vmem(r1+#0) = v0 }
 { r0 = add(r0,#128) }
 { r1 = add(r1,#128) }
 { r2 = add(r2,#-1) }
 { p0 = cmp.eq(r2,#0) }
 { if (!p0) jump backup_loop }
#endif
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
 { call resident_dispatch }
 { memw(r16+#8) = r0 }
recover:
 { r16 = ##0xf0a93400 }
#ifndef RG_LINK_ENTRY
 { r0 = memw(r16+#56) }
 { p0 = cmp.eq(r0,#0) }
 { if (p0) jump resident_cache_recovered }
 { call resident_cache_cleanup }
resident_cache_recovered:
#endif
 { r1:0 = c31:30 }
 { memd(r16+#48) = r1:0 }
 { r29 = memw(r16+#24) }
 /* A synchronous fault abandons C frames. deallocframe reads through FP, so
  * restoring SP alone can return into an abandoned frame and publish twice. */
 { r30 = add(r29,#FNPU_ENTRY_FRAME_BYTES) }
#if defined(TL_LINK_ENTRY) && defined(RG_POOL)
 /* Before the VTCM restore: if the image's worker pool is open for this
  * request (mailbox words 0/1 = seq/~seq in the saved page), post its exit
  * word and wait at most ~1 ms for both workers to acknowledge, so no worker
  * still writes the page once it holds the firmware's data again. The image
  * closes the pool itself on every return; this covers a fault on worker 0. */
 { r0 = memw(r16+#36) }
 { p0 = cmp.eq(r0,#0) }
 { if (p0) jump pool_drained }
 { r2 = ##0x31001400 }
 { r3 = ##0xf0a93000 }
 { r3 = memw(r3+#0) }
 { r4 = memw(r2+#0) }
 { p0 = cmp.eq(r4,r3) }
 { if (!p0) jump pool_drained }
 { r4 = memw(r2+#4) }
 { r5 = not(r3) }
 { p0 = cmp.eq(r4,r5) }
 { if (!p0) jump pool_drained }
 { memw(r2+#32) = r3 }
 { barrier }
 { r7:6 = c31:30 }
pool_drain_wait:
 { r4 = memw(r2+#36) }
 { r5 = memw(r2+#40) }
 { p0 = cmp.eq(r4,r3) }
 { p1 = cmp.eq(r5,r3) }
 { p0 = and(p0,p1) }
 { if (p0) jump pool_drained }
 { r5:4 = c31:30 }
 { r5:4 = sub(r5:4,r7:6) }
 { p0 = cmp.gtu(r4,#19200) }
 { if (p0) jump pool_drained }
 { pause(#64) }
 { jump pool_drain_wait }
pool_drained:
#endif
 { r17 = memw(r16+#28) }
 { r18 = memw(r16+#32) }
 { r0 = memw(r16+#36) }
 { p0 = cmp.eq(r0,#0) }
 { if (p0) jump registers }
#ifdef TL_LINK_ENTRY
 { r1 = ##0x31000000 }
 { r0 = ##0xf0a94000 }
 vtcm_copy64
#else
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
#endif
 { barrier }
registers:
#ifdef RG_STAMP_RESTORED
 /* timing instrument: after the VTCM restore (worker 0's status words 14-15) */
 { r1:0 = c31:30 }
 { memd(r16+#56) = r1:0 }
#endif
 { evb = r18 }
 { ssr = r17 }
 { isync }
#ifdef RG_STAMP_RESTORED
 /* timing instrument: after the context restore (worker 1's unused status words 14-15) */
 { r1:0 = c31:30 }
 { memd(r16+#120) = r1:0 }
#endif
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
/* Each target receives exactly the original r0 = EVB + vector*4. */
.macro resident_forward index
forward\index:
 { r0 = #\index; jump resident_forward_common }
.endm
 resident_forward 0
 resident_forward 1
 resident_forward 3
 resident_forward 5
 resident_forward 7
 resident_forward 9
 resident_forward 10
 resident_forward 11
 resident_forward 12
 resident_forward 13
 resident_forward 14
 resident_forward 15
 resident_forward 16
 resident_forward 17
 resident_forward 18
 resident_forward 19
 resident_forward 20
 resident_forward 21
 resident_forward 22
 resident_forward 23
 resident_forward 24
 resident_forward 25
 resident_forward 26
 resident_forward 27
 resident_forward 28
 resident_forward 29
 resident_forward 30
 resident_forward 31
 resident_forward 32
 resident_forward 33
 resident_forward 34
 resident_forward 35
 resident_forward 36
 resident_forward 37
 resident_forward 38
 resident_forward 39
 resident_forward 40
 resident_forward 41
 resident_forward 42
 resident_forward 43
 resident_forward 44
 resident_forward 45
 resident_forward 46
 resident_forward 47
 resident_forward 48
 resident_forward 49
 resident_forward 50
 resident_forward 51
 resident_forward 52
 resident_forward 53
 resident_forward 54
 resident_forward 55
 resident_forward 56
 resident_forward 57
 resident_forward 58
 resident_forward 59
 resident_forward 60
 resident_forward 61
 resident_forward 62
 resident_forward 63
resident_forward_common:
 { r0 = asl(r0,#2) }
 { r0 = add(r0,##0x87000000) }
 { jumpr r0 }
.balign 16
fault:
#if defined(TL_LINK_ENTRY) && defined(RG_POOL)
 /* Workers 1 and 2 run image code too: each faulting worker records into its
  * own status row and returns through its own finish. Other threads keep
  * the worker-0 row and path, as before. */
 { r0 = htid }
 { p0 = cmp.gtu(r0,#2) }
 { if (p0) r0 = #0 }
 { r1 = ##0xf0a93400 }
 { r0 = addasl(r1,r0,#6) }
#else
 { r0 = ##0xf0a93400 }
#endif
 { r1 = #2 }
 { memw(r0+#8) = r1 }
 { r1 = ssr }
 { memw(r0+#12) = r1 }
 { r1 = elr }
 { memw(r0+#16) = r1 }
 { r1 = badva }
 { memw(r0+#20) = r1 }
 { r1 = ##resident_cache_fault_return }
#if defined(TL_LINK_ENTRY) && defined(RG_POOL)
 { r0 = htid }
 { p0 = cmp.eq(r0,#1) }
 { p1 = cmp.eq(r0,#2) }
 { p0 = or(p0,p1) }
 { if (p0) r1 = ##pool_worker_recover }
#endif
 { elr = r1 }
 { rte }
#if defined(TL_LINK_ENTRY) && defined(RG_POOL)
/* Workers 1 and 2 (r16 = own status row): enter the sealed operator image,
 * cached, only for the Mercury request (opcode 111, params[0] = 1). The image
 * waits for worker 0's VTCM backup before touching VTCM. SP is saved as on
 * worker 0, so a fault returns here with the frame rebuilt; the worker never
 * writes its status result word, only its own fault fields on a fault. */
.balign 16
pool_worker:
 { r19 = ##0xf0a93000 }
 { r1 = memw(r19+#36) }
 { p0 = cmp.eq(r1,#111) }
 { if (!p0) jump finish }
 { r1 = memw(r19+#40) }
 { p0 = cmp.eq(r1,#1) }
 { if (!p0) jump finish }
 { r1 = ##TL_SEAL_VA }
 { r1 = memw(r1+#0) }
 { r2 = ##0x544c4e4b }
 { p0 = cmp.eq(r1,r2) }
 { if (!p0) jump finish }
 { memw(r16+#24) = r29 }
 { r0 = r19 }
 { r1 = ##0x88ff0000 }
 { callr r1 }
pool_worker_recover:
 { r0 = htid }
 { r16 = ##0xf0a93400 }
 { r16 = addasl(r16,r0,#6) }
 { r29 = memw(r16+#24) }
 { r30 = add(r29,#FNPU_ENTRY_FRAME_BYTES) }
 { jump finish }
#endif

.balign 16
resident_cache_fault_return:
 { r16 = ##0xf0a93400 }
 { r0 = #2 }
 { memw(r16+#8) = r0 }
 { jump recover }

#if defined(RG_LINK_ENTRY) && defined(TL_LINK_ENTRY)
/* Teacher linked loader (tests/teacher_link_admin.c): opcodes 110 and 111
 * enter the teacher operator image only while the seal word in the last
 * vector of its reservation (0x1ff3f80) holds the seal; the admin seal and
 * retire commands set and clear it. Everything else reaches frame_job. */
resident_dispatch:
 { r1 = memw(r19+#36) }
 { r1 = add(r1,#-110) }
 { p0 = cmp.gtu(r1,#1) }
 { if (p0) jump frame_job }
 { r1 = ##TL_SEAL_VA }
 { r1 = memw(r1+#0) }
 { r2 = ##0x544c4e4b }
 { p0 = cmp.eq(r1,r2) }
 { if (!p0) jump frame_job }
 { r1 = ##0xf1ff0000 }
 { r2 = ##0x88ff0000 }
 { r3 = memw(r19+#40) }
 { p0 = cmp.eq(r3,#1) }
 { r1 = mux(p0,r2,r1) }
 { jumpr r1 }
#elif defined(RG_LINK_ENTRY)
/* Resident state word 31 holds the seal only between the loader's seal and
 * retire commands, so an unsealed or retired image is never entered. The
 * uncached return address in r31 is preserved for the image's return. */
resident_dispatch:
 { r1 = memw(r19+#36) }
 { p0 = cmp.eq(r1,#32) }
 { if (!p0) jump frame_job }
 { r1 = ##0xf1a230fc }
 { r1 = memw(r1+#0) }
 { r2 = ##0x434f4445 }
 { p0 = cmp.eq(r1,r2) }
 { if (!p0) jump frame_job }
 { r1 = ##0xf1ff0000 }
 { r2 = ##0x88ff0000 }
 { r3 = memw(r19+#40) }
 { p0 = cmp.eq(r3,#1) }
 { r1 = mux(p0,r2,r1) }
 { jumpr r1 }
#else
/* Called only from uncached recover, with the entry return saved by allocframe.
 * Its r31 is disposable; deallocframe restores the firmware caller's return. */
resident_cache_cleanup:
 { r0 = ##0x87a92000 }
 { r1 = #128 }
resident_cache_clean_loop:
 { dccleaninva(r0) }
 { syncht }
 { icinva(r0) }
 { isync }
 { r0 = add(r0,#32) }
 { r1 = add(r1,#-1) }
 { p0 = cmp.eq(r1,#0) }
 { if (!p0) jump resident_cache_clean_loop }
 { barrier }
 { memw(r16+#60) = #1 }
 { jumpr r31 }

/* No nested call: retain the uncached entry's return address in r31, then
 * tail-jump to frame_job through the selected instruction alias. */
resident_dispatch:
 { r1 = memw(r19+#40) }
 { p0 = cmp.eq(r1,#1) }
 { if (!p0) jump frame_job }
 { r1 = memw(r19+#12) }
 { p0 = tstbit(r1,#0) }
 { if (!p0) jump frame_job }
 { r0 = ##0xf0a92000 }
 { r1 = ##0x31000000 }
 { r2 = #32 }
resident_stage_loop:
 { v0 = vmem(r0+#0) }
 { vmem(r1+#0) = v0 }
 { r0 = add(r0,#128) }
 { r1 = add(r1,#128) }
 { r2 = add(r2,#-1) }
 { p0 = cmp.eq(r2,#0) }
 { if (!p0) jump resident_stage_loop }
 { barrier }
 { memw(r16+#56) = #1 }
 { barrier }
 { r0 = ##0x87a92000 }
 { r1 = ##0x31000000 }
 { r2 = #32 }
resident_publish_vector:
 { v0 = vmem(r1+#0) }
 { vmem(r0+#0) = v0 }
 { barrier }
 { r3 = r0 }
 { r4 = #4 }
resident_publish_line:
 { dccleaninva(r3) }
 { syncht }
 { r3 = add(r3,#32) }
 { r4 = add(r4,#-1) }
 { p0 = cmp.eq(r4,#0) }
 { if (!p0) jump resident_publish_line }
 { r0 = add(r0,#128) }
 { r1 = add(r1,#128) }
 { r2 = add(r2,#-1) }
 { p0 = cmp.eq(r2,#0) }
 { if (!p0) jump resident_publish_vector }
 { r0 = ##0x87a92000 }
 { r1 = #128 }
resident_publish_instruction:
 { icinva(r0) }
 { isync }
 { r0 = add(r0,#32) }
 { r1 = add(r1,#-1) }
 { p0 = cmp.eq(r1,#0) }
 { if (!p0) jump resident_publish_instruction }
 { r0 = r19 }
 { r1 = ##(frame_job-0x69000000) }
 { jumpr r1 }
#endif
