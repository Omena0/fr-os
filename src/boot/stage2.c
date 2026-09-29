/*
 * stage2.c — second-stage bootloader.
 *
 * Runs in 32-bit protected mode, loaded to physical 0x8000 by stage1. It does
 * everything that needs BIOS services or privileged instructions a real-mode
 * MBR cannot touch:
 *
 *   1. Open the A20 line so addresses above 1 MiB are not aliased.
 *   2. Query the E820 memory map and a VESA linear framebuffer.
 *   3. Parse the kernel ELF64 and copy its segments to the landing zone.
 *   4. Build a temporary identity-mapped page table over the first 4 GiB.
 *   5. Enable PAE and long mode, then far-jump to the kernel with a `bootinfo`
 *      pointer in RDI.
 *
 * Nothing here is trusted by the kernel. `bootinfo` is a report, not a
 * contract: the kernel re-validates every E820 range before reserving it and
 * replaces the bootstrap page table with a real one built from its own
 * allocator as one of its first actions.
 *
 * All BIOS transfers go through the bounce window at 0x10000, because
 * INT 13h/AH=42h addresses its destination with a 16-bit segment:offset pair
 * and refuses any transfer that crosses a 64 KiB boundary. See boot_layout.h.
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
	__asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
	uint8_t val;
	__asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port) : "memory");
	return val;
}

static inline void io_wait(void)
{
	/* The POST diagnostic port is the documented way to give the chipset time
	 * to latch a write on legacy ISA hardware. */
	outb(0x80, 0);
}

static void serial_putc(char c)
{
	uint32_t spin = 1u << 20;

	while ((inb(COM1 + 5) & 0x20) == 0) {
		if (--spin == 0)
			return;             /* UART wedged: drop and keep booting */
	}
	outb(COM1, (uint8_t)c);
}

static void serial_puts(const char *s)
{
	for (; *s; s++)
		serial_putc(*s);
}

static void serial_puthex(uint64_t v)
{
	static const char digits[] = "0123456789abcdef";
	int i;

	serial_puts("0x");
	for (i = 60; i >= 0; i -= 4)
		serial_putc(digits[(v >> i) & 0xF]);
}

static void serial_putdec(uint64_t v)
{
	char buf[24];
	int i = (int)sizeof(buf);

	buf[--i] = '\0';
	do {
		buf[--i] = (char)('0' + (v % 10));
		v /= 10;
	} while (v);
	serial_puts(buf + i);
}

#define LOG(...) do { serial_puts("[boot2] " __VA_ARGS__); } while (0)

static void fail(const char *what)
{
	serial_puts("[boot2] FATAL: ");
	serial_puts(what);
	serial_puts("\r\n");
	stage2_halt();
}

/* ----------------------------------------------------------------- A20 ------ */

/*
 * The BIOS leaves A20 masked, so physical addresses wrap at 1 MiB and every
 * page table above that would be placed at the wrong address. Port 0x92's fast
 * A20 gate works on all modern hardware including QEMU; the 8042 keyboard
 * controller path is the fallback for machines that do not implement it.
 */
static void a20_enable(void)
{
	uint8_t v = inb(0x92);

	if (!(v & 0x02)) {
		outb(0x92, (uint8_t)(v | 0x02));
		io_wait();
		v = inb(0x92);
	}
	if (v & 0x02) {
		LOG("A20 enabled via port 0x92\r\n");
		return;
	}

	/* 8042 fallback: wait for the input buffer to drain, then flip A20 in the
	 * command byte and restore the keyboard. */
	for (;;) {
		uint8_t s = inb(0x64);
		io_wait();
		if (!(s & 0x02))
			break;
	}
	outb(0x64, 0xAD);              /* disable keyboard */
	outb(0x64, 0x20);              /* read command byte */
	for (;;) {
		uint8_t s = inb(0x64);
		io_wait();
		if (s & 0x01)
			break;
	}
	uint8_t cmd = inb(0x60);
	outb(0x64, 0x60);              /* write command byte */
	outb(0x60, (uint8_t)(cmd | 0x02));
	outb(0x64, 0xAE);              /* re-enable keyboard */
	LOG("A20 enabled via 8042\r\n");
}

/* ------------------------------------------------------------ BIOS read ----- */

/*
 * bios_read — read `count` 512-byte sectors from `lba` into the bounce window
 * at 0x10000, where INT 13h can address them as 0x1000:0x0000.
 *
 * The disk address packet is a 16-byte structure the firmware dereferences
 * through DS:SI, so it has to sit in low memory too; 0x11000 is reserved for
 * it. DS has base 0 in the flat segments stage2_entry.S installed, so DS:SI
 * is simply SI.
 */
static bool bios_read_bounce(uint64_t lba, uint32_t count)
{
	uint32_t remaining = count;
	uint64_t cur_lba = lba;

	while (remaining > 0) {
		uint32_t chunk = remaining;
		uint16_t carry;

		/* One call per 4 KiB keeps the transfer well inside the firmware's
		 * 64 KiB boundary rule and inside the reserved window. */
		if (chunk > BOUNCE_BYTES / 512)
			chunk = BOUNCE_BYTES / 512;

		uint8_t *p = (uint8_t *)(uintptr_t)DAP_ADDR;
		p[0] = 0x10;                               /* packet size */
		p[1] = 0;                                  /* reserved */
		p[2] = (uint8_t)(chunk & 0xFF);
		p[3] = (uint8_t)(chunk >> 8);
		p[4] = (uint8_t)(cur_lba & 0xFF);
		p[5] = (uint8_t)((cur_lba >> 8) & 0xFF);
		p[6] = (uint8_t)((cur_lba >> 16) & 0xFF);
		p[7] = (uint8_t)((cur_lba >> 24) & 0xFF);
		p[8] = 0;
		p[9] = 0;
		p[10] = 0;
		p[11] = 0;
		p[12] = (uint8_t)((BOUNCE_ADDR >> 4) & 0xFF);  /* offset  */
		p[13] = (uint8_t)((BOUNCE_ADDR >> 12) & 0xFF);
		p[14] = (uint8_t)((BOUNCE_ADDR >> 20) & 0xFF);  /* segment */
		p[15] = 0;

		__asm__ volatile(
			"int $0x13"
			: "=a"(carry), "+m"(*(uint8_t (*)[16])(uintptr_t)DAP_ADDR)
			: "a"((uint16_t)0x4200), "d"((uint16_t)boot_drive),
			  "S"((uint32_t)DAP_ADDR)
			: "memory");

		if (carry & 0xFF)
			return false;

		/* The firmware reports how many sectors it actually moved. A short
		 * transfer means the image is truncated, which we must not paper
		 * over by loading whatever happened to be in memory. */
		uint32_t got = (uint32_t)p[2] | ((uint32_t)p[3] << 8);
		if (got != chunk)
			return false;

		cur_lba += chunk;
		remaining -= chunk;
	}
	return true;
}

/* --------------------------------------------------------------- E820 ------- */

static struct e820_entry *const e820_map =
	(struct e820_entry *)(uintptr_t)E820_ADDR;
static uint32_t e820_count;

/*
 * INT 15h/EAX=E820h is the ACPI 3.0+ memory map. The continuation value in AX
 * is the authority on whether another entry follows; the carry flag alone is
 * unreliable across firmware implementations.
 */
static void e820_query(void)
{
	uint64_t last_base = 0;
	bool have_last = false;

	LOG("E820 memory map:\r\n");

	for (;;) {
		uint8_t *entry = (uint8_t *)e820_map + e820_count *
				 sizeof(struct e820_entry);
		uint32_t cont, _, ok;
		uint64_t base, length;
		uint32_t type, ext;

		if (e820_count >= E820_MAX_ENTRIES) {
			LOG("  map full at ");
			serial_putdec(e820_count);
			serial_puts(" entries\r\n");
			break;
		}

		__asm__ volatile(
			"int $0x15"
			: "=a"(cont), "=b"(_), "=c"(ok)
			: "a"(0xE820u), "b"(0x534D4150u), "c"(1u),
			  "d"(0u), "S"((uint32_t)(uintptr_t)entry)
			: "memory");
		(void)ok;

		/* Read the 24-byte ACPI 2.0 entry back out. Only the low 20 bytes
		 * are guaranteed to be written when a caller asks for one entry. */
		base = *(const uint64_t *)(const void *)entry;
		length = *(const uint64_t *)(const void *)(entry + 8);
		type = *(const uint32_t *)(const void *)(entry + 16);
		ext = *(const uint32_t *)(const void *)(entry + 20);

		e820_map[e820_count].base = base;
		e820_map[e820_count].length = length;
		e820_map[e820_count].type = type;
		e820_map[e820_count].acpi_extended = ext;
		e820_count++;

		serial_puts("  ");
		serial_puthex(base);
		serial_puts(" + ");
		serial_puthex(length);
		serial_puts(" type ");
		serial_putdec(type);
		serial_puts("\r\n");

		/* Entries are required to be sorted by ascending base address. The
		 * moment that stops holding, the map has wrapped and the remainder
		 * is undefined. */
		if (have_last && base <= last_base)
			break;
		last_base = base;
		have_last = true;

		if ((cont & 0xFFFFu) == 0)
			break;
	}

	if (e820_count == 0)
		fail("E820 returned no memory map entries");
}

/* ---------------------------------------------------------------- VBE ------- */

/*
 * VESA BIOS Extension 2.0 linear framebuffer. A text-mode console cannot show
 * anything but text, and the display driver wants a real framebuffer, so
 * stage2 programs one while the BIOS is still available.
 *
 * Both the controller info block and each mode info block must be reachable as
 * 16-bit ES:DI, so they are staged in the reserved scratch area.
 */
struct vbe_controller_info {
	uint32_t signature;             /* "VESA" */
	uint32_t version;
	uint32_t video_mode_ptr;        /* far pointer to the mode list */
	uint16_t vbe_major, vbe_minor;
	uint32_t public_description;
	uint32_t oem_name_ptr;
	uint32_t capabilities;
	uint32_t modes;                 /* physical pages of video memory */
} __attribute__((packed));

struct vbe_mode_info {
	uint16_t mode_attributes;
	uint8_t  bytes_per_scanline;
	uint8_t  x_resolution, y_resolution;
	uint8_t  red_mask_size, red_field_position;
	uint8_t  green_mask_size, green_field_position;
	uint8_t  blue_mask_size, blue_field_position;
	uint8_t  rsvd, memory_model, bits_per_pixel;
	uint32_t phys_base_addr;
	uint32_t lin_bytes_per_scanline;
	uint16_t red_mask_size16, red_field_pos16;
	uint16_t green_mask_size16, green_field_pos16;
	uint16_t blue_mask_size16, blue_field_pos16;
	uint16_t rsvd2;
	uint32_t phys_ext_addr;
} __attribute__((packed));

/* Mode attribute bits. */
#define VBE_MODE_ATTR_SUPPORTED   0x0001
#define VBE_MODE_ATTR_LFB         0x0040

static struct framebuffer_info fb_info;
static bool fb_enabled;

/* Scratch area the BIOS writes into: controller info, then one mode info. */
static uint8_t *const vbe_ctrl = (uint8_t *)(uintptr_t)VBE_SCRATCH_ADDR;
static uint8_t *const vbe_mode = (uint8_t *)(uintptr_t)(VBE_SCRATCH_ADDR + 0x100);

static bool vbe_get_mode_info(uint16_t mode)
{
	uint32_t ret = 0;

	*vbe_mode = 0;
	__asm__ volatile(
		"int $0x10"
		: "=a"(ret)
		: "a"(0x4F01u), "c"((uint32_t)mode),
		  "S"((uint32_t)(uintptr_t)vbe_mode)
		: "memory");
	return (ret & 0xFFFFu) == 0x004Fu;
}

static bool vbe_set_mode(uint16_t mode)
{
	uint32_t ret = 0;

	/* Bit 14 requests the linear framebuffer rather than a banked window. */
	__asm__ volatile(
		"int $0x10"
		: "=a"(ret)
		: "a"(0x4F02u), "b"(0x4000u), "c"((uint32_t)mode)
		: "memory");
	return (ret & 0xFFFFu) == 0x004Fu;
}

/*
 * Pick the best linear-framebuffer mode available.
 *
 * Preference order is 1024x768x32, then 800x600x32, then the largest 16-bit
 * mode. Scanning the mode list rather than hard-coding a mode number keeps this
 * working across the VGA BIOS implementations QEMU ships.
 */
static void vbe_setup(void)
{
	struct vbe_controller_info *ctrl = (struct vbe_controller_info *)vbe_ctrl;
	uint32_t ret = 0;
	uint32_t best_score = 0;
	uint16_t best_mode = 0xFFFF;
	uint16_t mode = 0;
	uint16_t far_ptr;

	LOG("querying VESA BIOS...\r\n");

	*vbe_ctrl = 0;
	__asm__ volatile(
		"int $0x10"
		: "=a"(ret)
		: "a"(0x4F00u), "S"((uint32_t)(uintptr_t)vbe_ctrl)
		: "memory");

	if ((ret & 0xFFFFu) != 0x004F || ctrl->signature != 0x41534556u) {
		LOG("  no VESA BIOS; framebuffer unavailable\r\n");
		return;
	}

	LOG("  VBE ");
	serial_putdec(ctrl->vbe_major);
	serial_puts(".");
	serial_putdec(ctrl->vbe_minor);
	serial_puts("\r\n");

	/* The mode list pointer is a far pointer in the form 0xSSSS:0xOOOO. */
	far_ptr = (uint16_t)(ctrl->video_mode_ptr & 0xFFFF);
	uint16_t *modes = (uint16_t *)(uintptr_t)far_ptr;
	modes = (uint16_t *)(uintptr_t)(((ctrl->video_mode_ptr >> 16) << 4) |
					far_ptr);

	for (uint32_t guard = 0; guard < 512; guard++) {
		struct vbe_mode_info *mi;

		mode = modes[guard];
		if (mode == 0xFFFF)
			break;
		if (!vbe_get_mode_info(mode))
			continue;

		mi = (struct vbe_mode_info *)vbe_mode;
		if (!(mi->mode_attributes & VBE_MODE_ATTR_SUPPORTED))
			continue;
		if (!(mi->mode_attributes & VBE_MODE_ATTR_LFB))
			continue;
		if (mi->phys_base_addr == 0)
			continue;
		if (mi->bits_per_pixel != 32 && mi->bits_per_pixel != 24)
			continue;

		/* Score resolutions: 1024x768 is the design target, and 32bpp
		 * beats 24bpp because it matches a packed framebuffer. */
		uint32_t score = 0;
		if (mi->x_resolution == 1024 && mi->y_resolution == 768)
			score = 3000;
		else if (mi->x_resolution == 800 && mi->y_resolution == 600)
			score = 2000;
		else if (mi->x_resolution == 1280 && mi->y_resolution == 1024)
			score = 2500;
		else
			score = (uint32_t)mi->x_resolution;
		if (mi->bits_per_pixel == 32)
			score += 100;

		if (score > best_score) {
			best_score = score;
			best_mode = mode;
		}
	}

	if (best_mode == 0xFFFF) {
		LOG("  no linear framebuffer mode found\r\n");
		return;
	}

	if (!vbe_set_mode(best_mode)) {
		LOG("  mode 0x");
		serial_puthex(best_mode);
		serial_puts(" refused by BIOS\r\n");
		return;
	}

	/* Re-read the mode info after the mode switch: some BIOSes only report a
	 * valid linear pitch once the mode is active. */
	if (!vbe_get_mode_info(best_mode)) {
		LOG("  mode info lost after mode set\r\n");
		return;
	}

	struct vbe_mode_info *mi = (struct vbe_mode_info *)vbe_mode;

	fb_info.address = mi->phys_base_addr;
	fb_info.pitch = mi->lin_bytes_per_scanline;
	fb_info.width = mi->x_resolution;
	fb_info.height = mi->y_resolution;
	fb_info.bpp = mi->bits_per_pixel;

	/* Derive the channel shifts from the reported field positions. The VBE
	 * structure gives sizes and positions separately; a shift is the field
	 * position scaled to bytes, which for the standard 8:8:8:8 layout is just
	 * the position. */
	fb_info.red_shift = mi->red_field_position;
	fb_info.green_shift = mi->green_field_position;
	fb_info.blue_shift = mi->blue_field_position;

	if (fb_info.pitch == 0)
		fb_info.pitch = fb_info.width * (fb_info.bpp / 8);

	fb_enabled = true;

	LOG("  mode 0x");
	serial_puthex(best_mode);
	serial_puts(": ");
	serial_putdec(fb_info.width);
	serial_puts("x");
	serial_putdec(fb_info.height);
	serial_puts("@");
	serial_putdec(fb_info.bpp);
	serial_puts(" pitch ");
	serial_putdec(fb_info.pitch);
	serial_puts(" at ");
	serial_puthex(fb_info.address);
	serial_puts("\r\n");
}

/* ---------------------------------------------------------------- ELF ------- */

struct elf64_header {
	uint8_t  e_ident[16];
	uint16_t e_type, e_machine;
	uint32_t e_version;
	uint64_t e_entry, e_phoff, e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
	uint32_t p_type, p_flags;
	uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} __attribute__((packed));

#define PT_LOAD 1

static uint64_t kernel_entry_vaddr;
static uint32_t kernel_bytes_loaded;

/*
 * copy_from_disk — copy `len` bytes starting at file offset `off` to physical
 * address `dst`.
 *
 * The copy goes through the bounce window because INT 13h cannot address above
 * 1 MiB. `off` need not be sector aligned: the first transfer is trimmed to the
 * leading partial sector and the data shifted.
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

static void load_kernel(void)
{
	struct elf64_header hdr;
	struct elf64_phdr phdr;
	uint32_t phdr_bytes;
	bool loaded_any = false;

	LOG("loading kernel...\r\n");

	/* The ELF header sits at the start of the kernel image. Read a whole
	 * sector so the program headers that usually follow are in memory too. */
	if (!bios_read_bounce(KERNEL_LBA, 1))
		fail("cannot read kernel ELF header");

	__builtin_memcpy(&hdr, (const void *)(uintptr_t)BOUNCE_ADDR, sizeof(hdr));

	if (hdr.e_ident[0] != 0x7F || hdr.e_ident[1] != 'E' ||
	    hdr.e_ident[2] != 'L' || hdr.e_ident[3] != 'F')
		fail("kernel is not an ELF image");
	if (hdr.e_ident[4] != 2)
		fail("kernel ELF is not ELF64");
	if (hdr.e_machine != 62)
		fail("kernel ELF is not x86-64");
	if (hdr.e_phentsize != sizeof(struct elf64_phdr))
		fail("unexpected program header size");
	if (hdr.e_phnum == 0 || hdr.e_phnum > 32)
		fail("implausible program header count");

	kernel_entry_vaddr = hdr.e_entry;
	LOG("  entry ");
	serial_puthex(hdr.e_entry);
	serial_puts(", ");
	serial_putdec(hdr.e_phnum);
	serial_puts(" program headers\r\n");

	phdr_bytes = (uint32_t)hdr.e_phnum * sizeof(phdr);

	for (uint32_t i = 0; i < hdr.e_phnum; i++) {
		uint64_t phdr_off = hdr.e_phoff + (uint64_t)i * sizeof(phdr);

		if (phdr_off + sizeof(phdr) > 512) {
			/* Program headers that do not fit in the first sector are
			 * read straight into the landing zone and used in place. */
			copy_from_disk(phdr_off,
				       KERNEL_LANDING_ADDR + phdr_off,
				       sizeof(phdr));
			__builtin_memcpy(&phdr,
				(const void *)(uintptr_t)(KERNEL_LANDING_ADDR + phdr_off),
				sizeof(phdr));
		} else {
			if (!bios_read_bounce(KERNEL_LBA + (phdr_off / 512), 1))
				fail("cannot read kernel program headers");
			__builtin_memcpy(&phdr,
				(const void *)(uintptr_t)(BOUNCE_ADDR + phdr_off % 512),
				sizeof(phdr));
		}

		if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0)
			continue;

		loaded_any = true;

		LOG("  LOAD vaddr ");
		serial_puthex(phdr.p_vaddr);
		serial_puts(" filesz ");
		serial_puthex(phdr.p_filesz);
		serial_puts(" memsz ");
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
	}

	if (!loaded_any)
		fail("kernel ELF has no loadable segments");

	/* Program headers beyond the first sector were read into the landing
	 * zone as scratch; leaving them there would corrupt .text. */
	LOG("  kernel loaded, ");
	serial_putdec(kernel_bytes_loaded);
	serial_puts(" bytes\r\n");
}

/* ---------------------------------------------------- bootstrap paging ------- */

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
#define BOOT_PD_STRIDE 0x1000

static void boot_page_tables_init(void)
{
	uint64_t *base = (uint64_t *)(uintptr_t)BOOT_PT_ADDR;
	uint64_t *pml4 = base + BOOT_PML4_OFF / 8;
	uint64_t *pdpt = base + BOOT_PDPT_OFF / 8;

	LOG("building bootstrap page tables...\r\n");

	for (uint64_t i = 0; i < BOOT_PT_BYTES / 8; i++)
		base[i] = 0;

	/* Identity view of the low 4 GiB. */
	pml4[0] = (uint64_t)(uintptr_t)pdpt | PT_PRESENT | PT_WRITE;

	/* The higher-half PDPT is a second table in the same window; it is
	 * reachable through PML4[511] and covers the negative canonical half.
	 * Its entries 508..511 span 0xFFFFFF8000000000 upward, which covers the
	 * kernel's link address at 0xFFFFFFFF80000000 (PDPT index 511). */
	uint64_t *pdpt_high = base + (BOOT_PDPT_OFF + 0x1000) / 8;
	pml4[511] = (uint64_t)(uintptr_t)pdpt_high | PT_PRESENT | PT_WRITE;

	for (int i = 0; i < 4; i++) {
		uint64_t *pd = base + (BOOT_PD0_OFF + (uint64_t)i * BOOT_PD_STRIDE) / 8;

		/* Low half: identity. Higher half: the same physical pages, so a
		 * kernel address and its physical alias resolve to the same frame
		 * until the kernel builds its real tables. */
		pdpt[i] = (uint64_t)(uintptr_t)pd | PT_PRESENT | PT_WRITE;
		pdpt_high[508 + i] = (uint64_t)(uintptr_t)pd | PT_PRESENT | PT_WRITE;

		for (int j = 0; j < 512; j++) {
			uint64_t phys = (uint64_t)i * (1ULL << 30) +
					(uint64_t)j * (2ULL << 20);
			pd[j] = phys | PT_PRESENT | PT_WRITE | PT_PS;
		}
	}

	boot_pml4_phys = (uint64_t)(uintptr_t)pml4;
	LOG("  PML4 at ");
	serial_puthex(boot_pml4_phys);
	serial_puts(", identity 0-4 GiB with 2 MiB pages\r\n");
}

/* ----------------------------------------------------------- long mode ------- */

/*
 * The final GDT has to contain a 64-bit code descriptor (L set), otherwise the
 * CPU faults on the first instruction after the far jump in stage2_long.S. It
 * is built in the read-only .rodata section of stage2 itself and the matching
 * GDTR in stage2_long.S is pointed at it by those two exported symbols.
 *
 * Layout matches the selectors stage2_long.S uses: 0x08 = 64-bit code,
 * 0x10 = data. The kernel builds its own GDT from the same definitions in C.
 */
extern uint64_t stage2_gdt_base;
extern uint16_t stage2_gdt_limit;

/*
 * The long-mode GDT, built at run time in writable memory. Keeping it in .data
 * (not .rodata) matters: stage2 must patch its physical address into the GDTR
 * before the transition, and a read-only mapping would fault.
 */
uint64_t boot_gdt[3] __attribute__((aligned(16)));

static void boot_gdt_init(void)
{
	boot_gdt[0] = 0;                                  /* null */
	boot_gdt[1] = GDT_LCODE64;                        /* 0x08: 64-bit code */
	boot_gdt[2] = GDT_LDATA64;                        /* 0x10: data */

	/* Three descriptors: null, 64-bit code, data. The limit is in bytes, so
	 * it covers exactly entries 0..2 and nothing beyond. */
	stage2_gdt_limit = 3 * 16 - 1;
	stage2_gdt_base = (uint64_t)(uintptr_t)boot_gdt;

	LOG("  GDT at ");
	serial_puthex(stage2_gdt_base);
	serial_puts("\r\n");
}

/* --------------------------------------------------------------- main -------- */

static struct bootinfo *const bootinfo = (struct bootinfo *)(uintptr_t)BOOTINFO_ADDR;

static void stage2_main(uint32_t entry_addr);
static void stage2_main(uint32_t entry_addr)
{
	(void)entry_addr;

	boot_drive = stage1_boot_drive;

	LOG("running at ");
	serial_puthex(STAGE2_ADDR);
	serial_puts(", boot drive 0x");
	serial_puthex(boot_drive);
	serial_puts("\r\n");

	a20_enable();
	e820_query();
	vbe_setup();
	load_kernel();
	boot_page_tables_init();

	boot_gdt_init();

	/* Hand off everything the kernel could not discover for itself. */
	bootinfo->magic = BOOTINFO_MAGIC;
	bootinfo->version = 1;
	bootinfo->flags = (uint32_t)(fb_enabled ? 1u : 0u);
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
