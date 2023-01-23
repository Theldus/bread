#include "util.h"
#include "net.h"
#include "gdb.h"

/* Main =). */
int main(void)
{
	struct handler_fd hfds[2] = {0};
	int ser_sv_fd, gdb_sv_fd;

#ifdef USE_SERIAL
	setup_serial(&ser_sv_fd, "/dev/ttyUSB0");
#else
	setup_server(&ser_sv_fd, 2345);
#endif

	setup_server(&gdb_sv_fd, 1234);

	printf("Please, conect your serial device first...\n");

	hfds[0].fd = ser_sv_fd;
#ifdef USE_SERIAL
	hfds[0].handler = handle_serial_msg;
#else
	hfds[0].handler = handle_accept_serial;
#endif

	hfds[1].fd = gdb_sv_fd;
	hfds[1].handler = handle_accept_gdb;
	handle_fds(2, hfds);
	return (0);
}


#if 0
https://tatsuo.medium.com/implement-gdb-remote-debug-protocol-stub-from-scratch-3-e87a697ca48c

https://sourceware.org/gdb/current/onlinedocs/gdb/Overview.html#Overview


https://www.embecosm.com/appnotes/ean4/embecosm-howto-rsp-server-ean4-issue-2.html#id3079129



serial_socket
	serial_connect
		serial_send_regs
			=> gdb_socket
			=> gdb_connect

#endif
