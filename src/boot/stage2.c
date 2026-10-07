/*
 * stage2.c — the 32-bit loader.
 *
 * stage2_entry.S has already left real mode, cleared the BSS and called
 * stage2_start. Everything here runs in 32-bit protected mode with flat
 * segments and paging still off, which is what makes the absolute pointers
 * below (E820 and bootinfo, past the end of the landing zone) usable as written.
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
/* uint32_t, not uint64_t: this file is built -m32, so the i386 ABI pushes all
 * three on the stack. stage2_long.S reads them from the frame. Declaring them
 * as 64-bit only widens each stack slot to two words without changing where
 * they land. */
extern void stage2_enter_long_mode(uint32_t pml4_phys, uint32_t entry,
				   uint32_t bootinfo_phys);

/* Supplied by stage1 through the stack: the BIOS boot drive number. */
extern uint8_t stage1_boot_drive;

static uint8_t boot_drive = 0;

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
 * Four full bus cycles, produced by an I/O write to a port nothing is attached
 * to.
 *
 * Both legacy interrupt controllers need this and the 8042 needs it for the
 * same reason: the 8259 recognises a command only after four cycles, and the
 * keyboard controller latches its command on the trailing edge of the write
 * that carries it. Without the settling time, the byte is occasionally ignored
 * and the symptom is an interrupt -- or an A20 bit -- that changed state at
 * random. This used to sit in this file with no caller; the A20 sequence below
 * is what it is for.
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

extern uint32_t stack32_top;

static void serial_puts(const char *s)
{
	while (*s)
		serial_putc(*s++);
}

/*
 * 64-bit hex, as sixteen digits with no prefix and no leading-zero suppression.
 *
 * Computed arithmetically rather than a lookup table, because the loader's
 * diagnostics are the only way to see anything when the loader is broken, and
 * a table in .rodata could be zeroed, making every number print as NUL bytes
 * with no output to say why.
 */
static void serial_puthex(uint64_t v)
{
	for (int i = 60; i >= 0; i -= 4) {
		unsigned d = (unsigned)((v >> i) & 0xF);
		serial_putc(d < 10 ? (char)('0' + d) : (char)('a' + d - 10));
	}
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

/* ------------------------------------------------- BIOS call tripwires ------- */

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
 *
 * It hands back the firmware's EBX:EAX in EDX:EAX, which matters to every
 * caller that has to know which register a result came in -- see e820_call().
 */
extern uint64_t bios_call(uint32_t vector, uint32_t a, uint32_t b, uint32_t c,
			  uint32_t d, uint32_t si, uint32_t di, uint32_t es);

/* The name of the firmware service most recently entered. */
static const char *diag_site;

/* How many firmware round trips have been made. */
static uint32_t diag_n;

static inline void diag_tick(void)
{
	diag_n++;
}

/*
 * There is nowhere to return to. `fail` reports and halts rather than
 * returning, so every caller can treat it as noreturn and the compiler will
 * not keep values alive across it that are about to become meaningless.
 *
 * The two extra lines it prints are the loader's only record of how far it got.
 * Every firmware call in stage2 goes through a round trip that leaves real mode
 * and comes back, and the failure mode of that round trip is silence: the log
 * stops at the last line before the call and nothing says which service was
 * being asked for. Naming the last service entered, and how many of them there
 * have been, turns "the loader stopped" into a specific claim that can be
 * checked -- and it costs two words and one counter.
 */
static void fail(const char *what)
{
	serial_puts("[boot2] FATAL: ");
	serial_puts(what);
	serial_puts("\r\n");
	serial_puts("[boot2] last BIOS call: ");
	serial_puts(diag_site ? diag_site : "(none)");
	serial_puts(", call #");
	serial_putdec(diag_n);
	serial_puts("\r\n");
	stage2_halt();
}

/* ------------------------------------------------------------------ A20 ------ */

/*
 * The three ways to open the A20 gate, and how to find out whether any of them
 * worked.
 *
 * Nothing about a closed gate is a crash. The CPU keeps running, only address
 * line 20 is held low, and every access between 1 MiB and 1 MiB + 1 MiB folds
 * back down to the bottom of memory. Nothing faults, because every address
 * involved resolves to something: the kernel copy lands on top of the real-mode
 * IVT at physical zero, and the far jump afterwards lands in the middle of
 * interrupt-vector bytes. So the only way to keep this from being a silent
 * catastrophe is to ask, and the asking has to be a real test rather than "the
 * outb returned".
 */

/* 8042 status/command and data ports. */
#define PS2_STATUS 0x64
#define PS2_DATA   0x60
#define PS2_CMD_READ_OUTPUT_PORT  0xD0
#define PS2_CMD_WRITE_OUTPUT_PORT 0xD1
#define PS2_CMD_DISABLE_KEYBOARD  0xAD
#define PS2_CMD_ENABLE_KEYBOARD   0xAE

/* The fast A20 gate, system control port A. */
#define PS2_FAST_GATE 0x92

/* INT 15h proper. boot_layout.h only names the three services the rest of the
 * loader asks for, and AH=2401 is the fourth; 0x15 is BIOS_INT_E820's vector
 * too, but calling it by that name here would read as a copy-paste error. */
#define BIOS_INT_MISC 0x15u

/* 8042 status register bits, port 0x64. */
#define PS2_STATUS_OBF 0x01
#define PS2_STATUS_IBF 0x02

static const uint32_t ps2_timeout = 100000u;

/*
 * Wait for the 8042 input buffer to drain, which is the only condition under
 * which a command byte is accepted.
 *
 * Bounded, and bounded deliberately: a wedged 8042 is a machine with no
 * keyboard controller rather than a loader that will never return, and the
 * other two A20 methods still work on such a machine.
 */
static bool ps2_wait_input(void)
{
	for (uint32_t i = 0; i < ps2_timeout; i++)
		if (!(inb(PS2_STATUS) & PS2_STATUS_IBF))
			return true;
	return false;
}

/* Write one command byte and wait for the controller to take it. */
static bool ps2_command(uint8_t cmd)
{
	if (!ps2_wait_input())
		return false;
	outb(PS2_STATUS, cmd);
	/* The controller latches the command on the trailing edge, so a bus
	 * cycle has to pass before the input buffer empties. */
	io_wait();
	return ps2_wait_input();
}

/* Wait for a reply byte, and hand it back. False if none arrives. */
static bool ps2_read_data(uint8_t *out)
{
	for (uint32_t i = 0; i < ps2_timeout; i++) {
		if (inb(PS2_STATUS) & PS2_STATUS_OBF) {
			*out = inb(PS2_DATA);
			return true;
		}
	}
	return false;
}

static bool ps2_write_data(uint8_t val)
{
	if (!ps2_wait_input())
		return false;
	outb(PS2_DATA, val);
	io_wait();
	return ps2_wait_input();
}

/*
 * A byte at a fixed physical address, through inline asm rather than a
 * dereferenced pointer.
 *
 * GCC treats a constant like 0x500 as a null-pointer dereference, so the
 * access is written as the instruction it is.
 */
static inline uint8_t phys_read8(uint32_t addr)
{
	uint8_t v;

	__asm__ volatile("movb (%1), %0" : "=q"(v) : "r"(addr) : "memory");
	return v;
}

static inline void phys_write8(uint32_t addr, uint8_t val)
{
	__asm__ volatile("movb %0, (%1)" : : "q"(val), "r"(addr) : "memory");
}

#define A20_TEST_LO 0x00000500u
#define A20_TEST_HI 0x00100500u

/*
 * Is the gate actually open?
 *
 * 0x500 and 0x100500 alias while A20 is closed. Both are ordinary RAM the
 * loader owns nothing in, so the test is three writes and three reads with
 * no lasting effect. A machine that cannot answer it has no memory above
 * 1 MiB, which this loader could not use anyway.
 */
static bool a20_is_open(void)
{
	/* Zero the high byte first: the two tests below are only conclusive if
	 * the low write cannot be confused with data that was already there. */
	phys_write8(A20_TEST_HI, 0x00);
	phys_write8(A20_TEST_LO, 0x5A);
	if (phys_read8(A20_TEST_HI) == 0x5A)
		return false;	/* the low write showed up 1 MiB up: aliased */

	phys_write8(A20_TEST_HI, 0xA5);
	if (phys_read8(A20_TEST_LO) != 0x5A)
		return false;	/* the high write showed up 1 MiB down: aliased */

	return true;
}

/*
 * Method one: the keyboard controller's output port.
 *
 * Bit 0 is forced on (not merely preserved) because writing it clear resets
 * the CPU before the kernel exists. Everything else in the register is
 * passed through untouched. The keyboard is re-enabled on every path out,
 * so a refused A20 does not cost a working keyboard.
 */
static bool a20_enable_8042(void)
{
	uint8_t status = 0;
	uint8_t verify = 0;
	uint8_t want;
	bool ok;

	ps2_command(PS2_CMD_DISABLE_KEYBOARD);

	ok = ps2_command(PS2_CMD_READ_OUTPUT_PORT) && ps2_read_data(&status);

	/*
	 * A second half of the same test as the OBF poll: the reply has to look
	 * like an output port, and the one bit that says so is bit 0, because a
	 * genuine read of a running machine's output port always has the reset
	 * line inactive. No scancode byte has that property, so a controller
	 * that answered with a keyboard byte instead of the register -- or with
	 * nothing at all, in which case 0x60 returns the last byte read -- is
	 * caught here rather than written back to the register.
	 */
	if (ok && (status & 0x01u) == 0) {
		ok = false;
		LOG("  8042 output port read back as 0x");
		serial_puthex(status);
		serial_puts("; not a port value\r\n");
	}

	if (ok) {
		want = (uint8_t)(status | 0x02u);
		ok = ps2_command(PS2_CMD_WRITE_OUTPUT_PORT) &&
		     ps2_write_data(want);

		/* Read the register back rather than believing the write. The
		 * latch is shared with port 0x92, and which of the two doors a
		 * chipset actually wires to the A20 line is not knowable from
		 * out here -- so the 0x92 read below is a second witness. */
		if (ok)
			ok = ps2_command(PS2_CMD_READ_OUTPUT_PORT) &&
			     ps2_read_data(&verify) && (verify & 0x02u) != 0;
	}

	ps2_command(PS2_CMD_ENABLE_KEYBOARD);

	return ok && a20_is_open();
}

/*
 * Method two: the fast gate.
 *
 * Bit 0 here has opposite polarity to the 8042 output port: here 0 is normal
 * and 1 is reset, so this read-modify-write clears bit 0 and sets bit 1.
 * Copying the 8042's polarity here would reboot the machine instead of
 * enabling A20.
 */
static bool a20_enable_fast_gate(void)
{
	uint8_t v = inb(PS2_FAST_GATE);

	outb(PS2_FAST_GATE, (uint8_t)((v & (uint8_t)~0x01u) | 0x02u));

	return (inb(PS2_FAST_GATE) & 0x02u) != 0;
}

/*
 * Method three: INT 15h/AH=2401.
 *
 * bios_call hands back EBX:EAX and drops the carry flag, so the status the
 * firmware puts in AH cannot be read from out here. The call is therefore
 * only worth making as a last resort, and it is judged by the same alias test
 * as the other two. BX selects which gate the firmware should use: 0 is the
 * keyboard controller and 1 is port 92h, and which of the two a given BIOS
 * honours is not knowable from here, so both are tried.
 */
static bool a20_enable_int15(void)
{
	for (uint32_t which = 0; which < 2u; which++) {
		diag_site = "a20-int15";
		diag_tick();

		(void)bios_call(BIOS_INT_MISC, 0x2401u, which, 0u, 0u, 0, 0, 0);

		if (a20_is_open())
			return true;
	}

	return false;
}

static void a20_enable(void)
{
	if (a20_enable_8042()) {
		LOG("A20 enabled via 8042\r\n");
		return;
	}

	if (a20_enable_fast_gate()) {
		LOG("A20 enabled via port 0x92 fast gate\r\n");
		return;
	}

	if (a20_enable_int15()) {
		LOG("A20 enabled via INT 15h/AH=2401\r\n");
		return;
	}

	/*
	 * Last word goes to the hardware rather than to the return values: some
	 * chipsets only answer the alias test once the gate is open through
	 * whatever mechanism the firmware used before stage2 ran.
	 */
	if (a20_is_open() || (inb(PS2_FAST_GATE) & 0x02u)) {
		LOG("A20 already enabled by firmware\r\n");
		return;
	}

	/*
	 * Failing here is the point. A machine whose A20 gate will not open
	 * cannot run this loader, and the alternative -- carrying on -- produces
	 * a kernel image written over the interrupt vector table and a far jump
	 * into it, with every access in between landing somewhere valid and
	 * nothing reporting anything at all.
	 */
	fail("cannot enable the A20 address line");
}

/* ------------------------------------------------------- BIOS trampoline ----- */

/* bios_call() itself is declared above, with the tripwires it is counted by.
 * INT 13h/AH=08h answers the geometry in ECX and EDX rather than in a buffer,
 * so the trampoline parks them here rather than returning them. */
extern uint32_t bios_ret_ecx;
extern uint32_t bios_ret_edx;

/* ELF program header types. Only PT_LOAD carries anything to copy. */
#define PT_LOAD 1

/* Handed to stage2_long.S, which reloads them after EFER.LME is set. */
/* The six bytes LGDT reads: 16-bit limit, then 32-bit base. Filled in by
 * boot_gdt_init once the GDT exists, because LGDT cannot take them from a
 * register and the assembler can only emit an address for a label. */
struct gdt_pointer {
	uint16_t limit;
	uint32_t base;
} __attribute__((packed));

extern struct gdt_pointer gdtr64;


/* --------------------------------------------------------- CHS conversion --- */

/*
 * A linear address as the 16-bit segment:offset pair the real-mode services
 * take. Every one of them dereferences through a segment register, and a buffer
 * above 64 KiB is reachable that way only if the segment carries the high part:
 * 0xF0000 is 0xF00:0x0000, not 0x000F:0x0000.
 */
static void seg16(uint32_t addr, uint16_t *seg, uint16_t *off)
{
	*seg = (uint16_t)(addr >> 4);
	*off = (uint16_t)(addr & 0x0F);
}

/* ------------------------------------------------------------ geometry ------ */

static uint32_t geom_spt;
static uint32_t geom_heads;

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

	/*
	 * The two fields do not use the same base, which is the whole difficulty.
	 *
	 * CL bits 5-0 hold the sector count directly: SeaBIOS reports 0x3F for
	 * the 63 sectors per track of a plain IDE disk. Add one here and the
	 * loader believes the disk has 64, and every LBA past sector 63 of a
	 * track is converted to a CHS address one sector too high -- which reads
	 * the wrong sector, silently, until the ELF header comes back as noise.
	 *
	 * DH holds the highest head number, not a count: 0x0F is fifteen heads
	 * numbered 0..15, so the count is one more. Leave it alone and every
	 * address past head 14 names a head the drive does not have. The
	 * firmware does not fail that cleanly either: the request simply never
	 * comes back, so the loader stops inside the first multi-sector read
	 * with no status byte and nothing on the serial port.
	 */
	geom_spt = (uint32_t)(bios_ret_ecx & 0x3F);
	geom_heads = (uint32_t)((bios_ret_edx >> 8) & 0xFF) + 1u;

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
		 * How many sectors one firmware call may usefully move: bounded by
		 * the rest of this track, because a CHS read may not cross a track
		 * boundary; by what the caller asked for; and by the bounce window.
		 *
		 * The window bound is the one that matters. The track bound alone
		 * allows 63 sectors, which is 32 KiB, and the firmware writes all
		 * of it to whatever buffer it was handed -- here 4 KiB at 0x10000.
		 * It does not know the buffer is smaller and does not care: the
		 * transfer runs straight over the disk address packet at 0x11000
		 * and the VBE scratch block above it, and reports success. Nothing
		 * faults, because every address involved is valid; the next
		 * sector's parameters are simply gone by the time they are read.
		 *
		 * AH is the function and AL the sector count, so the count goes in
		 * the low byte: (02h << 8) | batch. Asking for 0201h | batch << 8
		 * instead requests function batch+1 with a one-sector count; the
		 * firmware declines a function it does not implement, reports it in
		 * AH, and the bounce window keeps whatever was already in it.
		 */
		uint32_t batch = geom_spt - (cur_lba % geom_spt);

		if (batch > count)
			batch = count;
		if (batch > BOUNCE_BYTES / 512u)
			batch = BOUNCE_BYTES / 512u;

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
		uint32_t status = (uint32_t)bios_call(BIOS_INT_DISK,
						      (0x02u << 8) | batch, boff,
						      ((cylinder >> 2) << 6) | sector,
						      (head << 8) | boot_drive,
						      0, 0, bseg);

		if ((status & 0xFF00u) != 0)
			return false;

		/* AL comes back as the number of sectors actually moved, and a short
		 * transfer means the image is truncated -- which must not be papered
		 * over by using whatever happened to be in the bounce window. */
		if ((status & 0xFFu) != batch)
			return false;

		cur_lba += batch;
		count -= batch;
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
 *
 * Return value: the index of the next entry, or 0 for "this was the last one".
 * The two conventions have to be made to agree on that, because the caller
 * has one loop and one stop test.
 */
static uint32_t e820_call(uint32_t entry, uint32_t index)
{
	uint16_t es, di;
	uint64_t r;

	/* ES:DI has to name the entry being asked for, not the first one. A
	 * single pair computed before the loop makes every call after the first
	 * re-read entry 0, so the firmware writes the same answer every time and
	 * the map comes back as one real entry followed by the caller's poison. */
	seg16((uint32_t)(uintptr_t)(E820_ADDR +
				    entry * sizeof(struct e820_entry)), &es, &di);

	diag_site = "e820";
	diag_tick();

	/* SeaBIOS / Ralf Brown form: signature in DX:BP, index in BX, buffer
	 * at ES:DI.
	 *
	 * The signature comes back in EAX and the next index in EBX, and
	 * bios_call returns the firmware's EBX:EAX -- EBX in the *high* half of
	 * the 64-bit result, EAX in the low one. Reading the signature out of the
	 * low half and the index out of the high half is therefore not a choice
	 * but a consequence of the ABI, and the reverse of it cannot match. */
	r = bios_call(BIOS_INT_E820, 0xE820u, index, 24u,
		      0x534D4150u, 0, di, es);

	if ((uint32_t)r == 0x534D4150u)
		return (uint32_t)(r >> 32);	/* EBX: next index, 0 on last */

	/* Specification form: signature in EBX -- the high half again -- and the
	 * buffer named by a linear address in EDX, so the map above 64 KiB is
	 * reachable at all.
	 *
	 * There is no index in this convention: the firmware always fills the
	 * buffer it is handed. What it does answer is "is there another entry",
	 * in AX bit 19, and the bit is *set* when one follows. Reading it the
	 * other way round ends the map after the first entry, which is
	 * indistinguishable from a machine with 640 KiB of memory until the
	 * allocator believes it. So the index the loop wants is manufactured
	 * from `entry` here rather than read from anywhere. */
	r = bios_call(BIOS_INT_E820, 0xE820u, 0x534D4150u, 24u,
		      E820_ADDR + entry * sizeof(struct e820_entry), 0, 0, 0);

	if ((uint32_t)(r >> 32) == 0x534D4150u)
		return ((uint32_t)r & (1u << 19)) ? entry + 1u : 0u;

	/* Neither convention answered. The caller's poison is still in the
	 * entry, which is how the loop below finds out. */
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

		uint32_t next = e820_call(e820_count, e820_count);

		uint64_t base = *(const uint64_t *)(const void *)entry;
		uint64_t length = *(const uint64_t *)(const void *)(entry + 8);
		uint32_t type = *(const uint32_t *)(const void *)(entry + 16);

		/*
		 * The poison is still there, which means neither convention
		 * answered and the firmware refused the call. This is the check
		 * that decides between a machine with 640 KiB of memory and a
		 * loader that asked the wrong question: keeping the poisoned
		 * entry would hand the allocator a map of one 0xA5A5A5A5A5A5A5A5
		 * byte region and no error anywhere, and a count of one is not a
		 * count of zero, so the failure at the bottom of this function
		 * would not fire either.
		 */
		if (base == 0xA5A5A5A5A5A5A5A5ull) {
			if (e820_count == 0)
				fail("E820 map refused by the firmware");
			LOG("  entry refused by the firmware at index ");
			serial_putdec(e820_count);
			serial_puts("\r\n");
			break;
		}

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

/* How many framebuffer modes vbe_setup() keeps as candidates for the mode set.
 * A standard VGA mode list is around two hundred entries, of which the 32bpp
 * ones number a handful; this is comfortably more than any of them. */
#define VBE_MAX_CANDIDATES 12

static bool vbe_get_mode_info(uint16_t mode)
{
	uint16_t es;
	uint16_t di;

	seg16((uint32_t)(uintptr_t)vbe_mode, &es, &di);

	diag_site = "vbeinfo";
	diag_tick();

	uint32_t ret = (uint32_t)bios_call(BIOS_INT_VIDEO, 0x4F01u, 0,
					   (uint32_t)mode, 0, 0, di, es);

	return ret == 0x004Fu;
}

static bool vbe_set_mode(uint16_t mode)
{
	uint16_t es;
	uint16_t di;

	seg16(0, &es, &di);

	diag_site = "vbeset";
	diag_tick();

	uint32_t ret = (uint32_t)bios_call(BIOS_INT_VIDEO, 0x4F02u,
					   0x4000u | (uint32_t)(mode & 0xFFu),
					   0, 0, 0, di, es);

	return ret == 0x004Fu;
}

/*
 * Pull a framebuffer description out of the mode info block vbe_get_mode_info()
 * has just filled in.
 *
 * The channel shifts are the interesting part. A VBE mode info block carries
 * six bytes about the pixel layout, and they are *positions* and *sizes*, in
 * that order of confusion:
 *
 *   +0x31 RedMaskSize     +0x34 RedMaskPosition
 *   +0x32 GreenMaskSize   +0x35 GreenMaskPosition
 *   +0x33 BlueMaskSize    +0x36 BlueMaskPosition
 *
 * The kernel composes a pixel as (r << red_shift) | (g << green_shift) |
 * (b << blue_shift), so it needs the positions and nothing else. Taking the
 * sizes instead is not a small error: for a plain 32bpp mode, where all three
 * sizes are 8, it produces red=2, green=0, blue=1 rather than 16/8/0, and
 * every channel lands in the wrong bits of the wrong byte. Nothing faults, the
 * framebuffer is mapped, and the text is a plausible smear of colour -- which
 * is why the numbers are worth checking against the mode list by hand once.
 *
 * The positions themselves live in bits 7:3 of their bytes; bits 2:0 are
 * reserved and are dropped by the shift. What is *not* done here is converting
 * to byte offsets: the field is a bit shift, and the comment in boot.h that
 * says otherwise describes a packing this loader does not produce.
 */
static void vbe_extract_fb(void)
{
	fb_info.address = (uint32_t)(vbe_mode[0x28] | ((uint32_t)vbe_mode[0x29] << 8) |
				     ((uint32_t)vbe_mode[0x2A] << 16) |
				     ((uint32_t)vbe_mode[0x2B] << 24));
	fb_info.pitch = (uint32_t)(vbe_mode[0x10] | ((uint32_t)vbe_mode[0x11] << 8));
	fb_info.width = (uint32_t)(vbe_mode[0x12] | ((uint32_t)vbe_mode[0x13] << 8));
	fb_info.height = (uint32_t)(vbe_mode[0x14] | ((uint32_t)vbe_mode[0x15] << 8));
	fb_info.bpp = vbe_mode[0x19];
	fb_info.red_shift = (uint8_t)(vbe_mode[0x34] >> 3);
	fb_info.green_shift = (uint8_t)(vbe_mode[0x35] >> 3);
	fb_info.blue_shift = (uint8_t)(vbe_mode[0x36] >> 3);
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

	/* The mode list is a far pointer into the firmware's own memory. */

	/*
	 * The modes worth trying, kept rather than decided on the spot.
	 *
	 * AH=4F02h names the mode in eight bits, so a mode above 0xFF cannot be
	 * set through it, and a firmware's largest listed mode is routinely one of
	 * those. The walk below therefore records several candidates and the tail
	 * of this function tries them best-first, rather than committing to the
	 * biggest one and reporting that the machine has no framebuffer when the
	 * firmware declines a mode this interface cannot express.
	 */
	struct vbe_candidate {
		uint16_t mode;
		uint32_t score;
	};
	struct vbe_candidate cand[VBE_MAX_CANDIDATES];
	unsigned ncand = 0;

	seg16((uint32_t)(uintptr_t)ctrl, &es, &di);

	LOG("querying VESA BIOS...\r\n");

	diag_site = "vbectrl";
	diag_tick();

	uint32_t ret = (uint32_t)bios_call(BIOS_INT_VIDEO, 0x4F00u, 0, 0, 0,
					   0, di, es);

	if (ret != 0x004Fu) {
		LOG("  no VESA BIOS; framebuffer unavailable\r\n");
		return;
	}

	serial_puts("  VBE ");
	serial_puthex((uint64_t)(ctrl[2] | ((uint16_t)ctrl[3] << 8)));
	serial_puts("\r\n");

	best_mode = 0;

	/*
	 * Walk the mode list.
	 *
	 * VideoModePtr is the fourth field of the controller block, and the two
	 * words it occupies are stored offset first: +0x0E is the offset and
	 * +0x10 is the segment. The spec draws a FarPtr as segment:offset and
	 * that reading puts them the other way round, which on this firmware
	 * produced segment 0x0022 offset 0x2110 and so a linear address of
	 * 0x2330 -- inside the BIOS data area, where every word reads as zero.
	 * The walk then asks 4F01 about mode 0 two hundred and fifty-six times,
	 * the BIOS answers success every time, and the loader reports a
	 * firmware that has no modes at all.
	 *
	 * The list behind that pointer is an array of 16-bit mode numbers closed
	 * by 0xFFFF, not the array of four-byte far pointers the specification
	 * describes. Reading four bytes per entry and taking the first word
	 * examines every other mode and steps over the terminator, so the loop
	 * below is bounded by an explicit count instead of relying on the end
	 * marker being reachable.
	 */
	uint16_t list_off = (uint16_t)(ctrl[0x0E] | ((uint16_t)ctrl[0x0F] << 8));
	uint16_t list_seg = (uint16_t)(ctrl[0x10] | ((uint16_t)ctrl[0x11] << 8));

	for (unsigned i = 0; i < 1024u; i++) {
		uint16_t m;

		uint32_t linear = ((uint32_t)list_seg << 4) + list_off + i * 2u;
		uint8_t *p = (uint8_t *)(uintptr_t)linear;

		m = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));

		if (m == 0xFFFFu)
			break;

		if (!vbe_get_mode_info(m))
			continue;

		/*
		 * Mode info attributes word. Bit 0 is "mode is supported", bit 4 is
		 * graphics, bit 7 is "uses a linear framebuffer"; bits 8 and up are
		 * reserved. Testing bit 8 for the framebuffer -- which is what this
		 * did -- is a test that no conforming firmware can ever satisfy,
		 * because a reserved bit reads as zero, and it is the second half of
		 * why the loader never found a mode even when the mode list itself
		 * was read correctly.
		 */
		uint16_t attrs = (uint16_t)(vbe_mode[0] | ((uint16_t)vbe_mode[1] << 8));

		if (!(attrs & 0x0081u))
			continue;
		if (!(attrs & 0x0080u))
			continue;

		uint32_t width = (uint32_t)(vbe_mode[0x12] | ((uint32_t)vbe_mode[0x13] << 8));
		uint32_t height = (uint32_t)(vbe_mode[0x14] | ((uint32_t)vbe_mode[0x15] << 8));
		uint32_t phys = (uint32_t)(vbe_mode[0x28] | ((uint32_t)vbe_mode[0x29] << 8) |
					   ((uint32_t)vbe_mode[0x2A] << 16) |
					   ((uint32_t)vbe_mode[0x2B] << 24));

		if (phys == 0)
			continue;

		/*
		 * Only 32bpp. The kernel's framebuffer renderer composes a pixel by
		 * shifting the three channels into the mask positions the mode info
		 * block reports, which is exactly a 32bpp description; its 24bpp path
		 * writes the channels in R,G,B order, and a 24bpp VBE mode is
		 * ordinarily BGR, so accepting one would produce a legible screen in
		 * the wrong colours. A mode list offers 8/15/16/24bpp alternatives for
		 * every resolution that has a 32bpp form, so this costs nothing here.
		 */
		if (vbe_mode[0x19] != 32)
			continue;

		uint32_t score = width * height;

		if (ncand == VBE_MAX_CANDIDATES) {
			unsigned worst = 0;

			for (unsigned k = 1; k < ncand; k++)
				if (cand[k].score < cand[worst].score)
					worst = k;
			if (score <= cand[worst].score)
				continue;
			cand[worst].mode = m;
			cand[worst].score = score;
			continue;
		}

		cand[ncand].mode = m;
		cand[ncand].score = score;
		ncand++;
	}

	if (ncand == 0) {
		LOG("  no linear framebuffer mode found\r\n");
		return;
	}

	/* Order the candidates best-first. */
	for (unsigned i = 1; i < ncand; i++) {
		struct vbe_candidate keep = cand[i];
		unsigned j = i;

		while (j > 0 && cand[j - 1].score < keep.score) {
			cand[j] = cand[j - 1];
			j--;
		}
		cand[j] = keep;
	}

	/*
	 * Try them in order until one is accepted.
	 *
	 * This is not defensive padding. AH=4F02h carries the mode number in the
	 * low byte of BX alongside the bit-14 request for a linear framebuffer,
	 * so a mode numbered above 0xFF cannot be named through this interface at
	 * all -- the largest mode this firmware lists, 0x199, is exactly such a
	 * mode, and asking for it answers 0x014F. Choosing the single largest
	 * mode and setting it therefore fails on a machine whose 32bpp modes are
	 * perfectly settable. Walking down the list costs one extra BIOS round
	 * trip per rejected mode and gets a framebuffer out of hardware that the
	 * one-shot version reported as having none.
	 */
	for (unsigned i = 0; i < ncand; i++) {
		serial_puts("  trying mode 0x");
		serial_puthex((uint64_t)cand[i].mode);
		serial_puts("\r\n");

		if (vbe_set_mode(cand[i].mode)) {
			best_mode = cand[i].mode;
			break;
		}
	}

	if (best_mode == 0) {
		LOG("  every framebuffer mode refused; framebuffer unavailable\r\n");
		return;
	}

	/* The mode set can invalidate the mode info block, so the values the
	 * kernel needs are read back rather than trusted from before -- and this
	 * time the block is actually used. Fetching it and throwing the answer
	 * away left the kernel with a description of the mode as it was before
	 * the set, which is a different address and a different pitch on any
	 * firmware that reprograms it. */
	if (!vbe_get_mode_info(best_mode)) {
		LOG("  mode info lost after mode set\r\n");
		return;
	}

	vbe_extract_fb();

	if (fb_info.address == 0) {
		LOG("  no framebuffer address after mode set\r\n");
		return;
	}

	serial_puts("  fb ");
	serial_puthex((uint64_t)fb_info.address);
	serial_puts(" ");
	serial_putdec(fb_info.width);
	serial_puts("x");
	serial_putdec(fb_info.height);
	serial_puts(" @");
	serial_putdec(fb_info.bpp);
	serial_puts("bpp, shifts r");
	serial_putdec(fb_info.red_shift);
	serial_puts(" g");
	serial_putdec(fb_info.green_shift);
	serial_puts(" b");
	serial_putdec(fb_info.blue_shift);
	serial_puts("\r\n");

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
		uint32_t sectors;
		uint32_t chunk;

		/*
		 * One firmware call per sector, for now.
		 *
		 * The bounce window is 4 KiB so eight sectors could be moved per call,
		 * and the arithmetic for it is below and has been checked by hand. But
		 * no caller has ever exercised a multi-sector AH=02h on this firmware:
		 * every other read in the loader asks for one sector, and the code that
		 * would have asked for eight was dead until this loop started issuing
		 * reads of its own. A 540-sector kernel read one sector at a time is a
		 * few hundred INT 13h round trips and finishes in well under a second,
		 * which is a much better trade than a hang with a silent console. The
		 * batching can come back with a test behind it.
		 */
		sectors = 1;

		/*
		 * How much of the window this call actually covers. The window was
		 * filled from `sector`, so the byte at offset `in_sector` is the first
		 * byte wanted and the last byte available is `sectors * 512 - 1`.
		 *
		 * Deriving this from the caller's `len` as well matters: without that
		 * clamp the copy runs past the end of the segment on the last call.
		 */
		chunk = sectors * 512u - in_sector;
		if ((uint64_t)chunk > len)
			chunk = (uint32_t)len;

		if (!bios_read_bounce((uint64_t)KERNEL_LBA + sector, sectors))
			fail("kernel payload read failed");

		uint8_t *src = (uint8_t *)(uintptr_t)BOUNCE_ADDR + in_sector;

		/* Word-wise where both ends allow it. The source is a firmware
		 * transfer window and the destination is the landing zone; both are
		 * 4 KiB-aligned in practice, but neither is guaranteed, so the
		 * alignment is checked rather than assumed. */
		if (chunk >= 4u &&
		    (((uintptr_t)out | (uintptr_t)src) & 3u) == 0u) {
			uint32_t *ow = (uint32_t *)(void *)out;
			const uint32_t *sw = (const uint32_t *)(const void *)src;
			uint32_t words = chunk / 4u;

			for (uint32_t i = 0; i < words; i++)
				ow[i] = sw[i];
			for (uint32_t i = words * 4u; i < chunk; i++)
				out[i] = src[i];
		} else {
			for (uint32_t i = 0; i < chunk; i++)
				out[i] = src[i];
		}

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

	if (!bios_read_bounce(KERNEL_LBA, 1))
		fail("cannot read kernel ELF header");

	/* The sector is in the bounce window, not the landing zone. */
	{
		uint8_t *hdr_dst = (uint8_t *)(uintptr_t)KERNEL_LANDING_ADDR;
		const uint8_t *hdr_src = (const uint8_t *)(uintptr_t)BOUNCE_ADDR;

		for (uint32_t i = 0; i < 512u; i++)
			hdr_dst[i] = hdr_src[i];
	}

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
		uint32_t sector = (uint32_t)(phdr_off / 512);
		uint32_t in_sector = (uint32_t)(phdr_off % 512);
		uint8_t *dst_ph = (uint8_t *)(uintptr_t)PHDR_SCRATCH_ADDR;

		if (in_sector + sizeof(phdr) > 512) {
			if (!bios_read_bounce((uint64_t)KERNEL_LBA + sector, 1))
				fail("cannot read kernel program headers");
			for (uint32_t j = 0; j < 512 - in_sector; j++)
				dst_ph[j] = *(const volatile uint8_t *)
					(uintptr_t)(BOUNCE_ADDR + in_sector + j);
			if (!bios_read_bounce((uint64_t)KERNEL_LBA + sector + 1, 1))
				fail("cannot read kernel program headers");
			for (uint32_t j = 0; j < sizeof(phdr) - (512 - in_sector); j++)
				dst_ph[512 - in_sector + j] =
					*(const volatile uint8_t *)
						(uintptr_t)(BOUNCE_ADDR + j);
		} else {
			if (!bios_read_bounce((uint64_t)KERNEL_LBA + sector, 1))
				fail("cannot read kernel program headers");
			for (uint32_t j = 0; j < sizeof(phdr); j++)
				dst_ph[j] = *(const volatile uint8_t *)
					(uintptr_t)(BOUNCE_ADDR + in_sector + j);
		}

		phdr_off += eh->e_phentsize;

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

		/* The check has to be on p_memsz, not p_filesz. The difference
		 * between them is .bss, which the kernel owns from physical zero
		 * the moment it starts running, so a window sized by file size
		 * puts stage2's own scratch -- page tables, E820 map, bootinfo
		 * -- inside kernel memory and hands the kernel a page table it
		 * has already been overwritten into. The copy succeeds and the
		 * failure surfaces later, somewhere that does not explain it. */
		if (phdr.p_offset + phdr.p_memsz > KERNEL_MAX_BYTES)
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

			/* 32 bits at a time, and the tail byte-wise. This kernel's
			 * .bss is 1.5 MiB, and a byte loop over that is slow enough
			 * to look like a hang under QEMU -- which is exactly what it
			 * was mistaken for before the read batching above. */
			uint64_t words = bss_len / 4u;
			uint32_t *pw = (uint32_t *)(void *)p;

			for (uint64_t n = 0; n < words; n++)
				pw[n] = 0;
			for (uint64_t n = words * 4u; n < bss_len; n++)
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
 *   0x2D0000  PML4
 *   0x2D1000  PDPT (identity: entries 0-3; higher-half: entries 508-511)
 *   0x2D2000  PD #0  (0 - 1 GiB)
 * 0x2D3000  PD #1  (1 - 2 GiB)
 *   0x2D4000  PD #2  (2 - 3 GiB)
 *   0x2D5000  PD #3  (3 - 4 GiB)
 *
 * BOOT_PT_BASE is BOOT_PT_ADDR from boot_layout.h rather than a second copy of
 * the number: this window used to sit at 0x70000, inside what turned out to be
 * the kernel's own .bss, and the two definitions disagreed exactly where it
 * mattered.
 */
#define BOOT_PML4_OFF  0x0000
#define BOOT_PDPT_OFF  0x1000
#define BOOT_PD0_OFF   0x2000
#define BOOT_PDH_OFF   0x6000
/* Page table for the first 2 MiB of the kernel window. See boot_page_tables_init:
 * the landing zone is 1 MiB-aligned, which no 2 MiB page can describe. */
#define BOOT_PTP_OFF   0xA000
#define BOOT_PT_BASE   BOOT_PT_ADDR

static void boot_page_tables_init(void)
{
	uint64_t base = BOOT_PT_BASE;
	uint64_t *pml4 = (uint64_t *)(uintptr_t)(base + BOOT_PML4_OFF);
	uint64_t *pdpt = (uint64_t *)(uintptr_t)(base + BOOT_PDPT_OFF);
	uint64_t *pd0 = (uint64_t *)(uintptr_t)(base + BOOT_PD0_OFF);
	uint64_t *pd_high = (uint64_t *)(uintptr_t)(base + BOOT_PDH_OFF);
	uint64_t *pt_high = (uint64_t *)(uintptr_t)(base + BOOT_PTP_OFF);

	LOG("building bootstrap page tables...\r\n");

	/* Zero the whole window. Paging is about to be enabled with CR3
	 * pointing here, and a single non-zero word in a table that is supposed
	 * to be empty is a wild mapping rather than a fault. */
	for (uint64_t i = 0; i < BOOT_PT_ZERO_BYTES / 8; i++)
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
	 * The higher-half window, which is not an alias of the identity map.
	 *
	 * The kernel is linked at 0xffffffff80000000 but stage2 cannot put it
	 * there: INT 13h addresses below 1 MiB, so the image is copied into the
	 * landing zone first. Aliasing the higher half onto PDs 0-3 therefore maps
	 * virtual 0xffffffff80000000 to physical zero, where the first instruction
	 * is real-mode IVT, and every access lands on memory that happens to be
	 * mapped rather than on the image. The kernel sets up a stack from
	 * .bss, validates a bootinfo pointer, and writes to its own globals --
	 * none of which fault, because every one of those addresses resolves.
	 *
	 * Nothing is printed. There is no fault to report and no failure to
	 * notice; the kernel just carries on with a copy of itself at the wrong
	 * address and the loader looks as though it stopped at the far jump.
	 *
	 * So the higher half gets its own page directory whose entries start at
	 * the landing zone: virtual 0xffffffff80000000 is physical
	 * KERNEL_LANDING_ADDR, and the kernel's own offsets from its link address
	 * land on the bytes that were actually copied there.
	 */
	/*
	 * The first 2 MiB of the window needs 4 KiB pages, and this is the whole
	 * reason the kernel could not be started.
	 *
	 * KERNEL_LANDING_ADDR is 0x100000, which is 1 MiB-aligned but not
	 * 2 MiB-aligned. A 2 MiB page descriptor can only name a 2 MiB-aligned
	 * base, because the descriptor's physical field is bits 51:21 with the
	 * low 21 bits reserved for the in-page offset. Writing the landing zone
	 * into that field therefore does not describe the landing zone: the bit
	 * that carries 0x100000 is bit 20, which is part of the offset, so the
	 * CPU reads the base as zero.
	 *
	 * The entry still looks right. It is present, writable and marked as a
	 * large page, and reading it back gives the value that was written, so
	 * every check in the loader passes. What it actually maps is physical
	 * zero -- the real-mode interrupt vector table.
	 *
	 * Nothing faults, because every address in the window resolves to
	 * something. The kernel starts executing at the vector table, which is
	 * real-mode IVT entries rather than instructions, and the CPU wanders
	 * until it reaches an address it cannot fetch. By then the loader has
	 * already printed that it is entering long mode, so the console shows a
	 * successful transition followed by silence.
	 *
	 * The landing zone cannot simply be moved up to 2 MiB either: 0x100000
	 * is where the image goes because that is the first megabyte the firmware
	 * will read into without a bounce buffer, and the kernel's own .bss
	 * extends past 2 MiB. So the first 2 MiB of the window is described with
	 * 4 KiB pages instead, which can name any physical address. One page
	 * table covers all 512 of them, and it reaches exactly as far as the
	 * image plus its .bss.
	 */
	for (unsigned j = 0; j < 512; j++)
		pt_high[j] = (KERNEL_LANDING_ADDR + ((uint64_t)j << 12)) |
			     PT_PRESENT | PT_WRITE | PT_USER;

	pd_high[0] = (base + BOOT_PTP_OFF) |
		     PT_PRESENT | PT_WRITE | PT_USER;

	/* Everything above the first 2 MiB is 2 MiB-aligned relative to the
	 * landing zone, so those entries can be large pages after all. */
	for (unsigned i = 1; i < 4; i++) {
		for (unsigned j = 0; j < 512; j++)
			pd_high[i * 512 + j] =
				(KERNEL_LANDING_ADDR + (((uint64_t)i * 512 + j) << 21)) |
				PT_PRESENT | PT_WRITE | PT_USER | PT_PS;
	}

	/*
	 * Higher half: the indices the kernel's own link address actually lands on.
	 *
	 * For 0xFFFFFFFF80000000 the walk splits as PML4 511, PDPT 510, PD 0 --
	 * not "PML4 index 256" and not "PDPT 508". Those two numbers are the ones
	 * that come to mind for the higher half, and both are wrong:
	 *
	 *   - PML4 index 256 is 0xFFFFFF8000000000, the first address of the
	 *     higher canonical half -- 512 GiB below where the kernel is linked.
	 *   - PDPT index 508 begins the 512 GiB region that 0xFFFFFFFF80000000
	 *     sits in, but the kernel's address starts the *1 TiB* region, so it
	 *     is PDPT index 510, not 508.
	 *
	 * Filling those two wrong indices and leaving 511/510 zero fails quietly.
	 * Every index that was written resolves to something valid, so nothing in
	 * the tables looks malformed and no check trips. The first high-half
	 * access walks to pml4[511], reads zero, and raises a page fault -- long
	 * after the loader has announced the jump. The fault is delivered through
	 * the bootstrap IDT, and because paging is already on, a fault in the
	 * handler becomes a triple fault, which from the outside looks exactly
	 * like QEMU exiting without a word.
	 *
	 * PDPT 510 and 511 both reach the high-half directories, so the vmalloc
	 * range above the image resolves as well.
	 */
	pdpt[510] = (base + BOOT_PDH_OFF) |
			PT_PRESENT | PT_WRITE | PT_USER;
	pdpt[511] = (base + BOOT_PDH_OFF + 0x1000) |
			PT_PRESENT | PT_WRITE | PT_USER;

	pml4[0] = (base + BOOT_PDPT_OFF) | PT_PRESENT | PT_WRITE | PT_USER;
	/* PML4 index 511 is what 0xFFFFFFFF80000000 resolves through. */
	pml4[511] = (base + BOOT_PDPT_OFF) | PT_PRESENT | PT_WRITE | PT_USER;

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
	 *
	 * The array is 8 entries and only 5 are filled. The size is what the
	 * limit is derived from, so the slack is deliberate: it keeps the limit a
	 * whole number of descriptors and gives the kernel room to add segments
	 * here without a second trip through the assembly.
	 */
	gdt[0] = 0;
	gdt[1] = 0x00AF9A000000FFFFULL;	/* 0x08 64-bit code, base 0, 4G */
	gdt[2] = 0x00CF93000000FFFFULL;	/* 0x10 data, base 0, 4G */
	gdt[3] = 0x00AF9A000000FFFFULL;	/* 0x18 64-bit user code, DPL 3 */
	gdt[4] = 0x00CFF3000000FFFFULL;	/* 0x20 user data, DPL 3 */

	/*
	 * The limit comes from the array, not from a constant in the assembly.
	 *
	 * gdtr64's limit was written as 0x17 -- three descriptors, null/code/data --
	 * while this function has been building five for some time: null, 64-bit
	 * code, data, and a user code/data pair at DPL 3. A limit that describes
	 * fewer descriptors than the table holds is not a conservative choice,
	 * it is a live fault waiting for the first selector past the end: the
	 * far jump into 0x08 works, the kernel loads and starts using its user
	 * selectors, and the first one past the limit raises #GP with the IDT
	 * still pointing at stage2's reporter. Nothing is printed, because the
	 * reporter itself needs a segment selector to return.
	 *
	 * Deriving both halves from the same array makes the two impossible to
	 * disagree about. The unused tail entries are zero, which reads as a
	 * null descriptor and faults cleanly on use rather than aliasing
	 * something real.
	 */
	gdtr64.limit = (uint16_t)(sizeof(gdt) - 1);
	gdtr64.base = (uint32_t)(uintptr_t)gdt;

	/* Last chance to install the 64-bit reporter: after this the far jump
	 * makes every exception a long-mode delivery. */

	LOG("  GDT at 0x");
	serial_puthex(gdtr64.base);
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

	/*
	 * Mask both PICs before the first firmware call, and leave them masked.
	 *
	 * The 8259s come out of the BIOS identity-mapped with IRQ0 (the PIT)
	 * *unmasked* -- IMR 0x21 reads 0xFE, one bit clear, and that bit is
	 * IRQ0. Nothing is remapped until the kernel does it in pic_remap(), so
	 * for the whole of stage2 a timer tick is vector 0x08.
	 *
	 * That vector is fatal here, and the reason is the trampoline rather than
	 * the interrupt itself. The BIOS round trip runs in real mode with PE
	 * clear but stage2's own IDT still installed (IDTR = 0xB178), and an
	 * interrupt taken in real mode with a non-zero IDTR base is dispatched
	 * through that IDT rather than through the real-mode IVT at physical
	 * zero. Vector 8 therefore lands on stage2's gate at 0xB047, which is
	 * the *32-bit* reporter exc_stub.
	 *
	 * exc_stub is 32-bit code and the CPU is now in real mode decoding 16-bit
	 * instructions, so it executes as garbage: `50 53 51 52 56 57` becomes
	 * six 16-bit pushes, `mov 0x18(%esp),%eax` becomes an addressing mode
	 * that reads a different address, and the whole reporter -- including
	 * its COM1 writes -- produces nothing legible. It then walks into
	 * exc_halt64's `cli; hlt` and stops there for good.
	 *
	 * The symptom is that the log stops after an arbitrary `LOAD vaddr` line
	 * with the loader apparently still running, because the PIT fires at
	 * 18.2 Hz and the copy loop makes ~1400 sector reads: whether a given
	 * boot dies on the 2nd or the 4th `LOAD` line is just where the first
	 * tick happened to land. The `cli` after each bios_call (stage2_entry.S)
	 * does not help -- it runs after the round trip has already returned,
	 * and the tick arrives *during* the call, while the firmware has
	 * interrupts enabled because the trampoline pushes flags with IF set so
	 * the iret comes back with them.
	 *
	 * The loader is single-threaded and wants no interrupts at all, so
	 * masking every line is both the cheapest and the correct answer. The
	 * kernel remaps and unmasks the PIC itself (src/kernel/idt.c
	 * pic_remap), so nothing is lost by leaving them masked here.
	 */
	outb(0x21, 0xFF);		/* master: all IRQs masked */
	outb(0xA1, 0xFF);		/* slave:  all IRQs masked */

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
	bootinfo->version = BOOTINFO_VERSION;
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
	for (unsigned i = 0; i < sizeof(bootinfo->cmdline); i++)
		bootinfo->cmdline[i] = '\0';

	LOG("entering long mode, jumping to kernel at ");
	serial_puthex(kernel_entry_vaddr);
	serial_puts("\r\n");

	/* Does not return: the kernel takes over the machine here. */
	stage2_enter_long_mode((uint32_t)boot_pml4_phys,
			       (uint32_t)kernel_entry_vaddr,
			       (uint32_t)(uintptr_t)bootinfo);

	fail("returned from long mode transition");
}

/* Exported so stage2_entry.S can call it after clearing BSS. */
void stage2_start(void);
void stage2_start(void)
{
	stage2_main(STAGE2_ADDR);
}
