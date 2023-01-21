#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>

#include "net.h"
#include "util.h"

/**/
static int gdb_fd;
static int serial_fd;

/* GDB handle states. */
#define GDB_STATE_START   0x1
#define GDB_STATE_CMD     0x2
#define GDB_STATE_CSUM_D1 0x4
#define GDB_STATE_CSUM_D2 0x8

/* Serial handle states. */
#define SERIAL_STATE_START        0x10
#define SERIAL_STATE_SS_STOPPED   0xC8
#define SERIAL_STATE_READ_MEM_CMD 0xD8

//#define USE_MOCKS

/**/
static int have_x86_regs = 0;

/* Memory dump helpers. */
static uint8_t *dump_buffer;
static uint32_t last_dump_phys_addr;
static uint16_t last_dump_amnt;
static uint8_t saved_insns[4];

/**
 *
 */
union minibuf
{
	uint8_t   b8[4];
	uint16_t b16[2];
	uint32_t b32;
};

/**
 * Real Mode-dbg x86 regs
 *
 * This is the one that is sent to us
 */
struct srm_x86_regs
{
	uint16_t gs;
	uint16_t fs;
	uint16_t es;
	uint16_t ds;
	uint16_t ss;
	uint32_t edi;
	uint32_t esi;
	uint32_t ebp;
	uint32_t esp;
	uint32_t ebx;
	uint32_t edx;
	uint32_t ecx;
	uint32_t eax;
	uint16_t eip;
	uint16_t cs;
	uint16_t eflags;
} __attribute__((packed));

/**/
union urm_x86_regs
{
	struct srm_x86_regs r;
	uint8_t r8[sizeof (struct srm_x86_regs)];
};

/**
 * This is the struct that will be passed as-is to GDB
 */
struct sx86_regs
{
	uint32_t eax;
	uint32_t ecx;
	uint32_t edx;
	uint32_t ebx;
	uint32_t esp;
	uint32_t ebp;
	uint32_t esi;
	uint32_t edi;
	uint32_t eip;
	uint32_t eflags;
	uint32_t cs;
	uint32_t ss;
	uint32_t ds;
	uint32_t es;
	uint32_t fs;
	uint32_t gs;
} __attribute__((packed));

/**
 *
 */
#define MAX_REGS (sizeof(struct sx86_regs)/sizeof(uint32_t))

/**
 *
 */
static union ux86_regs
{
	struct sx86_regs r;
	uint32_t r32[MAX_REGS];
	uint8_t r8[sizeof (struct sx86_regs)];
} x86_regs;

/**
 *
 */
static ssize_t send_gdb_cmd(const char *buff, size_t len)
{
	int i;
	int csum;
	ssize_t ret;
	char csum_str[3];

	/* Calculate checksum. */
	for (i = 0, csum = 0; i < len; i++)
		csum += buff[i];
	csum &= 0xFF;

	send_all(gdb_fd, "$", 1, 0);
	send_all(gdb_fd, buff, len, 0);
	send_all(gdb_fd, "#", 1, 0);
	snprintf(csum_str, 3, "%02x", csum);
	ret = send_all(gdb_fd, csum_str, 2, 0);

	if (ret < 0)
		errx("Unable to send command to GDB!\n");

	return (0);
}

/**
 *
 */
static char *read_registers(void)
{
	char *buff;
	int i;

#ifdef MOCK_REGISTERS
	for (i = 0; i < MAX_REGS; i++)
	{
		if (i & 1)
			x86_regs.r32[i] = 0xabcdefAB;
		else
			x86_regs.r32[i] = 0x12345678;
	}

	/* Some 'sane' data. */
	x86_regs.r.cs  = 0x0;
	x86_regs.r.ds  = 0x100;
	x86_regs.r.es  = 0x100;
	x86_regs.r.fs  = 0x100;
	x86_regs.r.gs  = 0x100;
	x86_regs.r.eip = 0x7c00;
#else
	if (!have_x86_regs)
		errx("Error! registers are not available!\n");
#endif

	buff = encode_hex((const char*)x86_regs.r8,
		sizeof (struct sx86_regs));
	return (buff);
}

#ifdef USE_MOCKS
/**
 *
 */
static char *read_mock_memory(size_t len)
{
	char *buff, *buff1;

	if ((buff = malloc(len)) == NULL)
		errx("Unable to allocate %zd bytes!\n", len);

	for (size_t i = 0; i < len; i++)
		buff[i] = 0x90;

	buff1 = encode_hex(buff, len);
	free(buff);

	return (buff1);
}



static char *read_memory(size_t len)
{
	char *buff, *buff1;

	if ((buff = malloc(len)) == NULL)
		errx("Unable to allocate %zd bytes!\n", len);

	for (size_t i = 0; i < len; i++)
		buff[i] = 0x90;

	buff1 = encode_hex(buff, len);
	free(buff);

	return (buff1);
}
#endif

/**
 *
 */
static void handle_single_step(void)
{
#ifdef USE_MOCKS
	send_gdb_cmd("S05", 3);
#endif
}

/**
 *
 */
static void handle_halt_reason(void)
{
	/* due to signal (SIGTRAP, 5). */
	send_gdb_cmd("S05", 3);
}

/**
 *
 */
static void handle_read_registers(void)
{
	send_gdb_cmd(read_registers(), 128);
}

/**
 *
 */
static int handle_read_memory(const char *buff, size_t len)
{
	const char* end;
	const char *ptr;
	uint32_t addr, amnt;
	uint32_t phys1, phys2;
	union minibuf mb;

	ptr = buff;

	/* Skip first 'm'. */
	ptr++;
	len--;

	/* Get base address. */
	addr = str2hex(ptr, len, &end);
	if (*end != ',')
		errw("Expected ',' got '%c'\n", *end);

	end++;

	/* Get amount. */
	len -= (end - ptr + 1);
	amnt = str2hex(end, len, NULL);

	/*
	 * Reading memory is tricky: GDB does not know
	 * real-mode and assume the memory is linear.
	 *
	 * Because of this, we have to do some workarounds to avoid
	 * possible problems:
	 *   a)  All addresses sent to the debugger (serial) will be
	 *       physical.
	 *
	 *   b)  The addresses that the GDB asks to read, need to be
	 *       converted to physical, which leads us to:
	 *
	 *   b1) Instruction addresses: if addr*cs is close to the
	 *       current eip*cs, then GDB is asking to read a
	 *       a instruction address. We proceed to convert the
	 *       address.
	 *
	 *   b2) If b1) is false, the address to be read is already
	 *       physical (requested by the user, who will always
	 *       provide physical addresses), and will be passed
	 *       without conversions to the debugger.
	 *
	 *   b3) There is a possibility that GDB also asks to read
	 *       from the stack, think about that later.
	 */

	/*
	 * Check if instruction is greater than threshold.
	 *  512 bytes is just a guesstimate....
	 */
	phys1 = (x86_regs.r.cs << 4) + addr;
	phys2 = (x86_regs.r.cs << 4) + x86_regs.r.eip;

	if (ABS((int32_t)phys1 - (int32_t)phys2) >= 512)
		goto already_physical;

	/* Convert to physical. */
	addr = phys1;

already_physical:
	last_dump_phys_addr = addr;
	last_dump_amnt = amnt;

#ifndef USE_MOCKS
	/* Already prepare our buffer. */
	dump_buffer = malloc(amnt);
	if (!dump_buffer)
		errx("Unable to alloc %d bytes!\n", amnt);

	printf("asked to read: %x / bytes: %d\n", addr, amnt);

	/*
	 * Asks the serial device to send its memory
	 *
	 * Our 'protocol', is as follows:
	 *
	 * 0xD8 <address-4-bytes-LE> <size-2-bytes-LE>
	 *  ^--- read memory command, 1-byte
	 */
	mb.b8[0]  = SERIAL_STATE_READ_MEM_CMD;
	send_all(serial_fd, &mb.b8[0],  sizeof mb.b8[0],  0);
	mb.b32    = addr;
	send_all(serial_fd, &mb.b32,    sizeof mb.b32,    0);
	mb.b16[0] = amnt;
	send_all(serial_fd, &mb.b16[0], sizeof mb.b16[0], 0);

	/*
	 * For serial, we do not answer immediately, instead,
	 * we wait for the serial answer, and only then,
	 * reply with the memory.
	 *
	 * For mocks, we answer immediately.
	 */
#endif

#ifdef USE_MOCKS
	ptr = read_mock_memory(len);
	send_gdb_cmd(ptr, len * 2);
#endif

	return (0);
}

/**
 *
 */
static int handle_receive_read_memory(void)
{
	char *memory;
	int i, j, k, count;
	uint32_t break_eip;
	int sindex, eindex;
	uint32_t start_addr, end_addr;

	/* We need to check if the retrieved memory
	 * belongs do instruction or data.
	 *
	 * If instruction, given the base address
	 * check if there is a need to patch
	 * with our saved insns or not.
	 *
	 * If data, nothing need to be done.
	 */
	break_eip  = (x86_regs.r.cs << 4) + x86_regs.r.eip;
	start_addr = last_dump_phys_addr;
	end_addr   = start_addr + last_dump_amnt - 1;

	/*
	 * Check if our break-point EIP is inside the range
	 * of the dumped memory.
	 */
	if ((start_addr < break_eip && end_addr < break_eip)
		|| start_addr > break_eip + 3)
	{
		goto no_patch;
	}

	/* Calculate indexes and patch it. */
	if (start_addr < break_eip)
	{
		i = break_eip - start_addr;
		j = 0;
		count = MIN(end_addr - break_eip + 1, 4);
	}
	else
	{
		i = 0;
		j = start_addr - break_eip;
		count = MIN(break_eip + 3, end_addr) - start_addr + 1;
	}

	/* Patch. */
	for (k = 0; k < count; k++, i++, j++)
		dump_buffer[i] = saved_insns[j];

no_patch:
	memory = encode_hex(dump_buffer, last_dump_amnt);
	free(dump_buffer);

	/* Send memory to GDB. */
	send_gdb_cmd(memory, last_dump_amnt * 2);
	return (0);
}

/**
 *
 */
static int handle_gdb_cmd(const char *buff, size_t len, int csum,
	const char cread[3])
{
	int csum_chk;

	csum_chk = (int) str2hex(cread, 2, NULL);
	if (csum_chk != csum)
		errw("Checksum for message: %s (%d) doesnt match: %d!\n",
			buff, csum_chk, csum);

	/* Ack received message. */
	send_all(gdb_fd, "+", 1, 0);

	/*
	 * Handle single-char messages.
	 *
	 * From GDB docs:
	 * > At a minimum, a stub is required to support the ‘?’
	 *   command to tell GDB the reason for halting, ‘g’ and
	 *   ‘G’ commands for register access, and the ‘m’ and
	 *   ‘M’ commands for memory access.
	 *
	 * > Stubs that only control single-threaded targets can
	 *   implement run control with the ‘c’ (continue)
	 *   command, and if the target architecture supports
	 *   hardware- assisted single-stepping, the ‘s’ (step)
	 *   command.
	 *
	 * > ... All other commands are optional.
	 */
	switch (buff[0]) {
	/* Read registers. */
	case 'g':
		handle_read_registers();
		break;
	/* Read memory. */
	case 'm':
		handle_read_memory(buff, len);
		break;
	/* Halt reason. */
	case '?':
		handle_halt_reason();
		break;
	/* Single-step. */
	case 's':
		handle_single_step();
		break;
	/* Not-supported messages. */
	default:
		send_gdb_cmd(NULL, 0);
		break;
	}

	return (0);
}

/**
 *
 */
void handle_gdb_msg(struct handler_fd *hfd)
{
	int i;
	ssize_t ret;
	static int state  = GDB_STATE_START;
	static int csum   = 0;
	static char buff[32];
	static char csum_read[3] = {0};
	static char cmd_buff[64] = {0};
	static int  cmd_idx      =  0;

	gdb_fd = hfd->fd;

	ret = recv(gdb_fd, buff, sizeof buff, 0);
	if (ret <= 0)
		errx("GDB closed!\n");

	for (i = 0; i < ret; i++)
	{
		switch (state) {
		case GDB_STATE_START:
			/* Skip any char before a start of command. */
			if (buff[i] != '$')
				continue;

			state   = GDB_STATE_CMD;
			memset(cmd_buff, 0, sizeof cmd_buff);
			csum    = 0;
			cmd_idx = 0;
			break;

		case GDB_STATE_CSUM_D1:
			csum_read[0] = buff[i];
			state = GDB_STATE_CSUM_D2;
			break;

		case GDB_STATE_CSUM_D2:
			csum_read[1] = buff[i];
			state        = GDB_STATE_START;
			csum        &= 0xFF;

			/* Handles the command. */
			handle_gdb_cmd(cmd_buff, sizeof cmd_buff, csum, csum_read);

			LOG_CMD_REC("Command: (%s), csum: %x, csum_read: %s\n",
				cmd_buff, csum, csum_read);
			break;

		/* Inside a command. */
		case GDB_STATE_CMD:
			if (buff[i] == '#') {
				state = GDB_STATE_CSUM_D1;
				continue;
			}
			csum += buff[i];

			/* Just ignore if command exceeds buffer size. */
			if (cmd_idx > sizeof cmd_buff - 2)
				continue;

			cmd_buff[cmd_idx++] = buff[i];
			break;
		}
	}
}

/**
 *
 */
static void handle_single_step_stop(struct srm_x86_regs *x86_rm)
{
	x86_regs.r.eax = x86_rm->eax;
	x86_regs.r.ecx = x86_rm->ecx;
	x86_regs.r.edx = x86_rm->edx;
	x86_regs.r.ebx = x86_rm->ebx;
	x86_regs.r.esp = x86_rm->esp + 6; /* because we need to disconsider. */
	x86_regs.r.ebp = x86_rm->ebp;     /* EFLAGS+CS+EIP that are already. */
	x86_regs.r.esi = x86_rm->esi;     /* in the stack. */
	x86_regs.r.edi = x86_rm->edi;
	x86_regs.r.eip = x86_rm->eip;
	x86_regs.r.eflags = x86_rm->eflags;
	x86_regs.r.cs = x86_rm->cs;
	x86_regs.r.ss = x86_rm->ss;
	x86_regs.r.ds = x86_rm->ds;
	x86_regs.r.es = x86_rm->es;
	x86_regs.r.fs = x86_rm->fs;
	x86_regs.r.gs = x86_rm->gs;
	have_x86_regs = 1;

	if (gdb_fd <= 0)
		printf("Single-stepped, you can now connect GDB!\n");

#ifdef DUMP_REGS
	printf("eax: 0x%x\n", x86_regs.r.eax);
	printf("ebx: 0x%x\n", x86_regs.r.ebx);
	printf("ecx: 0x%x\n", x86_regs.r.ecx);
	printf("edx: 0x%x\n", x86_regs.r.edx);
	printf("esi: 0x%x\n", x86_regs.r.esi);
	printf("edi: 0x%x\n", x86_regs.r.edi);
	printf("ebp: 0x%x\n", x86_regs.r.ebp);
	printf("esp: 0x%x\n", x86_regs.r.esp);

	printf("eip: 0x%x\n", x86_regs.r.eip);
	printf("eflags: 0x%x\n", x86_regs.r.eflags);

	printf("cs: 0x%x\n", x86_regs.r.cs);
	printf("ds: 0x%x\n", x86_regs.r.ds);
	printf("es: 0x%x\n", x86_regs.r.es);
	printf("ss: 0x%x\n", x86_regs.r.ss);
	printf("fs: 0x%x\n", x86_regs.r.fs);
	printf("gs: 0x%x\n", x86_regs.r.gs);
#endif
}

/**
 *
 */
void handle_serial_msg(struct handler_fd *hfd)
{
	int i;
	ssize_t ret;
	uint8_t curr_byte;
	size_t x86_regs_size;

	static union urm_x86_regs rm_x86_r;
	static int state      = SERIAL_STATE_START;
	static char buff[64];
	static int  buf_idx   =  0;

	static char csum_read[3] = {0};
	static char cmd_buff[64] = {0};

	serial_fd = hfd->fd;
	x86_regs_size = sizeof(union urm_x86_regs);

	ret = recv(serial_fd, buff, sizeof buff, 0);
	if (ret <= 0)
		errx("Serial closed!\n");

	for (i = 0; i < ret; i++)
	{
		curr_byte = buff[i] & 0xFF;

		switch (state) {
		case SERIAL_STATE_START:
			if (curr_byte == SERIAL_STATE_SS_STOPPED)
			{
				state   = SERIAL_STATE_SS_STOPPED;
				buf_idx = 0;
				memset(&rm_x86_r, 0, x86_regs_size);
			}
			else if (curr_byte == SERIAL_STATE_READ_MEM_CMD)
			{
				printf("read mem!\n");
				state   = SERIAL_STATE_READ_MEM_CMD;
				buf_idx = 0;
			}
			break;

		/*
		 * PC has stopped and have dumped the regs + sav mem
		 * So this state saves the regs + the saved instructions
		 */
		case SERIAL_STATE_SS_STOPPED:
			/* Save regs. */
			if (buf_idx < x86_regs_size)
				rm_x86_r.r8[buf_idx++] = buff[i];

			/* Save instructions. */
			else if (buf_idx < x86_regs_size + 4)
			{
				saved_insns[buf_idx - x86_regs_size] = buff[i];
				buf_idx++;
			}

			/* Check if ended. */
			if (buf_idx == x86_regs_size + 4)
			{
				state = SERIAL_STATE_START;
				handle_single_step_stop(&rm_x86_r.r);
			}
			break;

		/* PC has answered with the memory. */
		case SERIAL_STATE_READ_MEM_CMD:
			dump_buffer[buf_idx++] = curr_byte;
			if (buf_idx == last_dump_amnt)
			{
				state = SERIAL_STATE_START;
				handle_receive_read_memory();
			}
			break;
		}
	}
}

/**
 *
 */
void handle_accept_gdb(struct handler_fd *hfd)
{
	struct handler_fd h;
	int fd;

	if (!have_x86_regs)
		errx("GDB must be connected after breakpoint!\n");

	fd = accept(hfd->fd, NULL, NULL);
	if (fd < 0)
		errx("Failed to accept connection, aborting...\n");

	printf("GDB connected!\n");

	h.fd = fd;
	h.handler = handle_gdb_msg;
	change_handled_fd(hfd->fd, &h);
}

/**
 *
 */
void handle_accept_serial(struct handler_fd *hfd)
{
	struct handler_fd h;
	int fd;

	fd = accept(hfd->fd, NULL, NULL);
	if (fd < 0)
		errx("Failed to accept connection, aborting...\n");

	printf("Serial connected, please wait...\n");

	h.fd = fd;
	h.handler = handle_serial_msg;
	change_handled_fd(hfd->fd, &h);
}
