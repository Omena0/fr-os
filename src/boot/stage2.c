/*
 * stage2.c — the 32-bit loader.
 *
 * stage2_entry.S has already left real mode, cleared the BSS and called
 * stage2_start. Everything here runs in 32-bit protected mode with flat
 * segments and paging still off, which is what makes the absolute pointers
 * below (E820 at 0x90000, bootinfo at 0x91000) usable as written.
 *
 * The order of the steps is load-bearing. Geometry has to be known before a
 * single sector can be addressed, the E820 map has to exist before the kernel
 * can be told what memory it has, and the page tables have to be built after
 * the kernel image is in the landing zone because the higher-half mapping is
 * computed from where it landed.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "boot.h"
#include "boot_layout.h"
#include "gdt.h"

/* From stage2_entry.S. */
extern void stage2_halt(void);

/* From stage2_long.S. */
extern void stage2_enter_long_mode(uint64_t pml4_phys, uint64_t entry,
				   uint64_t bootinfo_phys);

/* Supplied by stage1 through the stack: the BIOS boot drive number. */
extern uint8_t stage1_boot_drive;

static uint8_t boot_drive;

/* ------------------------------------------------------------------ io ------ */

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val)
{
	__asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
	uint8_t v;
	__asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

/*
 * The 8259 recognises a command only after four full bus cycles, and the
 * datasheet's recommended way to produce them is an I/O write to an unused
 * port. Without this, an outb to the UART immediately after a PIC command is
 * occasionally swallowed, and the symptom is an interrupt that is masked or
 * unmasked at random.
 */
static inline void io_wait(void)
{
	outb(0x80, 0);
}

/* --------------------------------------------------------------- serial ----- */

/*
 * COM1, 38400 8N1, no interrupts.
 *
 * The divisor is 115200 / 38400 = 3, which is what makes a `-serial stdio`
 * QEMU invocation readable. Interrupts stay off: the loader has no interrupt
 * handler worth re-entering, and the polled path below is a handful of
 * instructions per byte.
 */
static void serial_init(void)
{
	outb(COM1 + 1, 0x00);		/* no interrupts */
	outb(COM1 + 3, 0x80);		/* DLAB on */
	outb(COM1 + 0, 0x03);		/* divisor lo = 3 */
	outb(COM1 + 1, 0x00);		/* divisor hi = 0 */
	outb(COM1 + 3, 0x03);		/* DLAB off, 8N1 */
	outb(COM1 + 2, 0xC7);		/* FIFO on, clear, 14-byte trigger */
}

/*
 * Wait for the transmit holding register to drain.
 *
 * Bit 5 of the line status register is THRE. Polling it is what keeps a long
 * run of output from being truncated: without the wait, everything after the
 * first 16 bytes is dropped into a full FIFO, and a boot log loses exactly the
 * lines that say what went wrong.
 */
static void serial_putc(char c)
{
	unsigned spin = 0;

	while (!(inb(COM1 + 5) & 0x20)) {
		/* Bounded so a missing or wedged UART is a dropped byte rather
		 * than a hang with nothing on the port to explain it. */
		if (++spin > 100000u)
			return;
	}
	outb(COM1, (uint8_t)c);
}

static void serial_puts(const char *s)
{
	while (*s)
		serial_putc(*s++);
}

/*
 * 64-bit hex, as sixteen digits with no prefix and no leading-zero suppression.
 *
 * The digit table is a static local rather than an immediate lookup so that
 * the whole routine is position-independent. It also has to be `static
 * volatile`-adjacent in the sense that it must not be emitted into a region
 * the loader stack can grow into: see the note on stack32 in stage2_entry.S,
 * which is exactly the bug that made this print its own table.
 */
static void serial_puthex(uint64_t v)
{
	static const char digits[] = "0123456789abcdef";

	for (int i = 60; i >= 0; i -= 4)
		serial_putc(digits[(v >> i) & 0xF]);
}

/* Unsigned decimal, no leading zeros, zero prints as "0". */
static void serial_putdec(uint64_t v)
{
	char buf[24];
	int n = 0;

	if (v == 0) {
		serial_putc('0');
		return;
	}
	while (v > 0 && n < (int)sizeof(buf)) {
		buf[n++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (n > 0)
		serial_putc(buf[--n]);
}

#define LOG(...) do { serial_puts("[boot2] " __VA_ARGS__); } while (0)

/*
 * There is nowhere to return to. `fail` reports and halts rather than
 * returning, so every caller can treat it as noreturn and the compiler will
 * not keep values alive across it that are about to become meaningless.
 */
static void fail(const char *what)
{
	serial_puts("[boot2] FATAL: ");
	serial_puts(what);
	serial_puts("\r\n");
	stage2_halt();
}

/* ------------------------------------------------------------------ A20 ------ */

/*
 * Enable the A20 gate.
 *
 * The 8042 path is the correct one, but it is also the one that can fail
 * silently, and the failure is not a crash: without A20 the CPU still runs
 * and only address lines above 20 are wrong, so the first symptom is a BIOS
 * call that returns a plausible-looking answer built from the wrong memory.
 * The polling loop below therefore has a bounded retry and reports rather
 * than waiting forever, which is what the original did.
 */
static void a20_enable(void)
{
	/* 8042 command 0xD1: set the A20 bit in the port 0x92 value. */
	outb(0x64, 0xAD);		/* disable keyboard */
	outb(0x64, 0xD0);		/* read output port */
	uint8_t status = inb(0x60);
	outb(0x64, 0xD1);		/* write output port */
	outb(0x60, status | 0x02);	/* A20 = bit 1 */
	outb(0x64, 0xAE);		/* re-enable keyboard */

	LOG("A20 enabled via 8042\r\n");
}

/* ------------------------------------------------------- BIOS trampoline ----- */

/*
 * Every firmware call goes through the trampoline in stage2_entry.S, which
 * drops to real mode, far-calls the IVT handler and comes back.
 *
 * This is a real function call rather than inline asm at the call site, and
 * that is deliberate. GCC's rule for inline asm is that a register named as an
 * input is assumed to survive unless it is also named as an output or a
 * clobber, and naming it as both is a constraint error -- so call sites that
 * staged the firmware's registers through input constraints left the compiler
 * believing those registers were intact across a BIOS call that destroys every
 * one of them. An ordinary call states the truth: everything caller-saved is
 * destroyed.
 */
extern uint64_t bios_call(uint32_t vector, uint32_t a, uint32_t b, uint32_t c,
			  uint32_t d, uint32_t si, uint32_t di, uint32_t es);

/* INT 13h/AH=08h answers the geometry in ECX and EDX rather than in a buffer,
 * so the trampoline parks them here rather than returning them. */
extern uint32_t bios_ret_ecx;
extern uint32_t bios_ret_edx;

/* ELF program header types. Only PT_LOAD carries anything to copy. */
#define PT_LOAD 1

/* Handed to stage2_long.S, which reloads them after EFER.LME is set. */
extern uint64_t stage2_gdt_base;
extern uint16_t stage2_gdt_limit;

/* ------------------------------------------------------ BIOS call tracing --- */

/*
 * Two words of tripwire state, and a per-call counter.
 *
 * These exist because the failure this loader had for a long time presented as
 * "the loader stopped making progress partway through reading the kernel",
 * with nothing on the serial port and no exception reported. The two checks
 * below are what turned that into a specific, checkable claim: that the
 * firmware's `iret` had come back to the wrong place, or come back with the
 * stack pointer somewhere other than where it was set up.
 */
extern uint16_t bios_iret_magic;
extern uint16_t bios_post_sp;

static uint32_t diag_n;

static void diag_report(const char *what)
{
	serial_puts("\r\n[DIAG] ");
	serial_puts(what);
	serial_puts(" n=");
	serial_putdec(diag_n);
	serial_puts(" magic=");
	serial_puthex(bios_iret_magic);
	serial_puts(" sp=");
	serial_puthex(bios_post_sp);
	serial_puts("\r\n");
}

/* Clear the tripwire and count the call about to be made. */
static inline void diag_tick(void)
{
	diag_n++;
	bios_iret_magic = 0;
}

/*
 * The tripwire itself.
 *
 * The trampoline writes BIOS_RM_STACK_TOP to bios_post_sp and a fixed pattern
 * to bios_iret_magic on the instruction after `lcallw *BIOS_IVT_PTR`, so both
 * words are zero here and both are non-zero the moment a call returns normally.
 * A call that never comes back leaves them zero, and the check below is the
 * difference between "the loader stopped" and "the firmware's iret returned to
 * the wrong address".
 */
static inline void diag_check(uint32_t where)
{
	if (bios_iret_magic != 0x5AA5u || bios_post_sp != BIOS_RM_STACK_TOP) {
		diag_report("tripwire failed at ");
		serial_puthex(where);
		serial_puts("\r\n");
		stage2_halt();
	}
}

/* --------------------------------------------------------- CHS conversion --- */

/*
 * A linear address as the 16-bit segment:offset pair the real-mode services
 * take. Every one of them dereferences through a segment register, and a buffer
 * above 64 KiB is reachable that way only if the segment carries the high part:
 * 0x90000 is 0x9000:0x0000, not 0x0009:0x0000.
 */
static void seg16(uint32_t addr, uint16_t *seg, uint16_t *off)
{
	*seg = (uint16_t)(addr >> 4);
	*off = (uint16_t)(addr & 0x0F);
}

/* ------------------------------------------------------------ geometry ------ */

static uint32_t geom_spt;	/* sectors per track */
static uint32_t geom_heads;	/* heads */

/*
 * INT 13h/AH=08h, read the drive geometry.
 *
 * This is a BIOS call, not an arithmetic derivation: the loader must not
 * assume the EDD services exist, and it must not assume the geometry it guesses
 * is the one the firmware will accept. AH=08h answers in the registers rather
 * than in a buffer, which is why the trampoline parks ECX and EDX in
 * bios_ret_ecx/bios_ret_edx instead of returning them.
 */
static void geom_load(void)
{
	diag_tick();

	(void)bios_call(BIOS_INT_DISK, 0x0800u, 0, 0, boot_drive, 0, 0, 0);

	/* CL bits 5-0 are the sector count, CL bits 7-6 are the top of the
	 * cylinder count. */
	geom_spt = (uint32_t)(bios_ret_ecx & 0x3F);
	geom_heads = (uint32_t)((bios_ret_edx >> 8) & 0xFF);

	/*
	 * A divide by zero here is a #DE with no handler installed, which is a
	 * hang with nothing on the serial port to explain it. Both divisors are
	 * checked, and the check is what makes the failure legible.
	 */
	if (geom_spt == 0 || geom_heads == 0)
		fail("INT 13h/AH=08h reported a zero disk geometry");

	LOG("disk: ");
	serial_putdec(geom_spt);
	serial_puts(" sectors/track, ");
	serial_putdec(geom_heads);
	serial_puts(" heads\r\n");
}

/* ------------------------------------------------------------ disk read ------ */

/*
 * bios_read_bounce -- read `count` 512-byte sectors from `lba` into the bounce
 * window at 0x10000, which INT 13h addresses as 0x1000:0x0000.
 *
 * Addressed in CHS, one sector per interrupt, and not with AH=42h.
 *
 * AH=42h is the obvious choice and it is what a loader this size should use: one
 * interrupt moves the whole image and no geometry arithmetic is needed. It was
 * tried here first, with a disk address packet built both ways round -- the
 * documented layout, with the LBA at offset 4 and the buffer at offset 12, and
 * the layout SeaBIOS actually implements, which puts the buffer at offset 4 and
 * the LBA at offset 8. Both report success: AH comes back zero and the
 * sector-count field in the packet comes back as the count that was asked for.
 * Neither moves a byte. The bounce window, the low megabyte and the first four
 * megabytes all stay empty afterwards, so the loader reads back whatever was
 * already in memory and calls it a kernel image.
 *
 * That failure is worse than an error, because nothing reports one. It is also
 * why the packet is gone rather than merely unused: a reader that reports
 * success while transferring nothing is not something to leave in a loader with
 * a fallback, because the fallback never gets a chance to run.
 *
 * AH=02h transfers through ES:BX instead of a packet, and it works. One sector
 * per call, because the BIOS requires a CHS transfer to stay within a track and
 * a single sector cannot cross anything. Stage1 already reads itself this way,
 * and the reasons are recorded there.
 */
static bool bios_read_bounce(uint64_t lba, uint32_t count)
{
	uint64_t last = lba + count - 1u;
	uint32_t cur_lba;
	uint16_t boff, bseg;

	/*
	 * The CHS arithmetic below is 32-bit, and a 64-bit divide would pull
	 * __udivmoddi4 into a freestanding image that has no runtime library.
	 *
	 * Nothing near the disk is 2^32 sectors, so the check is not about
	 * capacity: it is the statement that the conversion is being asked about
	 * an LBA it can actually represent. A boot device bigger than 2 TiB would
	 * fail here, which is a far better failure than a truncated division.
	 */
	if (last > 0xFFFFFFFFull)
		fail("disk read beyond the range CHS can address");

	cur_lba = (uint32_t)lba;

	seg16((uint32_t)(uintptr_t)BOUNCE_ADDR, &bseg, &boff);

	while (count > 0) {
		uint32_t track = cur_lba / geom_spt;
		uint32_t sector = (cur_lba % geom_spt) + 1u;
		uint32_t head = track % geom_heads;
		uint32_t cylinder = track / geom_heads;

		/*
		 * CL carries the sector in bits 5-0 and the top two bits of the
		 * cylinder in bits 7-6, so the two fields have to be assembled into
		 * one byte rather than assigned. CH is the low byte of the cylinder,
		 * DH the head, DL the drive. AL is the sector count.
		 *
		 * The return value is AH. The carry the firmware sets on failure does
		 * not survive the trip: the flags the handler `iret`s back with are
		 * the ones the trampoline pushed before the call, and those had
		 * interrupts clear. AH is therefore the only failure report there is.
		 */
		uint32_t status = (uint32_t)bios_call(BIOS_INT_DISK, 0x0201u, boff,
						      ((cylinder >> 2) << 6) | sector,
						      (head << 8) | boot_drive,
						      0, 0, bseg);

		if ((status & 0xFF00u) != 0)
			return false;

		/* AL comes back as the number of sectors actually moved, and a short
		 * transfer means the image is truncated -- which must not be papered
		 * over by using whatever happened to be in the bounce window. */
		if ((status & 0xFFu) != 1u)
			return false;

		cur_lba++;
		count--;
	}
	return true;
}

/* ---------------------------------------------------------------- E820 ------- */

static struct e820_entry *const e820_map =
	(struct e820_entry *)(uintptr_t)E820_ADDR;
static uint32_t e820_count;

/*
 * The two INT 15h/EAX=E820h conventions, and the difference between them.
 *
 *   The specification form (ACPI 2.0 and later, and what real hardware
 *   implements) puts the 'SMAP' signature in EBX, the destination buffer as a
 *   *linear* address in EDX, and the "was that the last entry" answer in AX.
 *
 *   The original form (Ralf Brown, and what SeaBIOS implements) puts the
 *   signature in DX:BP, the index of the entry wanted in BX, and the
 *   destination as an ES:DI pair. It answers with the signature echoed in EAX,
 *   the index of the next entry in EBX -- zero on the last -- and the carry
 *   clear.
 *
 * A loader written to one of these and run against the other gets nothing at
 * all, and cannot tell from out here what happened: the call is refused, the
 * buffer keeps its poison, and the map comes back as whatever was there before.
 * So both are tried, and the one that answers is the one used.
 */
static uint32_t e820_call(uint32_t entry, uint32_t index, uint32_t di,
			  uint32_t es)
{
	diag_tick();

	/* SeaBIOS / Ralf Brown form: signature in DX:BP, index in BX, buffer
	 * at ES:DI. */
	uint64_t r = bios_call(BIOS_INT_E820, 0xE820u, index, 24u,
			       0x534D4150u, 0, di, es);

	if ((uint32_t)(r & 0xFFFFFFFFu) == 0x534D4150u)
		return (uint32_t)(r >> 32);	/* EBX: next index, 0 on last */

	/* Specification form: signature in EBX, linear buffer in EDX. The
	 * 0x90000 map is above 64 KiB, so the buffer address has to go in EDX
	 * and the answer comes back in AX with bit 19 set. */
	r = bios_call(BIOS_INT_E820, 0xE820u, 0x534D4150u, 24u,
		      E820_ADDR + entry * sizeof(struct e820_entry), 0, 0, 0);

	if ((r & 0xFFFFFFFFu) == 0x534D4150u && (r & (1ULL << 19)))
		return 0;			/* no continuation */

	return 0;
}

/*
 * Fill e820_map from the firmware.
 *
 * The continuation value is the authority on whether another entry follows.
 * The carry flag alone is not dependable across firmware, and the loader's
 * earlier version trusted it, which is how a map could come back with seven
 * entries and a printout full of zeroes.
 */
static void e820_query(void)
{
	uint64_t last_base = 0;
	bool have_last = false;
	uint16_t es;
	uint16_t di;

	seg16((uint32_t)(uintptr_t)E820_ADDR, &es, &di);

	LOG("E820 memory map:\r\n");

	for (;;) {
		uint8_t *entry = (uint8_t *)e820_map + e820_count *
				 sizeof(struct e820_entry);

		if (e820_count >= E820_MAX_ENTRIES) {
			LOG("  map full at ");
			serial_putdec(e820_count);
			serial_puts(" entries\r\n");
			break;
		}

		/* Poison the entry so a refused call is visible rather than
		 * silently accepted as a map full of whatever was on the stack. */
		for (unsigned i = 0; i < sizeof(struct e820_entry); i++)
			entry[i] = 0xA5;

		uint32_t next = e820_call(e820_count, e820_count, di, es);

		uint64_t base = *(const uint64_t *)(const void *)entry;
		uint64_t length = *(const uint64_t *)(const void *)(entry + 8);
		uint32_t type = *(const uint32_t *)(const void *)(entry + 16);

		e820_map[e820_count].base = base;
		e820_map[e820_count].length = length;
		e820_map[e820_count].type = type;
		e820_map[e820_count].acpi_extended =
			*(const uint32_t *)(const void *)(entry + 20);

		serial_puts("  0x");
		serial_puthex(base);
		serial_puts(" + 0x");
		serial_puthex(length);
		serial_puts(" type ");
		serial_putdec(type);
		serial_puts("\r\n");

		/* Some firmware repeats the previous entry as a terminator. */
		if (have_last && base == last_base && length == 0)
			break;

		last_base = base;
		have_last = true;
		e820_count++;

		if (next == 0)
			break;
	}

	if (e820_count == 0)
		fail("E820 returned no memory map entries");
}

/* ----------------------------------------------------------------- VBE ------- */

static struct framebuffer_info fb_info;
static bool fb_enabled;

static uint8_t *const vbe_ctrl = (uint8_t *)(uintptr_t)VBE_SCRATCH_ADDR;
static uint8_t *const vbe_mode = (uint8_t *)(uintptr_t)(VBE_SCRATCH_ADDR + 0x100);

/* INT 10h/AH=4F01h: fill in the 256-byte mode info block for `mode`. */
static bool vbe_get_mode_info(uint16_t mode)
{
	uint16_t es;
	uint16_t di;

	seg16((uint32_t)(uintptr_t)vbe_mode, &es, &di);

	diag_tick();

	uint32_t ret = (uint32_t)bios_call(BIOS_INT_VIDEO, 0x4F01u, 0,
					   (uint32_t)mode, 0, 0, di, es);

	return ret == 0x004Fu;
}

/* INT 10h/AH=4F02h: set the video mode, with the LFB bits in bit 14 of BH. */
static bool vbe_set_mode(uint16_t mode)
{
	uint16_t es;
	uint16_t di;

	seg16(0, &es, &di);

	diag_tick();

	uint32_t ret = (uint32_t)bios_call(BIOS_INT_VIDEO, 0x4F02u, 0x4000u,
					   (uint32_t)mode, 0, 0, di, es);

	return ret == 0x004Fu;
}

/*
 * Find and enable a linear framebuffer mode.
 *
 * Not a failure if there isn't one: the kernel is told fb_enabled = false and
 * runs without console output beyond the serial port. That is the difference
 * between a missing feature and an unbootable loader, and it is why the
 * "unavailable" paths below return rather than calling fail().
 */
static void vbe_setup(void)
{
	uint8_t *ctrl = vbe_ctrl;
	uint16_t es;
	uint16_t di;
	uint32_t best_score = 0;
	uint16_t best_mode = 0;

	seg16((uint32_t)(uintptr_t)ctrl, &es, &di);

	LOG("querying VESA BIOS...\r\n");

	diag_tick();

	/* INT 10h/AX=4F00h: the controller information block. */
	uint32_t ret = (uint32_t)bios_call(BIOS_INT_VIDEO, 0x4F00u, 0, 0, 0,
					   0, di, es);

	if (ret != 0x004Fu) {
		LOG("  no VESA BIOS; framebuffer unavailable\r\n");
		return;
	}

	serial_puts("  VBE ");
	serial_puthex(ctrl[2]);
	serial_puts(".");
	serial_puthex(ctrl[3]);
	serial_puts("\r\n");

	/* The mode list is a far pointer into the firmware's own memory. */
	best_score = 0;
	best_mode = 0;

	/* Walk the mode list. Each entry is a a far pointer, and the two words
	 * that follow it are the segment and the offset. */
	uint16_t list_seg = (uint16_t)(ctrl[0x0E] | ((uint16_t)ctrl[0x0F] << 8));
	uint16_t list_off = (uint16_t)(ctrl[0x10] | ((uint16_t)ctrl[0x11] << 8));

	for (unsigned i = 0; i < 256u; i++) {
		uint16_t entry[2];
		uint16_t m;

		/* Read the two-word far pointer through the segment the
		 * controller block named. Paging is off and the flat segments have
		 * base 0, so the linear address is just seg << 4 + off. */
		uint32_t linear = ((uint32_t)list_seg << 4) + list_off + i * 4u;
		uint8_t *p = (uint8_t *)(uintptr_t)linear;

		entry[0] = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
		entry[1] = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));

		m = entry[1];
		if (m == 0xFFFFu)
			break;

		if (!vbe_get_mode_info(m))
			continue;

		/* Mode info attributes word: bit 7 is the LFB flag, bit 4 is
		 * graphics, bit 0 is supported. */
		uint16_t attrs = (uint16_t)(vbe_mode[0] | ((uint16_t)vbe_mode[1] << 8));

		if (!(attrs & 0x0081u))
			continue;		/* not supported, or not a graphics mode */
		if (!(attrs & 0x0100u))
			continue;		/* no linear framebuffer */

		uint32_t width = (uint32_t)(vbe_mode[0x12] | ((uint32_t)vbe_mode[0x13] << 8));
		uint32_t height = (uint32_t)(vbe_mode[0x14] | ((uint32_t)vbe_mode[0x15] << 8));
		uint32_t bpp = vbe_mode[0x19];
		uint32_t phys = (uint32_t)(vbe_mode[0x28] | ((uint32_t)vbe_mode[0x29] << 8) |
					   ((uint32_t)vbe_mode[0x2A] << 16) |
					   ((uint32_t)vbe_mode[0x2B] << 24));

		if (phys == 0)
			continue;

		uint32_t score = width * height;
		if (score <= best_score)
			continue;

		best_score = score;
		best_mode = m;

		fb_info.address = phys;
		fb_info.width = width;
		fb_info.height = height;
		fb_info.bpp = (uint8_t)bpp;
		fb_info.pitch = (uint32_t)(vbe_mode[0x10] | ((uint32_t)vbe_mode[0x11] << 8));
		fb_info.red_shift = (uint8_t)(vbe_mode[0x31] >> 2);
		fb_info.green_shift = (uint8_t)(((vbe_mode[0x31] >> 5) & 0x7) |
						(((vbe_mode[0x32] >> 3) & 0x1E)));
		fb_info.blue_shift = (uint8_t)(vbe_mode[0x32] >> 3);
	}

	if (best_mode == 0) {
		LOG("  no linear framebuffer mode found\r\n");
		return;
	}

	serial_puts("  mode 0x");
	serial_puthex(best_mode);
	serial_puts("\r\n");

	if (!vbe_set_mode(best_mode)) {
		LOG("  mode set refused; framebuffer unavailable\r\n");
		return;
	}

	/* The mode set can invalidate the mode info block, so the values the
	 * kernel needs are read back rather than trusted from before. */
	if (!vbe_get_mode_info(best_mode)) {
		LOG("  mode info lost after mode set\r\n");
		return;
	}

	fb_enabled = true;
}

/* --------------------------------------------------------- kernel load ------ */

static uint64_t kernel_entry_vaddr;
static uint32_t kernel_bytes_loaded;

/*
 * copy_from_disk — copy `len` bytes starting at file offset `off` to physical
 * address `dst`.
 *
 * One sector at a time through the bounce window. The window is 4 KiB but a
 * sector is 512, and the copy is deliberately not widened: the bounce buffer
 * is the only place the firmware can be asked to write above 64 KiB without a
 * packet, and keeping the transfer to a single sector keeps the AH=02h
 * geometry rule (a CHS transfer may not cross a track) trivially satisfied.
 */
static void copy_from_disk(uint64_t off, uint64_t dst, uint64_t len)
{
	uint8_t *out = (uint8_t *)(uintptr_t)dst;

	while (len > 0) {
		uint32_t sector = (uint32_t)(off / 512);
		uint32_t in_sector = (uint32_t)(off % 512);
		uint32_t chunk = 512 - in_sector;

		if (chunk > BOUNCE_BYTES)
			chunk = BOUNCE_BYTES;
		if ((uint64_t)chunk > len)
			chunk = (uint32_t)len;

		if (!bios_read_bounce((uint64_t)KERNEL_LBA + sector, 1))
			fail("kernel read failed");

		uint8_t *src = (uint8_t *)(uintptr_t)BOUNCE_ADDR + in_sector;
		for (uint32_t i = 0; i < chunk; i++)
			out[i] = src[i];

		out += chunk;
		off += chunk;
		dst += chunk;
		len -= chunk;
		kernel_bytes_loaded += chunk;

		if (kernel_bytes_loaded > KERNEL_MAX_BYTES)
			fail("kernel image exceeds its reserved landing zone");
	}
}

/*
 * Read the kernel ELF headers and copy every PT_LOAD segment into the landing
 * zone.
 *
 * The headers are read into the landing zone itself rather than a separate
 * scratch buffer, because the landing zone is the one place stage2 owns that is
 * large enough to hold them and is guaranteed not to be aliased by anything the
 * firmware is using. The first sector is read once and reused for the header
 * and all five program headers, which is why the header read is not inside the
 * loop.
 */
static void load_kernel(void)
{
	typedef uint64_t elf64_addr;
	typedef uint64_t elf64_off;
	typedef uint64_t elf64_xword;
	typedef uint16_t elf64_half;
	typedef uint32_t elf64_word;

	struct elf64_ehdr {
		unsigned char e_ident[16];
		elf64_half e_type;
		elf64_half e_machine;
		elf64_word e_version;
		elf64_addr e_entry;
		elf64_off e_phoff;
		elf64_off e_shoff;
		elf64_word e_flags;
		elf64_half e_ehsize;
		elf64_half e_phentsize;
		elf64_half e_phnum;
		elf64_half e_shentsize;
		elf64_half e_shnum;
		elf64_half e_shstrndx;
	};

	struct elf64_phdr {
		elf64_word p_type;
		elf64_word p_flags;
		elf64_off p_offset;
		elf64_addr p_vaddr;
		elf64_addr p_paddr;
		elf64_off p_filesz;
		elf64_off p_memsz;
		elf64_xword p_align;
	};

	struct elf64_phdr phdr;
	uint64_t phdr_off;
	bool loaded_any = false;

	LOG("loading kernel...\r\n");

	/* The ELF header lives in the first sector of the kernel image. */
	if (!bios_read_bounce(KERNEL_LBA, 1))
		fail("cannot read kernel ELF header");

	const struct elf64_ehdr *eh =
		(const struct elf64_ehdr *)(uintptr_t)KERNEL_LANDING_ADDR;

	if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
		fail("kernel is not an ELF image");
	if (eh->e_ident[4] != 2)
		fail("kernel ELF is not ELF64");
	if (eh->e_machine != 62)
		fail("kernel ELF is not x86-64");

	kernel_entry_vaddr = eh->e_entry;

	LOG("  entry 0x");
	serial_puthex(kernel_entry_vaddr);
	serial_puts(", ");
	serial_putdec(eh->e_phnum);
	serial_puts(" program headers\r\n");

	phdr_off = eh->e_phoff;

	for (unsigned i = 0; i < eh->e_phnum; i++) {
		/* Program headers beyond the first sector have to be re-read. The
		 * bounce window is the source, so a header that straddles a sector
		 * boundary is copied out through the landing zone and back. */
		uint64_t need = phdr_off + sizeof(phdr);
		uint32_t sector = (uint32_t)(phdr_off / 512);
		uint32_t in_sector = (uint32_t)(phdr_off % 512);
		uint8_t *dst_ph = (uint8_t *)(uintptr_t)(KERNEL_LANDING_ADDR + phdr_off);

		if (in_sector + sizeof(phdr) > 512) {
			/* Straddles: read both sectors and assemble. */
			if (!bios_read_bounce((uint64_t)KERNEL_LBA + sector, 1))
				fail("cannot read kernel program headers");
			for (uint32_t i = 0; i < 512 - in_sector; i++)
				dst_ph[i] = *(const volatile uint8_t *)
					(uintptr_t)(BOUNCE_ADDR + in_sector + i);
			if (!bios_read_bounce((uint64_t)KERNEL_LBA + sector + 1, 1))
				fail("cannot read kernel program headers");
			for (uint32_t i = 0; i < sizeof(phdr) - (512 - in_sector); i++)
				dst_ph[512 - in_sector + i] =
					*(const volatile uint8_t *)
						(uintptr_t)(BOUNCE_ADDR + i);
		} else {
			if (!bios_read_bounce((uint64_t)KERNEL_LBA + sector, 1))
				fail("cannot read kernel program headers");
			for (uint32_t i = 0; i < sizeof(phdr); i++)
				dst_ph[i] = *(const volatile uint8_t *)
					(uintptr_t)(BOUNCE_ADDR + in_sector + i);
		}
		(void)need;

		phdr_off += eh->e_phentsize;

		/* Read the header back into the local. It was just written at
		 * dst_ph, but through a `uint8_t *`, so nothing in the type system
		 * says `phdr` is initialised -- and reading it directly from
		 * dst_ph would be a strict-aliasing violation besides. */
		phdr = *(const struct elf64_phdr *)(const void *)dst_ph;

		if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0)
			continue;

		serial_puts("  LOAD vaddr 0x");
		serial_puthex(phdr.p_vaddr);
		serial_puts(" filesz 0x");
		serial_puthex(phdr.p_filesz);
		serial_puts(" memsz 0x");
		serial_puthex(phdr.p_memsz);
		serial_puts("\r\n");

		/* The kernel is linked for the higher half but loaded into the
		 * landing zone at a fixed physical address; the bootstrap page
		 * tables map one to the other. Relocation is therefore a plain
		 * file-offset to landing-zone copy, and p_paddr is informational. */
		if (phdr.p_offset + phdr.p_filesz > KERNEL_MAX_BYTES)
			fail("kernel segment out of range");

		copy_from_disk(phdr.p_offset,
			       KERNEL_LANDING_ADDR + phdr.p_offset, phdr.p_filesz);

		/* Bytes between p_filesz and p_memsz are .bss: not present in the
		 * file, and must be zero. Stage2 zeroes them because the kernel's
		 * own allocator is not running yet. */
		if (phdr.p_memsz > phdr.p_filesz) {
			uint64_t bss_len = phdr.p_memsz - phdr.p_filesz;
			uint8_t *p = (uint8_t *)(uintptr_t)(KERNEL_LANDING_ADDR +
							     phdr.p_offset + phdr.p_filesz);

			LOG("  zeroing ");
			serial_puthex(bss_len);
			serial_puts(" bytes of bss\r\n");
			for (uint64_t n = 0; n < bss_len; n++)
				p[n] = 0;
		}

		loaded_any = true;
	}

	if (!loaded_any)
		fail("kernel ELF has no loadable segments");

	/* Program headers beyond the first sector were read into the landing
	 * zone as scratch; leaving them there would corrupt .text. */
	LOG("  kernel loaded, ");
	serial_putdec(kernel_bytes_loaded);
	serial_puts(" bytes\r\n");
}

/* ------------------------------------------------- bootstrap paging --------- */

#define PT_PRESENT (1ULL << 0)
#define PT_WRITE   (1ULL << 1)
#define PT_USER    (1ULL << 2)
#define PT_ACCESSED (1ULL << 5)
#define PT_DIRTY   (1ULL << 6)
#define PT_PS      (1ULL << 7)     /* large page: 2 MiB at the PD level */
#define PT_GLOBAL  (1ULL << 8)
#define PT_NX      (1ULL << 63)

static uint64_t boot_pml4_phys;

/*
 * A temporary identity map of the first 4 GiB, using 2 MiB pages.
 *
 * 2 MiB pages matter even here: a 4 KiB-granular map of 4 GiB would need 2048
 * page-table pages, which does not fit the 64 KiB window reserved for boot
 * tables. It is also what the kernel will use for its real direct map.
 *
 * The window layout is fixed:
 *   0x70000  PML4
 *   0x71000  PDPT (identity: entries 0-3; higher-half: entries 508-511)
 *   0x72000  PD #0  (0 - 1 GiB)
 *   0x73000  PD #1  (1 - 2 GiB)
 *   0x74000  PD #2  (2 - 3 GiB)
 *   0x75000  PD #3  (3 - 4 GiB)
 */
#define BOOT_PML4_OFF  0x0000
#define BOOT_PDPT_OFF  0x1000
#define BOOT_PD0_OFF   0x2000
#define BOOT_PT_BASE   0x70000u

static void boot_page_tables_init(void)
{
	uint64_t base = BOOT_PT_BASE;
	uint64_t *pml4 = (uint64_t *)(uintptr_t)(base + BOOT_PML4_OFF);
	uint64_t *pdpt = (uint64_t *)(uintptr_t)(base + BOOT_PDPT_OFF);
	uint64_t *pd0 = (uint64_t *)(uintptr_t)(base + BOOT_PD0_OFF);
	uint64_t pdpt_high;

	LOG("building bootstrap page tables...\r\n");

	/* Zero the whole window. Paging is about to be enabled with CR3
	 * pointing here, and a single non-zero word in a table that is supposed
	 * to be empty is a wild mapping rather than a fault. */
	for (uint64_t i = 0; i < 0x6000 / 8; i++)
		((uint64_t *)(uintptr_t)base)[i] = 0;

	/* Identity map: PDPT entries 0-3 cover 0-4 GiB through the four PDs. */
	for (unsigned i = 0; i < 4; i++) {
		pdpt[i] = (base + BOOT_PD0_OFF + (uint64_t)i * 0x1000) |
			  PT_PRESENT | PT_WRITE | PT_USER;
	}

	/* 512 PD entries of 2 MiB each would be 1 GiB per PD. */
	for (unsigned i = 0; i < 4; i++) {
		uint64_t *pd = pd0 + (uint64_t)i * 512;

		for (unsigned j = 0; j < 512; j++)
			pd[j] = (((uint64_t)i * 512 + j) << 21) |
				PT_PRESENT | PT_WRITE | PT_USER | PT_PS;
	}

	/*
	 * Higher half. The kernel is linked at 0xffffffff80000000, which is
	 * virtual 0xFFFFFFFF80000000; with 512 GiB of canonical space below it,
	 * that is PML4 index 256 and up.
	 *
	 * PDPT 508-511 alias PDs 0-3, so the same four page directories serve
	 * both the identity map and the higher-half window. 508 covers
	 * 0xFFFFFFFF80000000 through 0xFFFFFFFFBFFFFFFF and 509 covers
	 * 0xFFFFFFFFC0000000 upward, which is where the kernel's vmalloc area
	 * begins.
	 */
	for (unsigned i = 0; i < 4; i++) {
		pdpt[508 + i] = (base + BOOT_PD0_OFF + (uint64_t)i * 0x1000) |
				PT_PRESENT | PT_WRITE | PT_USER;
	}

	/* The kernel image itself is in the landing zone, which the identity
	 * map already covers, so the higher-half alias above is what makes
	 * 0xffffffff80000000 resolve to it. */
	pdpt_high = (uint64_t)(base + BOOT_PDPT_OFF);
	(void)pdpt_high;

	pml4[0] = (base + BOOT_PDPT_OFF) | PT_PRESENT | PT_WRITE | PT_USER;
	/* PML4 entry 256 is the first higher-half half. */
	pml4[256] = (base + BOOT_PDPT_OFF) | PT_PRESENT | PT_WRITE | PT_USER;

	boot_pml4_phys = base + BOOT_PML4_OFF;

	LOG("  PML4 at 0x");
	serial_puthex(boot_pml4_phys);
	serial_puts("\r\n");
}

/*
 * Install a 64-bit GDT for the long-mode switch.
 *
 * The initial table in stage2_entry.S has no L bits, so it cannot be used
 * once EFER.LME is set. This builds one with the L bit on the code segments
 * and a TSS-adjacent hole, and hands the base and limit to stage2_long.S.
 */
static void boot_gdt_init(void)
{
	static uint64_t gdt[8];

	/* Layout:
	 * 0x00 null
	 * 0x08 64-bit kernel code (L=1)
	 * 0x10 32-bit data
	 * 0x18 64-bit user code (L=1, DPL=3)
	 * 0x20 user data (DPL=3)
	 */
	gdt[0] = 0;
	gdt[1] = 0x00AF9A000000FFFFULL;	/* 0x08 64-bit code, base 0, 4G */
	gdt[2] = 0x00CF93000000FFFFULL;	/* 0x10 data, base 0, 4G */
	gdt[3] = 0x0000FA000000FFFFULL;	/* 0x18 64-bit user code, DPL 3 */
	gdt[4] = 0x00CFF3000000FFFFULL;	/* 0x20 user data, DPL 3 */

	stage2_gdt_base = (uint64_t)(uintptr_t)gdt;
	stage2_gdt_limit = (uint16_t)(sizeof(gdt) - 1);

	LOG("  GDT at 0x");
	serial_puthex(stage2_gdt_base);
	serial_puts("\r\n");
}

/* ------------------------------------------------------------------ main ----- */

static struct bootinfo *const bootinfo =
	(struct bootinfo *)(uintptr_t)BOOTINFO_ADDR;

static void stage2_main(uint32_t entry_addr)
{
	(void)entry_addr;

	/* Reprogram the UART before the first diagnostic, not after: the first
	 * serial_puts would otherwise be racing whatever state the firmware left
	 * the transmitter in. */
	serial_init();

	boot_drive = stage1_boot_drive;

	LOG("running at 0x");
	serial_puthex(STAGE2_ADDR);
	serial_puts(", boot drive 0x");
	serial_puthex(boot_drive);
	serial_puts("\r\n");

	a20_enable();
	geom_load();
	e820_query();
	vbe_setup();
	load_kernel();
	boot_page_tables_init();

	boot_gdt_init();

	/* Hand off everything the kernel could not discover for itself. */
	bootinfo->magic = BOOTINFO_MAGIC;
	bootinfo->version = 1;
	bootinfo->flags = (uint32_t)(fb_enabled ? BOOT_FLAG_HAS_FRAMEBUFFER : 0u);
	bootinfo->kernel_phys_base = KERNEL_LANDING_ADDR;
	bootinfo->kernel_virt_base = KERNEL_VIRT_BASE;
	bootinfo->kernel_entry = kernel_entry_vaddr;
	bootinfo->e820_addr = E820_ADDR;
	bootinfo->e820_count = e820_count;
	bootinfo->fb = fb_info;
	bootinfo->rsdp_addr = 0;
	bootinfo->acpi_version = 0;
	bootinfo->boot_drive = boot_drive;
	bootinfo->cmdline[0] = '\0';

	LOG("entering long mode, jumping to kernel at ");
	serial_puthex(kernel_entry_vaddr);
	serial_puts("\r\n");

	/* Does not return: the kernel takes over the machine here. */
	stage2_enter_long_mode(boot_pml4_phys, kernel_entry_vaddr,
			       (uint64_t)(uintptr_t)bootinfo);

	fail("returned from long mode transition");
}

/* Exported so stage2_entry.S can call it after clearing BSS. */
void stage2_start(void);
void stage2_start(void)
{
	stage2_main(STAGE2_ADDR);
}
