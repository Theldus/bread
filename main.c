#include "util.h"
#include "net.h"
#include "gdb.h"
#include <getopt.h>
#include <string.h>

#define MODE_SERIAL 0
#define MODE_SOCKET 1

void usage(const char*);

/* Command line arguments structure. */
struct args
{
	int  mode;
	int  serial_port;
	int  gdb_port;
	char *device;
} args = {
	.mode = MODE_SERIAL,
	.serial_port = 2345,
	.gdb_port = 1234,
	.device = NULL,
};

/**
 * @brief Parse command-line arguments.
 *
 * @param argc Argument count.
 * @param argv Argument list.
 */
void parse_args(int argc, char **argv)
{
	int c; /* Current arg. */
	while ((c = getopt(argc, argv, "hsd:p:g:")) != -1)
	{
		switch (c) {
		case 'h':
			usage(argv[0]);
			break;
		case 's':
			args.mode = MODE_SOCKET;
			break;
		case 'd':
			args.device = strdup(optarg);
			break;
		case 'p':
			args.serial_port = simple_read_int(
				optarg, strlen(optarg), 10);
			break;
		case 'g':
			args.gdb_port = simple_read_int(
				optarg, strlen(optarg), 10);
			break;
		default:
			usage(argv[0]);
			break;
		}
	}

	/* Check for -s and -d. */
	if (args.mode == MODE_SOCKET)
	{
		if (args.device)
		{
			fprintf(stderr, "'-d' option is incompatible with '-s'\n");
			usage(argv[0]);
		}
	}
	else if (!args.device)
	{
		if (args.serial_port)
		args.device = "/dev/ttyUSB0";
	}

	/* Validate ports. */
	if (!args.gdb_port || (args.mode == MODE_SOCKET &&
		!args.serial_port))
	{
		fprintf(stderr, "Invalid chosen ports, please select "
			"a valid port!\n");
		usage(argv[0]);
	}
}

/**
 * @brief Show program usage.
 * @param prgname Program name.
 */
void usage(const char *prgname)
{
	fprintf(stderr, "Usage: %s [options]\n", prgname);
	fprintf(stderr,
		"Options:\n"
		"  -s Enable serial through socket, instead of device\n"
		"  -d <path> Replaces the default device path (/dev/ttyUSB0)\n"
		"            (does not work if -s is enabled)\n"
		"  -p <port> Serial port (as socket), default: 2345\n"
		"  -g <port> GDB port, default: 1234\n"
		"  -h This help\n\n"
		"If no options are passed the default behavior is:\n"
		"  %s -d /dev/ttyUSB0 -g 1234\n\n"
		"Minimal recommended usages:\n"
		"  %s -s (socket mode, serial on 2345 and GDB on 1234)\n"
		"  %s    (device mode, serial on /dev/ttyUSB0 and GDB on 1234)\n",
		prgname, prgname, prgname);
	exit(EXIT_FAILURE);
}

/* Main =). */
int main(int argc, char **argv)
{
	struct handler_fd hfds[2] = {0};
	int ser_sv_fd, gdb_sv_fd;

	parse_args(argc, argv);

	/* Setup serial. */
	if (args.mode == MODE_SERIAL)
	{
		setup_serial(&ser_sv_fd, args.device);
		hfds[0].handler = handle_serial_msg;
	}
	else
	{
		setup_server(&ser_sv_fd, args.serial_port);
		hfds[0].handler = handle_accept_serial;
	}

	/* Setup GDB. */
	setup_server(&gdb_sv_fd, args.gdb_port);

	if (args.mode == MODE_SOCKET)
		printf("Please, conect your serial device first...\n");
	else
		printf("Please turn-on your debugged device and wait...\n");

	printf("(do not connect GDB yet!)\n");

	hfds[0].fd = ser_sv_fd;
	hfds[1].fd = gdb_sv_fd;
	hfds[1].handler = handle_accept_gdb;
	handle_fds(2, hfds);
	return (0);
}
