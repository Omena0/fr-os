/*
 * stage1.S — Master Boot Record.
 *
 * The BIOS loads the first 512 bytes of the boot device to 0x7C00 in real mode
 * and transfers control with interrupts disabled. All this stage does is save
 * the boot drive the BIOS handed us in DL, pull stage2 off the disk, and jump to
 * it. No filesystem, no partitions, no privileged instructions — just enough to
 * get stage2 into memory and out of the BIOS's way.
 *
 * Code and data share one section so the MBR padding at the end is a
 * compile-time constant: the BIOS reads exactly 512 bytes and ignores anything
 * past them, and the .space directive uses a negative value if the layout ever
 * outgrows that budget, which the assembler rejects. A stage1 that no longer
 * fits is a build error rather than a mysteriously unbootable image.
 *
 * Addressed by CHS, not LBA.
 *
 * INT 13h/AH=42h is the obvious choice and it is what a loader of this size
 * should want: one interrupt moves the whole image and no geometry arithmetic
 * is needed. It is not used here because it is not reliable in practice. Every
 * firmware tested against this loader — SeaBIOS under QEMU with the disk on
 * IDE, on an explicit ich9-ahci controller, and behind virtio-blk — answers
 * AH=0x42 with AH=0x01, "invalid command", even for the very disk it just
 * booted the MBR from. CHS is the one disk interface that has existed
 * unchanged since the IBM PC, is implemented by every BIOS in service, and is
 * small enough to fit alongside the rest of a 512-byte MBR. The cost is a
 * geometry query and a per-sector loop; the benefit is that the bootloader
 * boots.
 *
 * The per-sector loop is not an accident. The BIOS specifies that a CHS
 * transfer must not cross a track boundary, and a one-sector transfer cannot
 * cross anything. Batching would save about sixty interrupts and cost a page
 * of careful arithmetic to get the boundary case right, and for 32 KiB of
 * bootloader the interrupts are not the bottleneck.
 */
	.code16
	.section .text
#include "boot_layout.h"

	.globl _start
_start:
	cli
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	$_start, %sp		/* stack inside our own image, growing down */
	cld

	/* The BIOS leaves the boot drive number in DL. Save it first, then
	 * announce ourselves over the serial port so a failure here is visible
	 * even though no video mode has been set up yet. */
	movb	%dl, drive
	call	serial_init
	movw	$msg_stage1, %si
	call	serial_puts

	/* Everything downstream assumes a hard disk in DL >= 0x80. A boot from a
	 * floppy or a CD would land here with a drive that can never hold the
	 * image, and reading it would fail much later with a message about the
	 * image rather than about the drive. */
	cmpb	$0x80, %dl
	jb	.no_disk

	call	read_geometry
	jc	.no_disk

	call	chs_read
	jnc	.loaded

	/* Report the status the BIOS returned rather than just the fact of the
	 * failure. AX still holds it: reset_segments preserves AX along with the
	 * flags, and neither the fail path nor the return touches it. "INT 13h
	 * read failed" alone cannot distinguish a bad CHS address from a
	 * function the firmware does not implement, and those two have completely
	 * different fixes. */
	movw	$msg_readerr, %si
	call	serial_puts
	movb	$' ', %al
	call	putc
	movw	%ax, %si
	call	puthexw
	jmp	.hang

.loaded:
	movw	$msg_stage2, %si
	call	serial_puts

	/* Hand over. Stage2 builds its own GDT and stack; nothing here survives
	 * the far jump except the drive number, which is left in the `drive` byte
	 * for stage2 to pick up.
	 *
	 * A far jump does not push anything, so a stack handover would need an
	 * explicit push before it -- and the MBR has no room for that. Four bytes
	 * of `pushw` push the image past the 512-byte boundary, which does not
	 * fail the build (the `.space` guard below only catches overflow in the
	 * other direction) and instead makes the loader stop partway through
	 * stage2 with a bare "INT 13h read failed".
	 *
	 * `drive` is therefore defined in the padding at a fixed offset rather
	 * than alongside the other loop state, and costs no code at all: the
	 * `movb %dl, drive` at the top of this file is the store. Stage2 also has
	 * a `stage1_boot_drive` byte of its own, but the two images are linked
	 * separately and share no symbol table, so there is no way for it to name
	 * ours -- the handover has to go through the one piece of memory both can
	 * agree on without agreeing on a name. See STAGE1_DRIVE_ADDR. */
	ljmp	$0x0000, $STAGE2_ADDR

	/* Every failure path ends here. There is no way to report back to
	 * whatever started this, and returning would mean a far jump into a
	 * segment that may not be the one we were entered with. Halting with the
	 * reason already on the serial port is the only honest outcome. */
.no_disk:
	movw	$msg_nodisk, %si
	call	serial_puts
.hang:
	cli
1:	hlt
	jmp	1b

/* ---------------------------------------------------------------------------
 * reset_segments — put DS and ES back to zero.
 *
 * A BIOS interrupt may return with DS and ES pointing anywhere. Real mode has
 * no flat address space, so every `mov ..., 0x7db4` in this file silently
 * becomes `mov ..., DS*16+0x7db4` and touches a completely different address.
 *
 * This is not a theoretical hazard. It cost an afternoon: after the AH=08h
 * call left DS pointing into the BIOS data area, the CHS loop's
 * `div word [bx_spt]` divided by whatever word happened to live at
 * DS*16+0x7db4, raised #DE, and the firmware's fault handler sent execution
 * back to the same instruction, so the machine sat in an endless divide fault
 * with nothing on the serial port. Every BIOS call here is therefore bracketed
 * by this routine, both before and after: before, because a call may also
 * arrive with the segments already wrong, and after, because the code that
 * reads the result needs them right.
 * --------------------------------------------------------------------------- */
reset_segments:
	/* The flags are saved as well as AX. `xorw %ax, %ax` sets CF, and this
	 * routine is called immediately after INT 13h precisely so the caller can
	 * test the carry the BIOS returned. Without the pushfw/popfw pair the
	 * reload silently eats that carry, every failing read looks like a
	 * successful one, and the loader jumps into 32 KiB of whatever happened
	 * to be in memory. */
	pushfw
	pushw	%ax
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	popw	%ax
	popfw
	ret

/* ---------------------------------------------------------------------------
 * read_geometry — INT 13h/AH=08h, read the drive parameters.
 *
 * Fills SECTORS_PER_TRACK and MAX_HEAD from CL bits 5-0 and DH respectively, and
 * CYL_COUNT from the cylinder fields, so that chs_read can convert an LBA into a
 * complete CHS triple.
 *
 * Two details here are the difference between working and not working.
 *
 * First, the CHS arguments must be zeroed before the call rather than left as
 * whatever the previous interrupt left behind. Firmware uses the incoming CHS
 * to decide which drive and which partition's geometry to report, so a garbage
 * request does not fail loudly: it returns a plausible-looking answer for the
 * wrong thing. An earlier version of this loader left CH/CL/DH alone and got
 * back a correct sector count paired with a head count of 0xFF, which became a
 * one-head geometry and put every CHS address on the wrong track.
 *
 * Second, none of the returned counts may be zero. The CHS conversion divides
 * by the sector and head counts, and a division by zero in real mode raises
 * #DE with no handler installed, which is a silent hang with nothing on the
 * serial port. A geometry that cannot support the stage2 window is rejected
 * outright rather than divided by.
 * --------------------------------------------------------------------------- */
read_geometry:
	xorw	%ax, %ax		/* AH=08h, request the current geometry */
	xorw	%cx, %cx		/* cylinder 0, sector 0 */
	xorw	%dx, %dx		/* head 0 */
	movb	$0x08, %ah
	call	reset_segments
	int	$0x13
	call	reset_segments
	jc	.geo_fail

	/* Sectors per track. CL bits 7-6 are the top two bits of the cylinder
	 * count, not part of this field. */
	movb	%cl, %al
	andb	$0x3f, %al
	cmpb	$0, %al
	jne	.geo_spt_ok
	movb	$1, %al
.geo_spt_ok:
	movb	%al, sectors_per_track

	/*
	 * AH has to be cleared before AX goes into bx_spt.
	 *
	 * `movb $0x08, %ah` above put the function number there and nothing since
	 * has overwritten it -- the interrupt is *supposed* to clobber AH with a
	 * status byte, and on success it returns 0, which is the only reason this
	 * has ever worked. A BIOS that answers a successful AH=08h with a non-zero
	 * status leaves that status in AH, and the word stored here becomes
	 * 0x0800 | spt: 2111 instead of 63, with the sectors-per-track byte
	 * alongside it still holding the correct 63. Nothing disagrees, so nothing
	 * complains.
	 *
	 * Every LBA->CHS division in chs_read then divides by the wrong number.
	 * `divw bx_spt` succeeds, the carry comes back clear, and the BIOS accepts
	 * the resulting sector number -- so stage1 reads the wrong sectors and
	 * carries on. The failure is a stage2 image assembled out of unrelated disk
	 * sectors, which presents as a far jump into memory with no GDT in it.
	 *
	 * The head count below already gets this right with `xorb %ah, %ah`, which
	 * is why only one of the two divisors was ever wrong.
	 */
	xorb	%ah, %ah
	movw	%ax, bx_spt

	/* Head count. DH is the highest head number, so the count is one more.
	 * A zero here means the BIOS did not fill the field in; 16 is the safe
	 * assumption, as every drive since 1994 has at least that many. */
	xorb	%ah, %ah
	movb	%dh, %al
	cmpb	$0, %al
	jne	.geo_heads
	movb	$15, %al
.geo_heads:
	incb	%al
	movb	%al, max_head
	movw	%ax, bx_heads

	/* The cylinder only ever appears in CH, never in CL bits 6-7. The stage2
	 * window is 63 sectors starting at LBA 1, and the cylinder is
	 * (LBA / spt) / heads, whose largest possible value over any geometry is
	 * 62 — for the degenerate one-sector-per-track, one-head disk. That fits
	 * in eight bits, so the two-bit extension in CL is dead code here and the
	 * MBR's 512-byte budget is better spent on the segment reloads. A disk so
	 * small that the window leaves cylinder 0 does not exist: at one sector
	 * per track, 63 sectors already spans 63 cylinders.
	 */
	cmpw	$0, bx_spt
	je	.geo_fail
	cmpw	$0, bx_heads
	je	.geo_fail

	xorw	%ax, %ax
	ret
.geo_fail:
	stc
	ret

/* ---------------------------------------------------------------------------
 * chs_read — read the stage2 window one sector at a time using CHS.
 *
 * LBA to CHS, for sectors-per-track S, head count H and cylinder count C:
 *
 *     sector   = (LBA mod S) + 1        BIOS sectors are 1-based
 *     track    = LBA div S
 *     head     = track mod H
 *     cylinder = track div H
 *
 * Every intermediate lives in memory rather than in a register. Sixteen-bit
 * real mode does not have enough registers to carry the loop state, both
 * division operands and the CHS fields at once, and the BIOS call itself
 * destroys everything except AX and the flags.
 *
 * The LBA in particular cannot live in BX: ES:BX is the destination buffer for
 * INT 13h, so BX is committed for the duration of the call. An earlier version
 * kept the LBA in BX and incremented it after the call, which incremented the
 * destination instead and left the loop reading sector 1 forever.
 * --------------------------------------------------------------------------- */
chs_read:
	movw	$STAGE2_LBA, chs_lba		/* current LBA */
	movw	$STAGE2_ADDR, %si		/* running destination pointer */

.chs_next:
	/*
	 * The bound is an immediate, not a register.
	 *
	 * The obvious spelling puts it in CX, and that reads correctly right up
	 * until the first sector has been fetched: CH and CL are the BIOS's
	 * cylinder and sector parameters, so CX is left holding the CHS triple
	 * of the sector just read rather than the end of the range. The next
	 * comparison is then "lba >= last sector number", which for LBA 1 is
	 * 2 >= 2 -- true -- and the loop exits having loaded exactly one sector.
	 * The rest of stage2 is whatever the BIOS left in memory, so the loader
	 * jumps into a region with no GDT in it and the far jump that enters
	 * protected mode faults. Four bytes of CX were being spent on a value
	 * that the very next instruction needs for something else.
	 */
	cmpw	$(STAGE2_LBA + STAGE2_MAX_SECTORS), chs_lba
	jae	.chs_done

	/* LBA to track and sector.
	 *
	 * `div` leaves the quotient in AX and the remainder in DX, so the track
	 * is what comes out of AX and the sector is what comes out of DX. Taking
	 * them the other way round still produces a syntactically valid CHS
	 * triple, and the BIOS accepts it and returns the wrong sector's data
	 * with the carry clear — which is why this reads as a successful boot
	 * into garbage rather than as an error.
	 *
	 * Both divisions need DX cleared first: it is the high half of the
	 * dividend, and whatever the geometry call left there turns a small LBA
	 * into a quotient too large for AX, which raises #DE. */
	movw	chs_lba, %ax
	xorw	%dx, %dx
	divw	bx_spt			/* AX = track, DX = LBA mod SPT */
	movw	%ax, chs_track

	/* No overflow check is needed on the sector: the remainder is at most
	 * SPT-1, so adding one for the BIOS's 1-based numbering cannot exceed
	 * SPT, and SPT is masked to six bits at geometry time. */
	movb	%dl, chs_sector
	incb	chs_sector

	movw	chs_track, %ax
	xorw	%dx, %dx
	divw	bx_heads		/* AX = cylinder, DX = head */
	movb	%al, chs_cyl
	movb	%dl, chs_head

	/* Assemble the INT 13h CHS arguments:
	 *   AL  = sectors to transfer
	 *   AH  = function
	 *   CH  = cylinder bits 0-7
	 *   CL  = sector in bits 0-5, cylinder bits 8-9 in bits 6-7
	 *   DH  = head
	 *   ES:BX = destination
	 *
	 * AL is set explicitly, and it has to be. The two divisions above leave
	 * the cylinder in AL, so writing AH alone would ask the firmware to
	 * transfer however many sectors the cylinder number happened to be — zero
	 * for the first sector of every track. SeaBIOS rejects a zero-length
	 * transfer with AH=01 "invalid command", which is indistinguishable from
	 * an unsupported function and sent the debugging a long way wrong.
	 */
	movw	%si, %bx
	movb	drive, %dl
	movb	chs_cyl, %ch
	movb	chs_sector, %cl
	movb	chs_head, %dh
	movb	$0x02, %ah			/* AH=02: read sectors */
	movb	$0x01, %al			/* AL=1: one sector per call */
	call	reset_segments
	int	$0x13
	call	reset_segments
	jc	.chs_fail

	/* A short read is a failure, not a success. The carry flag is clear when
	 * the firmware accepted the request, but AL then says how many sectors it
	 * actually moved, and a short count means the tail of stage2 is still
	 * whatever the BIOS left in memory. */
	cmpb	$0x01, %al
	jne	.chs_fail

	addw	$512, %si
	incw	chs_lba
	jmp	.chs_next

.chs_done:
	xorw	%ax, %ax
	ret
.chs_fail:
	stc
	ret

/* ---------------------------------------------------------------------------
 * serial_init — configure COM1 for 8N1 at 115200 baud.
 *
 * 115200 Hz / 1 = divisor 1. The modem control and FIFO control registers are
 * programmed explicitly so a state left behind by the BIOS cannot leave the
 * transmitter disabled or the FIFO holding stale bytes.
 *
 * Every register is named by its own address, because the two that are easy to
 * confuse here are the two the register file is full of: the interrupt enable
 * register and the divisor latch are the same port (0x3F9), and which one you
 * are writing is decided by a bit in the *line control* register at 0x3FB. This
 * used to keep DX at 0x3F8 across the first two writes, so both the "interrupts
 * off" byte and the DLAB byte went out of the *transmitter* instead of to the
 * IER and the LCR.
 *
 * The consequences were, in order of how much they mattered:
 *
 *   - DLAB was never set, so the two divisor writes that followed went to the
 *     interrupt enable register rather than to the divisor latch. The baud rate
 *     was therefore whatever the firmware left behind, and the one number in this
 *     routine that the comment above states as a fact was never actually
 *     programmed. stage2 programs its own divisor (3, not 1), so the rate also
 *     changed partway through every boot.
 *   - IER was programmed with 1 and then with 0 by the code that thought it was
 *     writing the divisor, so interrupts ended up off by accident rather than by
 *     decision.
 *   - Two bytes were transmitted to the serial port on every boot: the NUL from
 *     the IER write and the 0x80 from the DLAB write. That is the `00 80` at
 *     the very front of every boot log, ahead of anything stage1 prints, and it
 *     is why the first line of the log has two bytes of junk in it.
 *
 * Rewritten to address each register directly it is the same length, because
 * writing 0x03 to the LCR clears DLAB and sets 8N1 in the same byte -- the
 * separate "DLAB off" write at the end was redundant even when it reached the
 * right port.
 * --------------------------------------------------------------------------- */
serial_init:
	pushw	%ax
	pushw	%dx
	movw	$com1 + 1, %dx		/* IER */
	xorb	%al, %al			/* interrupts off */
	outb	%al, %dx
	movw	$com1 + 3, %dx		/* LCR */
	movb	$0x80, %al		/* DLAB on: divisor latch accessible */
	outb	%al, %dx
	movw	$com1, %dx		/* divisor latch low */
	movb	$0x01, %al		/* divisor low  = 1 */
	outb	%al, %dx
	movb	$0, %al			/* divisor high = 0 */
	outb	%al, %dx
	movw	$com1 + 3, %dx		/* LCR */
	movb	$0x03, %al		/* 8 data bits, no parity, 1 stop bit,
					 * and bit 7 clear: DLAB off */
	outb	%al, %dx
	movw	$com1 + 2, %dx		/* FCR */
	movb	$0xc7, %al		/* FIFO enabled, cleared, 14-byte trigger */
	outb	%al, %dx
	movw	$com1 + 4, %dx		/* MCR */
	movb	$0x0b, %al		/* DTR | RTS | OUT2 */
	outb	%al, %dx
	popw	%dx
	popw	%ax
	ret

/*
 * putc -- write AL to COM1, polling LSR.THRE. The spin is bounded; if the UART
 * never becomes ready the character is dropped, because wedging the machine
 * with no way to report anything is worse than a garbled log line.
 */
putc:
	pushw	%ax
	pushw	%dx
	pushw	%cx
	movb	%al, %ah			/* the spin below clobbers AL */
	movw	$com1 + 5, %dx		/* LSR -- NOT the spin counter */
	movw	$0x2000, %cx
.put_wait:
	inb	%dx, %al
	testb	$0x20, %al		/* LSR_THRE */
	jnz	.put_ready
	loop	.put_wait
	jmp	.put_done			/* UART wedged: discard and continue */
.put_ready:
	movw	$com1, %dx
	movb	%ah, %al
	outb	%al, %dx
.put_done:
	popw	%cx
	popw	%dx
	popw	%ax
	ret

/* ---------------------------------------------------------------------------
 * puthexw — write the word at DS:SI to COM1 as four hex digits.
 *
 * The nibble is biased by '0' - 10 and then nudged past the ten decimal digits,
 * which turns the digit/letter split into a single compare and costs about a
 * third less code than a table would. The MBR's 512 bytes are the binding
 * constraint on everything in this file.
 * --------------------------------------------------------------------------- */
puthexw:
	pushw	%ax
	pushw	%cx
	movw	$4, %cx
	/* The loop target has to be a label inside the body, not the function
	 * entry: `loop puthexw` would re-run the prologue and reload CX with 4
	 * on every pass, so the counter would never reach zero. */
.hex_loop:
	rolw	$4, %si
	movw	%si, %ax
	andw	$0x0f, %ax
	cmpb	$10, %al
	jae	.hex_alpha
	addb	$0x30, %al		/* '0' */
	jmp	.hex_emit
.hex_alpha:
	addb	$0x57, %al		/* 'a' is 10 further on than '0' is */
.hex_emit:
	call	putc
	loop	.hex_loop
	popw	%cx
	popw	%ax
	ret

serial_puts:
	pushw	%ax
	pushw	%si
	orb	$0x20, %al			/* AL is scratch until loaded */
.serial_next:
	movb	(%si), %al
	testb	%al, %al
	jz	.serial_done
	incw	%si
	call	putc
	jmp	.serial_next
.serial_done:
	popw	%si
	popw	%ax
	ret

/* ---------------------------------------------------------------------------
 * Data. The geometry and the running CHS fields live here rather than in
 * registers, inside the MBR's own 512 bytes because that is the only memory
 * this stage is guaranteed to own.
 * --------------------------------------------------------------------------- */
msg_stage1:
	.asciz	"\r\n[boot] stage1\r\n"
msg_stage2:
	.asciz	"[boot] stage2 loaded\r\n"
msg_nodisk:
	.asciz	"[boot] no fixed drive\r\n"
msg_readerr:
	.asciz	"[boot] INT 13h read failed\r\n"

	.align 2
	bx_spt:
	.word	1			/* sectors per track, for divw */
bx_heads:
	.word	1			/* head count, for divw */
sectors_per_track:
	.byte	1
max_head:
	.byte	1
chs_sector:
	.byte	1
chs_head:
	.byte	0
chs_cyl:
	.byte	0
chs_track:
	.word	0
chs_lba:
	.word	0

	.equ com1, 0x3f8

	/* The boot drive handover byte.
	 *
	 * Placed at a fixed offset in the padding rather than next to the other
	 * loop state, because stage2 has to be able to find it by a hardcoded
	 * address (the two images share no symbol table) and because the MBR has
	 * no room for a stack push. Landing it exactly on STAGE1_DRIVE_ADDR means
	 * the `movb %dl, drive` at the top of this file is the whole handover and
	 * no extra instruction is spent on it.
	 *
	 * The padding below is what makes this free: it is bytes the image was
	 * going to contain anyway, ahead of the signature. */
	.org	STAGE1_DRIVE_ADDR - 0x7C00
drive:
	.byte	0

	/* Pad to offset 510 and stamp the MBR signature. */
	.globl mbr_tail
mbr_tail:
	.org	510
	.byte	0x55, 0xaa
