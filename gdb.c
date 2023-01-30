#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>

#include "net.h"
#include "util.h"

#define DUMP_REGS 1

#define TO_PHYS(S,O) (((S) << 4)+(O))

/**/
static int gdb_fd;
static int serial_fd;

/* GDB handle states. */
#define GDB_STATE_START   0x1
#define GDB_STATE_CMD     0x2
#define GDB_STATE_CSUM_D1 0x4
#define GDB_STATE_CSUM_D2 0x8

/* Serial handle states. */
#define SERIAL_STATE_START         0x10
#define SERIAL_STATE_ADD_SW_BREAK  0xA8
#define SERIAL_STATE_REM_SW_BREAK  0xB8
#define SERIAL_STATE_SS            0xC8
#define SERIAL_STATE_READ_MEM_CMD  0xD8
#define SERIAL_STATE_CONTINUE      0xE8
#define SERIAL_STATE_WRITE_MEM_CMD 0xF8
#define SERIAL_STATE_REG_WRITE     0xA7
#define SERIAL_MSG_OK              0x04

//#define USE_MOCKS

/**/
static int have_x86_regs = 0;

/* Memory dump helpers. */
static uint8_t *dump_buffer;
static uint32_t last_dump_phys_addr;
static uint16_t last_dump_amnt;

/* Breakpoint cache. */
static uint32_t breakpoint_insn_addr;
static int single_step_before_continue;


#ifndef UART_POLLING
static uint8_t saved_insns[4];
#endif

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
	uint32_t edi;
	uint32_t esi;
	uint32_t ebp;
	uint32_t esp;
	uint32_t ebx;
	uint32_t edx;
	uint32_t ecx;
	uint32_t eax;
	uint16_t gs;
	uint16_t fs;
	uint16_t es;
	uint16_t ds;
	uint16_t ss;
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

/* ------------------------------------------------------------------*
 * GDB commands                                                      *
 * ------------------------------------------------------------------*/

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
static inline void send_gdb_halt_reason(void) {
	send_gdb_cmd("S05", 3);
}

/**
 *
 */
static inline void send_gdb_ack(void) {
	send_all(gdb_fd, "+", 1, 0);
}

/**
 *
 */
static inline void send_gdb_unsupported_msg(void) {
	send_gdb_cmd(NULL, 0);
}

/**
 *
 */
static inline void send_gdb_ok(void) {
	send_gdb_cmd("OK", 2);
}

/**
 *
 */
static inline void send_gdb_error(void) {
	send_gdb_cmd("E00", 3);
}

/**
 *
 */
static void send_serial_ctrlc(void)
{
	union minibuf mb;
	mb.b8[0] = 3;
	send_all(serial_fd, &mb.b8[0], sizeof mb.b8[0], 0);
}

/* ------------------------------------------------------------------*
 * Misc                                                              *
 * ------------------------------------------------------------------*/

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
#endif

/**
 *
 */
static uint32_t get_current_eip_phys(void) {
	return TO_PHYS(x86_regs.r.cs, x86_regs.r.eip);
}


/**
 * @brief Attempts to convert a passed address from GDB
 * to its physical counterpart.
 *
 * Since GDB does not know anything about SEG:OFF
 * the address passed through EIP is 'wrong'. This
 * function attempts to convert the given address
 * and then check if its similar to the current
 * SEG:OFF EIP, if so, the address probably is
 * physical
 *
 * @param addr Maybe not-physical address to be
 *             converted.
 *
 * @return Returns the same address, or the physical
 * converted address.
 *
 */
static uint32_t to_physical(uint32_t addr)
{
	uint32_t phys1, phys2;
	uint32_t ret;

	ret = addr;

	/*
	 * Check if instruction is greater than threshold.
	 *  512 bytes is just a guesstimate....
	 */
	phys1 = TO_PHYS(x86_regs.r.cs, addr);
	phys2 = get_current_eip_phys();

	if (ABS((int32_t)phys1 - (int32_t)phys2) >= 512)
		goto already_physical;

	ret = phys1;

already_physical:
	return (ret);
}

/* ------------------------------------------------------------------*
 * GDB handlers                                                      *
 * ------------------------------------------------------------------*/

/**
 *
 */
static void handle_gdb_single_step(void)
{
	/* For mock, just send the we're already halted =). */
#ifdef USE_MOCKS
	send_gdb_halt_reason();
#else
	union minibuf mb;

	/* Send to our serial-line that we want a single-step. */
	mb.b8[0]  = SERIAL_STATE_SS;
	send_all(serial_fd, &mb.b8[0], sizeof mb.b8[0], 0);

	have_x86_regs = 0;
#endif
}

/**
 *
 */
static void send_gdb_continue(void)
{
	union minibuf mb;
	have_x86_regs = 0;
	single_step_before_continue = 0;
	mb.b8[0] = SERIAL_STATE_CONTINUE;
	send_all(serial_fd, &mb.b8[0], sizeof mb.b8[0], 0);
}

/**
 *
 */
static void handle_gdb_continue(void)
{
	/*
	 * Check if we should single-step first:
	 *
	 * If we are currently at the same address where we add the
	 * breakpoint, we are in a dilemma, because how are we going
	 * to execute the code if the break is the current code
	 * itself?
	 *
	 * GDB catches on to this and issues a single-step before
	 * the continue, which effectively solves the problem,
	 * right? almost. GDB assumes linear memory and is unaware
	 * of segment:offset, so it cannot compare a physical address
	 * with the current CS+EIP and thus does not issue the
	 * necessary single-step.
	 *
	 * To get around this, we need to mimic what GDB does, and
	 * silently issue a single-step on its own before proceeding
	 * with continue.
	 */
	if (breakpoint_insn_addr == get_current_eip_phys())
	{
		single_step_before_continue = 1;
		handle_gdb_single_step();
		return;
	}

	/* Send to our serial-line that we want to continue. */
	send_gdb_continue();
}

/**
 *
 */
static void handle_gdb_halt_reason(void) {
	/* due to signal (SIGTRAP, 5). */
	send_gdb_halt_reason();
}

/**
 *
 */
static void handle_gdb_read_registers(void) {
	send_gdb_cmd(read_registers(), 128);
}

/**
 *
 */
static int handle_gdb_read_memory(const char *buff, size_t len)
{
	const char* end;
	const char *ptr;
	union minibuf mb;
	uint32_t addr, amnt;

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

	/* Convert to physical. */
	addr = to_physical(addr);
	last_dump_phys_addr = addr;
	last_dump_amnt = amnt;

#ifndef USE_MOCKS
	/* Already prepare our buffer. */
	dump_buffer = malloc(amnt);
	if (!dump_buffer)
		errx("Unable to alloc %d bytes!\n", amnt);

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
static int handle_gdb_write_memory_hex(const char *buff, size_t len)
{
	const char *ptr, *end, *memory;
	uint32_t addr, amnt;
	union minibuf mb;

	ptr = buff;

	/* Skip first 'X'. */
	ptr++;
	len--;

	/* Get base address. */
	addr = str2hex(ptr, len, &end);
	if (*end != ',')
		errw("Expected ',' got '%c'\n", *end);

	end++;

	/* Get amount. */
	len -= (end - ptr + 1);
	amnt = str2hex(end, len, &end);
	if (*end != ':')
		errw("Expected ':' got '%c'\n", *end);

	/* If 0, just send that we support X command and quit. */
	if (!amnt)
	{
		send_gdb_ok();
		return (0);
	}

	end++;

	/* Decode hex buffer to binary. */
	memory = decode_hex(end, amnt);

	/* Send to our serial device. */
	mb.b8[0]  = SERIAL_STATE_WRITE_MEM_CMD;
	send_all(serial_fd, &mb.b8[0],  sizeof mb.b8[0],  0);
	mb.b32    = addr;
	send_all(serial_fd, &mb.b32,    sizeof mb.b32,    0);
	mb.b16[0] = amnt;
	send_all(serial_fd, &mb.b16[0], sizeof mb.b16[0], 0);
	send_all(serial_fd, memory,     amnt,             0);

	return (0);
}

/**
 *
 */
static int handle_gdb_add_sw_breakpoint(const char *buff, size_t len)
{
	const char *ptr, *end, *memory;
	uint32_t addr, amnt;
	union minibuf mb;
	uint32_t phys1, phys2;

	/* Skip 'Z0'. */
	ptr  = buff;
	ptr += 2;
	len -= 2;

	if (*ptr != ',')
		errw("Expected ',' got '%c'\n", *end);

	ptr++;
	len--;

	/* Get breakpoint address. */
	addr = str2hex(ptr, len, &end);
	if (*end != ',')
		errw("Expected ',' got '%c'\n", *end);

	/* Maybe convert to physical, if not already, */
	addr = to_physical(addr);
	breakpoint_insn_addr = addr;

	/* Send to our serial device. */
	mb.b8[0]  = SERIAL_STATE_ADD_SW_BREAK;
	send_all(serial_fd, &mb.b8[0], sizeof mb.b8[0], 0);
	mb.b32    = breakpoint_insn_addr;
	send_all(serial_fd, &mb.b32,   sizeof mb.b32,   0);
	return (0);
}

/**
 *
 */
static int handle_gdb_remove_sw_breakpoint(const char *buff, size_t len)
{
	union minibuf mb;

	((void)buff); /* Only support 1 sw break at the moment. */
	((void)len);

	breakpoint_insn_addr = 0;

	/* Send to our serial device. */
	mb.b8[0] = SERIAL_STATE_REM_SW_BREAK;
	send_all(serial_fd, &mb.b8[0], sizeof mb.b8[0], 0);
}

/**
 *
 */
static int handle_gdb_write_register(const char *buff, size_t len)
{
	uint32_t reg_num_gdb, reg_num_rm;
	const char *ptr, *end, *dec;
	union minibuf value, mb;

	static const int gdb_to_rm[] =
		/* EAX. */                           /* GS. */
		   {7,6,5,4,3,2,1,0,13,15,14,12,11,10,9,8};

	ptr = buff;

	/* Skip first 'P'. */
	ptr++;
	len--;

	/* Get reg num. */
	reg_num_gdb = str2hex(ptr, len, &end);
	if (*end != '=')
		errw("Expected '=' got '%c'\n", *end);

	end++;

	/* Get value. */
	len -= (end - ptr + 1);
	dec  = decode_hex(end, 4);
	memcpy(&value, dec, 4);

	/* Validate register. */
	if (reg_num_gdb >= 16)
	{
		send_gdb_error();
		return (-1);
	}

	reg_num_rm = gdb_to_rm[reg_num_gdb];

	/*
	 * Validate value: 16-bit registers should not
	 * receive values greater than 16-bit =)
	 */
	if (reg_num_rm >= 8 && value.b32 > ((1<<16)-1))
	{
		send_gdb_error();
		return (-1);
	}

	/* Update our 'cache'. */
	x86_regs.r32[reg_num_gdb] = value.b32;

	/* Send to our serial device. */
	mb.b8[0]  = SERIAL_STATE_REG_WRITE;
	send_all(serial_fd, &mb.b8[0],  sizeof mb.b8[0], 0);
	mb.b8[0]  = reg_num_rm;
	send_all(serial_fd, &mb.b8[0],  sizeof mb.b8[0], 0);
	mb.b32    = value.b32;
	send_all(serial_fd, &mb.b32,    sizeof mb.b32,   0);
	return (0);
}

/**/
struct gdb_handle
{
	int  state;
	int  csum;
	int  cmd_idx;
	char buff[32];
	char csum_read[3];
	char cmd_buff[512];
} gdb_handle = {
	.state = GDB_STATE_START
};

/**
 *
 */
static int handle_gdb_cmd(struct gdb_handle *gh)
{
	int csum_chk;

	csum_chk = (int) str2hex(gh->csum_read, 2, NULL);
	if (csum_chk != gh->csum)
		errw("Checksum for message: %s (%d) doesnt match: %d!\n",
			gh->cmd_buff, csum_chk, gh->csum);

	/* Ack received message. */
	send_gdb_ack();

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
	switch (gh->cmd_buff[0]) {
	/* Read registers. */
	case 'g':
		handle_gdb_read_registers();
		break;
	/* Read memory. */
	case 'm':
		handle_gdb_read_memory(gh->cmd_buff,
			sizeof gh->cmd_buff);
		break;
	/* Memory write hex. */
	case 'M':
		handle_gdb_write_memory_hex(gh->cmd_buff,
			sizeof gh->cmd_buff);
		break;
	/* Halt reason. */
	case '?':
		handle_gdb_halt_reason();
		break;
	/* Single-step. */
	case 's':
		handle_gdb_single_step();
		break;
	/* Continue. */
	case 'c':
		handle_gdb_continue();
		break;
	/* Insert breakpoint. */
	case 'Z':
		if (gh->cmd_buff[1] == '0')
			handle_gdb_add_sw_breakpoint(gh->cmd_buff,
			sizeof gh->cmd_buff);
		else
			send_gdb_unsupported_msg();
		break;
	/* Remove breakpoint. */
	case 'z':
		if (gh->cmd_buff[1] == '0')
			handle_gdb_remove_sw_breakpoint(gh->cmd_buff,
			sizeof gh->cmd_buff);
		else
			send_gdb_unsupported_msg();
		break;
	/* Write register. */
	case 'P':
		handle_gdb_write_register(gh->cmd_buff,
			sizeof gh->cmd_buff);
		break;
	/* Not-supported messages. */
	default:
		send_gdb_unsupported_msg();
		break;
	}

	return (0);
}

/* ------------------------------------------------------------------*
 * GDB handling state machine                                        *
 * ------------------------------------------------------------------*/

static void handle_gdb_state_start(struct gdb_handle *gh,
	uint8_t curr_byte)
{
	/*
	 * If Ctrl+C.
	 *
	 * Ctrl+C/break is a special command that doesnt need
	 * to be ack'ed nor anything
	 */
	if (curr_byte == 3)
	{
		send_serial_ctrlc();
		return;
	}

	/* Skip any char before a start of command. */
	if (curr_byte != '$')
		return;

	gh->state   = GDB_STATE_CMD;
	memset(gh->cmd_buff, 0, sizeof gh->cmd_buff);
	gh->csum    = 0;
	gh->cmd_idx = 0;
}

static inline void handle_gdb_state_csum_d1(struct gdb_handle *gh,
	uint8_t curr_byte)
{
	gh->csum_read[0] = curr_byte;
	gh->state = GDB_STATE_CSUM_D2;
}

static inline void handle_gdb_state_csum_d2(struct gdb_handle *gh,
	uint8_t curr_byte)
{
	gh->csum_read[1] = curr_byte;
	gh->state        = GDB_STATE_START;
	gh->csum        &= 0xFF;

	/* Handles the command. */
	handle_gdb_cmd(gh);

	LOG_CMD_REC("Command: (%s), csum: %x, csum_read: %s\n",
		gh->cmd_buff, gh->csum, gh->csum_read);
}

static inline void handle_gdb_state_cmd(struct gdb_handle *gh,
	uint8_t curr_byte)
{
	if (curr_byte == '#')
	{
		gh->state = GDB_STATE_CSUM_D1;
		return;
	}
	gh->csum += curr_byte;

	/* Emit a warning if command exceeds buffer size. */
	if (gh->cmd_idx > sizeof gh->cmd_buff - 2)
		errx("Command exceeds buffer size (%zu): %s\n",
			gh->cmd_buff, sizeof gh->cmd_buff);

	gh->cmd_buff[gh->cmd_idx++] = curr_byte;
}

/**
 *
 */
void handle_gdb_msg(struct handler_fd *hfd)
{
	int i;
	ssize_t ret;
	uint8_t curr_byte;

	ret = recv(gdb_fd, gdb_handle.buff, sizeof gdb_handle.buff, 0);
	if (ret <= 0)
		errx("GDB closed!\n");

	for (i = 0; i < ret; i++)
	{
		curr_byte = gdb_handle.buff[i] & 0xFF;

		switch (gdb_handle.state) {
		/* Decide which state to go. */
		case GDB_STATE_START:
			handle_gdb_state_start(&gdb_handle, curr_byte);
			break;
		/* First digit checksum. */
		case GDB_STATE_CSUM_D1:
			handle_gdb_state_csum_d1(&gdb_handle, curr_byte);
			break;
		/* Second digit checsum. */
		case GDB_STATE_CSUM_D2:
			handle_gdb_state_csum_d2(&gdb_handle, curr_byte);
			break;
		/* Inside a command. */
		case GDB_STATE_CMD:
			handle_gdb_state_cmd(&gdb_handle, curr_byte);
			break;
		}
	}
}

/* ------------------------------------------------------------------*
 * Serial handlers                                                   *
 * ------------------------------------------------------------------*/

/**
 *
 */
static int handle_serial_receive_read_memory(void)
{
	char *memory;

#ifndef UART_POLLING
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

#endif /* !UART_POLLING. */

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
static void handle_serial_single_step_stop(struct srm_x86_regs *x86_rm)
{
	x86_regs.r.eax = x86_rm->eax;
	x86_regs.r.ecx = x86_rm->ecx;
	x86_regs.r.edx = x86_rm->edx;
	x86_regs.r.ebx = x86_rm->ebx;

	/*
	 * We need to disconsider the first 8 16-bit
	 * registers already pushed in the stack.
	 */
	x86_regs.r.esp = x86_rm->esp + (2*8);

	x86_regs.r.ebp = x86_rm->ebp;
	x86_regs.r.esi = x86_rm->esi;
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

	/*
	 * If there is a valid connection already:
	 * - Check if this a 'silent' single-step
	     (see handle_gdb_add_sw_breakpoint() for more info)
	 *
	 * Otherwise, tell GDB that we're already stopped.
	 */
	else if (single_step_before_continue)
		send_gdb_continue();
	else
		send_gdb_halt_reason();


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

/* ------------------------------------------------------------------*
 * Serial handling state machine                                     *
 * ------------------------------------------------------------------*/

struct serial_handle
{
	int  state;
	int  buff_idx;
	char buff[64];
	char csum_read[3];
	char cmd_buff[64];
	union urm_x86_regs rm_x86_r;
} serial_handle = {
	.state = SERIAL_STATE_START
};


static void handle_serial_state_start(struct serial_handle *sh,
	uint8_t curr_byte)
{
	if (curr_byte == SERIAL_STATE_SS)
	{
		sh->state    = SERIAL_STATE_SS;
		sh->buff_idx = 0;
		memset(&sh->rm_x86_r, 0, sizeof(union urm_x86_regs));
	}
	else if (curr_byte == SERIAL_STATE_READ_MEM_CMD)
	{
		sh->state   = SERIAL_STATE_READ_MEM_CMD;
		sh->buff_idx = 0;
	}
	else if (curr_byte == SERIAL_MSG_OK)
		send_gdb_ok();
}

static void handle_serial_state_ss(struct serial_handle *sh,
	uint8_t curr_byte)
{
	size_t x86_regs_size = sizeof(union urm_x86_regs);

	/* Save regs. */
	if (sh->buff_idx < x86_regs_size)
		sh->rm_x86_r.r8[sh->buff_idx++] = curr_byte;

#ifndef UART_POLLING
	/* Save instructions. */
	else if (sh->buff_idx < x86_regs_size + 4)
	{
		saved_insns[sh->buff_idx - x86_regs_size] = curr_byte;
		sh->buff_idx++;
	}
#endif

	/* Check if ended. */
#ifndef UART_POLLING
	if (sh->buff_idx == x86_regs_size + 4)
#else
	if (sh->buff_idx == x86_regs_size)
#endif
	{
		sh->state = SERIAL_STATE_START;
		handle_serial_single_step_stop(&sh->rm_x86_r.r);
	}
}

static void handle_serial_state_read_mem_cmd(struct serial_handle *sh,
	uint8_t curr_byte)
{
	dump_buffer[sh->buff_idx++] = curr_byte;
	if (sh->buff_idx == last_dump_amnt)
	{
		sh->state = SERIAL_STATE_START;
		handle_serial_receive_read_memory();
	}
}


/**
 *
 */
void handle_serial_msg(struct handler_fd *hfd)
{
	int i;
	ssize_t ret;
	uint8_t curr_byte;

	serial_fd = hfd->fd;

	ret = read(serial_fd, serial_handle.buff,
		sizeof serial_handle.buff);

	if (ret <= 0)
		errx("Serial closed!\n");

	/* For each received byte. */
	for (i = 0; i < ret; i++)
	{
		curr_byte = serial_handle.buff[i] & 0xFF;

		switch (serial_handle.state) {
		/* Check which state should go, if any. */
		case SERIAL_STATE_START:
			handle_serial_state_start(&serial_handle, curr_byte);
			break;
		/*
		 * PC has stopped and have dumped the regs + sav mem
		 * So this state saves the regs + the saved instructions
		 */
		case SERIAL_STATE_SS:
			handle_serial_state_ss(&serial_handle, curr_byte);
			break;
		/* PC has answered with the memory. */
		case SERIAL_STATE_READ_MEM_CMD:
			handle_serial_state_read_mem_cmd(&serial_handle,
				curr_byte);
			break;
		}
	}
}

/* ------------------------------------------------------------------*
 * Accept/initialization routines                                    *
 * ------------------------------------------------------------------*/

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
	gdb_fd = fd;
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
	serial_fd = fd;
}
