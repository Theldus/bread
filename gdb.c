/*
 * MIT License
 *
 * Copyright (c) 2023 Davidson Francis <davidsondfgl@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>

#include "net.h"
#include "util.h"

#ifdef VERBOSE
#define DUMP_REGS 1
#endif

/* Convert a given SEG:OFF to physical address. */
#define TO_PHYS(S,O) (((S) << 4)+(O))

/* File descriptors for GDB and serial. */
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
#define SERIAL_STATE_ADD_HW_WATCH  0xB7
#define SERIAL_STATE_REM_HW_WATCH  0xC7
#define SERIAL_MSG_OK              0x04

/* Watch types. */
#define HW_WATCH_WRITE  0x01
#define HW_WATCH_ACCESS 0x03

/* Stop reasons. */
#define STOP_REASON_NORMAL      10
#define STOP_REASON_WATCHPOINT  20

/* The registers are cached, so this flag signals
 * if the cache is updated or not. */
static int have_x86_regs = 0;

/* Memory dump helpers. */
static uint8_t *dump_buffer;
static uint32_t last_dump_phys_addr;
static uint16_t last_dump_amnt;

/* Breakpoint cache. */
static uint32_t breakpoint_insn_addr;
static int single_step_before_continue;

/**
 * Mini-buffr to hold different byte-sized values
 * to send to serial.
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

/**
 * x86 stop data
 *
 * This is the data the is sent to us every time the
 * debugger has stopped, whether by single-step,
 * breakpoint, signal and etc.
 */
union x86_stop_data
{
	struct d
	{
		struct   srm_x86_regs x86_regs;
		uint8_t  stop_reason;
		uint32_t stop_addr;
#ifndef UART_POLLING
		uint8_t  saved_insns[4];
#endif
	} __attribute__((packed)) d;
	uint8_t data[sizeof (struct d)];
} x86_stop_data;

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

/* Amount of registers. */
#define MAX_REGS (sizeof(struct sx86_regs)/sizeof(uint32_t))

/**
 * The x86 registers that are kept as cache and sent to
 * GDB as well. Since these register might be accessed
 * from various ways, this union provides an easy way
 * to do that.
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
 * @brief Send a GDB command/packet in the format:
 * $data#NN, where NN is the checksum modulo 256.
 *
 * All GDB commands follows the same structure.
 *
 * @param buff Buffer containing the data to be
 * sent.
 * @param len Buffer length.
 *
 * @return Returns 0 if success, -1 otherwise.
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

	send_all(gdb_fd, "$", 1);
	send_all(gdb_fd, buff, len);
	send_all(gdb_fd, "#", 1);
	snprintf(csum_str, 3, "%02x", csum);
	ret = send_all(gdb_fd, csum_str, 2);

	if (ret < 0)
		errx("Unable to send command to GDB!\n");

	return (0);
}

/**
 * @brief Send the halt reason to GDB.
 */
static inline void send_gdb_halt_reason(void)
{
	char buf[32] = {0};

	/* Instruction hw bp, Ctrl+C, or initial break. */
	if (x86_stop_data.d.stop_reason == STOP_REASON_NORMAL)
	{
		send_gdb_cmd("S05", 3);
		return;
	}

	/*
	 * Hardware watchpoint
	 * GDB requires another type of message, that tells
	 * the stop reason and address.
	 */
	snprintf(buf, sizeof buf - 1, "T05watch:%08x;",
		x86_stop_data.d.stop_addr);

	send_gdb_cmd(buf, strlen(buf));
}

/**
 * @brief Acks a previous message/packet sent from GDB
 */
static inline void send_gdb_ack(void) {
	send_all(gdb_fd, "+", 1);
}

/**
 * @brief Tells GDB that we do not support the
 * receive message/packet.
 */
static inline void send_gdb_unsupported_msg(void) {
	send_gdb_cmd(NULL, 0);
}

/**
 * @brief Confirms that the previous command was
 * successfully executed.
 *
 * The 'OK' command is generally sent by the serial,
 * and then forwarded to GDB, as the serial device is
 * the only one that knows if the command succeeded
 * or not.
 */
static inline void send_gdb_ok(void) {
	send_gdb_cmd("OK", 2);
}

/**
 * @brief Tells GDB that something went wrong with
 * the latest command.
 */
static inline void send_gdb_error(void) {
	send_gdb_cmd("E00", 3);
}

/**
 * @brief Sends a 'Ctrl+C' to the serial device.
 */
static void send_serial_ctrlc(void) {
	send_serial_byte(3);
}

/* ------------------------------------------------------------------*
 * Misc                                                              *
 * ------------------------------------------------------------------*/

/**
 * Read all registers (already in cache), encodes
 * them to hex and returns it.
 *
 * @return Returns all the registers hex-encoded.
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
 * @brief Reads from a 'fake' memory (filled with NOPs)
 * and returns it.
 *
 * @param len Desired memory size.
 *
 * @return Returns the fake memory.
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
 * @brief Reads the current EIP (physical)
 * from the cache and returns it.
 *
 * @return Returns the physical EIP.
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
 * @brief Handles the single-step command from
 * GDB.
 */
static void handle_gdb_single_step(void)
{
	/* For mock, just send the we're already halted =). */
#ifdef USE_MOCKS
	send_gdb_halt_reason();
#else
	/* Send to our serial-line that we want a single-step. */
	send_serial_byte(SERIAL_STATE_SS);
	have_x86_regs = 0;
#endif
}

/**
 * @brief Sends the continue to the serial device.
 */
static void send_gdb_continue(void)
{
	have_x86_regs = 0;
	single_step_before_continue = 0;
	send_serial_byte(SERIAL_STATE_CONTINUE);
}

/**
 * @brief Handles the 'continue' command from GDB.
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
 * @brief Handles the 'halt reason (?)' command from GDB.
 */
static void handle_gdb_halt_reason(void) {
	send_gdb_halt_reason();
}

/**
 * @brief Handle he 'read registers (g)' command from GDB.
 */
static void handle_gdb_read_registers(void) {
	send_gdb_cmd(read_registers(), 128);
}

/**
 * @brief Handles the 'read memory (m)' command from GDB.
 *
 * @param Message buffer to be parsed.
 * @param Buffer length.
 *
 * @return Returns 0 if the request is valid, -1 otherwise.
 *
 * @note Please note that the actual memory read is
 * done by the serial device. This routine only parses
 * the command and forward the request to the serial
 * device.
 */
static int handle_gdb_read_memory(const char *buff, size_t len)
{
	const char *ptr;
	uint32_t addr, amnt;

	ptr = buff;

	/* Skip first 'm'. */
	expect_char('m', ptr, len);
	addr = read_int(ptr, &len, &ptr, 16);
	expect_char(',', ptr, len);

	/* Get amount. */
	amnt = simple_read_int(ptr, len, 16);

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
	send_serial_byte(SERIAL_STATE_READ_MEM_CMD);
	send_serial_dword(addr);
	send_serial_word(amnt);

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
 * @brief Handles the 'write memory (M)' command from GDB.
 *
 * @param Message buffer to be parsed.
 * @param Buffer length.
 *
 * @return Returns 0 if the request is valid, -1 otherwise.
 *
 * @note Please note that the actual memory write is
 * done by the serial device. This routine only parses
 * the command and forward the request to the serial
 * device.
 */
static int handle_gdb_write_memory_hex(const char *buff, size_t len)
{
	const char *ptr, *memory;
	uint32_t addr, amnt;

	ptr = buff;

	/* Skip first 'M'. */
	expect_char('M', ptr, len);
	addr = read_int(ptr, &len, &ptr, 16);
	expect_char(',', ptr, len);

	/* Get amount. */
	amnt = read_int(ptr, &len, &ptr, 16);
	expect_char(':', ptr, len);

	/* If 0, just send that we support X command and quit. */
	if (!amnt)
	{
		send_gdb_ok();
		return (0);
	}

	/* Decode hex buffer to binary. */
	memory = decode_hex(ptr, amnt);

	/* Send to our serial device. */
	send_serial_byte(SERIAL_STATE_WRITE_MEM_CMD);
	send_serial_dword(addr);
	send_serial_word(amnt);
	send_all(serial_fd, memory, amnt);
	return (0);
}

/**
 * @brief Handles the 'add breakpoint (Zn)' command from GDB.
 *
 * This routine handles all kinds of breakpoints that GDB
 * might ask for, from Z0 to Z4. Please note that even
 * SW breakpoints are handled as HW breakpoints, and this
 * current implementation only supports 1 instruction
 * breakpoint and 1 hw watchpoint (whether access or write).
 *
 * The only kind of breakpoint that is not supported is the
 * 'Z3' or 'read watchpoint', because x86 does not supports
 * them. However, GDB is smart enough to realize that and
 * asks to use Z4 instead. When a Z4 breakpoints (emulating
 * Z3) happens, all the writes are silently ignored and GDB
 * continues to execute again.
 *
 * @param Message buffer to be parsed.
 * @param Buffer length.
 *
 * @return Returns 0 if the request is valid, -1 otherwise.
 */
static int handle_gdb_add_breakpoint(const char *buff, size_t len)
{
	const char *ptr = buff;
	uint32_t addr;

	/* Skip 'Z0'. */
	expect_char('Z', ptr, len);
	expect_char_range('0', '4', ptr, len);
	expect_char(',', ptr, len);

	/* Get breakpoint address. */
	addr = read_int(ptr, &len, &ptr, 16);
	expect_char(',', ptr, len);

	/* Maybe convert to physical, if not already, */
	addr = to_physical(addr);

	/*
	 * Check which type of breakpoint we have and act
	 * accordingly.
	 */
	switch (buff[1]) {
	/*
	 * Instruction break,
	 * Since we only support hw break anyway, 0 (sw) and
	 * 1 (hw) will have the same meaning...
	 */
	case '0':
	case '1':
		breakpoint_insn_addr = addr;
		send_serial_byte(SERIAL_STATE_ADD_SW_BREAK);
		send_serial_dword(breakpoint_insn_addr);
		break;
	/* Write watchpoint. */
	case '2':
		send_serial_byte(SERIAL_STATE_ADD_HW_WATCH);
		send_serial_byte(HW_WATCH_WRITE);
		send_serial_dword(addr);
		break;
	/* Read watchpoint. */
	case '3':
		send_gdb_unsupported_msg();
		break;
	/* Access (Read/Write) watchpoint. */
	case '4':
		send_serial_byte(SERIAL_STATE_ADD_HW_WATCH);
		send_serial_byte(HW_WATCH_ACCESS);
		send_serial_dword(addr);
		break;
	}

	return (0);
}

/**
 * @brief Handles the 'remove breakpoint (zn)' command from GDB.
 *
 * Since one breakpoint for instruction and one for data is
 * supported, the address provided in the packet is ignored.
 *
 * @param Message buffer to be parsed.
 * @param Buffer length.
 *
 * @return Returns 0 if the request is valid, -1 otherwise.
 */
static int handle_gdb_remove_breakpoint(const char *buff, size_t len)
{
	const char *ptr = buff;

	/* Skip 'z0'. */
	expect_char('z', ptr, len);
	expect_char_range('0', '4', ptr, len);
	expect_char(',', ptr, len);

	/*
	 * Check which type of breakpoint we have and act
	 * accordingly.
	 *
	 * Since we only support 1 instruction (ATM) and 1
	 * data breakpoint, there is no need to check
	 * address and etc...
	 */
	switch (buff[1]) {
	/* Instruction break. */
	case '0':
	case '1':
		breakpoint_insn_addr = 0;
		send_serial_byte(SERIAL_STATE_REM_SW_BREAK);
		break;
	/* Remaining. */
	case '2':
	case '3':
	case '4':
		send_serial_byte(SERIAL_STATE_REM_HW_WATCH);
		break;
	}

	return (0);
}

/**
 * @brief Handles the 'write register (P)' GDB command;
 *
 * Please note the the segment registers and EIP,EFLAGS
 * are 16-bit. An attempt to write a 32-bit value on them
 * will emit an error.
 *
 * Also note that the mapping from what we receive from
 * the serial device and the mapping expected by GDB
 * differs, so there is a need to a conversion.
 *
 * @param buff Buffer to be parsed.
 * @param len Buffer length.
 *
 * @return Returns 0 if the command is valid, -1 otherwise.
 */
static int handle_gdb_write_register(const char *buff, size_t len)
{
	uint32_t reg_num_gdb, reg_num_rm;
	const char *ptr, *dec;
	union minibuf value;

	static const int gdb_to_rm[] =
		/* EAX. */                           /* GS. */
		   {7,6,5,4,3,2,1,0,13,15,14,12,11,10,9,8};

	ptr = buff;

	expect_char('P', ptr, len);
	reg_num_gdb = read_int(ptr, &len, &ptr, 16);
	expect_char('=', ptr, len);
	dec = decode_hex(ptr, 4);

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
	send_serial_byte(SERIAL_STATE_REG_WRITE);
	send_serial_byte(reg_num_rm);
	send_serial_dword(value.b32);
	return (0);
}

/*
 * Keeps all the variables for the GDB state machine here
 */
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
 * @brief Generic handler for all GDB commands/packets.
 *
 * This routine handles all messages and dispatches each
 * of them for the appropriated handler, if any. If not
 * supported, a not-supported packet is sent to GDB.
 *
 * @param gh GDB state machine data.
 *
 * @return Always 0.
 */
static int handle_gdb_cmd(struct gdb_handle *gh)
{
	int csum_chk;

	csum_chk = (int) simple_read_int(gh->csum_read, 2, 16);
	if (csum_chk != gh->csum)
		errw("Checksum for message: %s (%d) doesn't match: %d!\n",
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
		handle_gdb_add_breakpoint(gh->cmd_buff,
			sizeof gh->cmd_buff);
		break;
	/* Remove breakpoint. */
	case 'z':
		handle_gdb_remove_breakpoint(gh->cmd_buff,
			sizeof gh->cmd_buff);
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

/**
 * @brief Handle the start of state for a GDB command.
 *
 * If any non-valid start of command is received, the char
 * is silently ignored.
 *
 * @param gh GDB state machine data.
 * @param curr_byte Current byte read.
 */
static void handle_gdb_state_start(struct gdb_handle *gh,
	uint8_t curr_byte)
{
	/*
	 * If Ctrl+C.
	 *
	 * Ctrl+C/break is a special command that doesn't need
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

/**
 * @brief Handle the receipt of the first checksum digit.
 *
 * @param gh GDB state machine data.
 * @param curr_byte Current byte read.
 */
static inline void handle_gdb_state_csum_d1(struct gdb_handle *gh,
	uint8_t curr_byte)
{
	gh->csum_read[0] = curr_byte;
	gh->state = GDB_STATE_CSUM_D2;
}

/**
 * @brief Handle the receipt of the last checksum digit.
 *
 * This also marks the end of the command, so the command
 * in this stage is completely received and ready to be
 * parsed.
 *
 * @param gh GDB state machine data.
 * @param curr_byte Current byte read.
 */
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

/**
 * @brief Handle the command data.
 *
 * While already received a command, this routine saves its
 * content until the marker of end-of-command (#).
 *
 * @param gh GDB state machine data.
 * @param curr_byte Current byte read.
 */
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
 * @brief For each byte received, calls the appropriate
 * handler, accordingly with the byte and the current
 * state.
 *
 * @param hfd Socket handler (not used).
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
 * @brief Handles a read memory.
 *
 * In this point the serial device had already read the
 * memory and sent to the bridge. The memory is (almost)
 * ready to be sent to GDB.
 *
 * *Almost because while in interrupt-based mode, the
 * contents of the memory will change in order to keep
 * the CPU cool (hlt+jmp hlt). Due to that, if the
 * memory read overlaps the overwritten instructions,
 * we need to patch with the original instructions.
 *
 * @return Always 0.
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
		dump_buffer[i] = x86_stop_data.d.saved_insns[j];

#endif /* !UART_POLLING. */

no_patch:
	memory = encode_hex(dump_buffer, last_dump_amnt);
	free(dump_buffer);

	/* Send memory to GDB. */
	send_gdb_cmd(memory, last_dump_amnt * 2);
	return (0);
}

/**
 * @brief Handles the data received when the machine stops.
 *
 * Whenever the debugger stops, when machine sends to us
 * multiple data, including all of its registers. This
 * function takes all these registers and saves them
 * into the cache, as well as send the appropriate
 * messages to GDB to signalize that we're stopped.
 *
 * @param x86_rm Real mode x86 registers.
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

/* Serial state machine data. */
struct serial_handle
{
	int  state;
	int  buff_idx;
	char buff[64];
	char csum_read[3];
	char cmd_buff[64];
} serial_handle = {
	.state = SERIAL_STATE_START
};


/**
 * @brief Handle the start of state for a serial command.
 *
 * If any non-valid start of command is received, the char
 * is silently ignored.
 *
 * Valid start states are: stop, read-memory or ok
 *
 * @param sh Serial state machine data.
 * @param curr_byte Current byte read.
 *
 * @note The messages sent from serial, are, in fact,
 * responses from earlier GDB messages, so thats why
 * number of possible responses are small: most of the
 * commands only an 'OK' is required.
 */
static void handle_serial_state_start(struct serial_handle *sh,
	uint8_t curr_byte)
{
	if (curr_byte == SERIAL_STATE_SS)
	{
		sh->state    = SERIAL_STATE_SS;
		sh->buff_idx = 0;
		memset(&x86_stop_data, 0, sizeof(union x86_stop_data));
	}
	else if (curr_byte == SERIAL_STATE_READ_MEM_CMD)
	{
		sh->state    = SERIAL_STATE_READ_MEM_CMD;
		sh->buff_idx = 0;
	}
	else if (curr_byte == SERIAL_MSG_OK)
		send_gdb_ok();
}

/**
 * @brief Handle the machine stop data and reason.
 *
 * This function grabs the stop data and calls the appropriate
 * handler.
 *
 * @param sh Serial state machine data.
 * @param Current byte read.
 */
static void handle_serial_state_ss(struct serial_handle *sh,
	uint8_t curr_byte)
{
	size_t x86_size = sizeof(union x86_stop_data);

	/* Save stopped data. */
	if (sh->buff_idx < x86_size)
		x86_stop_data.data[sh->buff_idx++] = curr_byte;

	/* Check if ended. */
	if (sh->buff_idx == x86_size)
	{
		sh->state = SERIAL_STATE_START;
		handle_serial_single_step_stop(&x86_stop_data.d.x86_regs);
	}
}

/**
 * @brief Handles the debugger response to an earlier
 * GDB read memory command.
 *
 * @param sh Serial state data.
 * @param curr_byte Current byte read.
 *
 * @note The data length is already saved when GDB
 * asks to read.
 */
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
 * @brief For each byte received, calls the appropriate
 * handler, accordingly with the byte and the current
 * state.
 *
 * @param hfd Socket handler (not used).
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
 * @brief Handles the accept() when the GDB client attempts
 * to connect.
 *
 * This routine also checks if its the right moment to receive
 * a GDB connection: the bridge must have a valid connection
 * with the serial device and the device must be already
 * in 'break', waiting for the debugger to proceed.
 *
 * @param hfd Handler structure.
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
 * @brief Handles the accept() when the serial client (VMs)
 * attempts to connect.
 *
 * This is only useful for VMs and its not used for
 * real hardware.s
 *
 * @param hfd Handler structure.
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
