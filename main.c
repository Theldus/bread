#include "util.h"
#include "net.h"
#include "gdb.h"

/* Main =). */
int main(void)
{
	struct handler_fd hfds[2];
	int ser_sv_fd, gdb_sv_fd;

	setup_server(&ser_sv_fd, 2345);
	setup_server(&gdb_sv_fd, 1234);
	printf("Please, conect your serial device first...\n");

	hfds[0].fd = ser_sv_fd;
	hfds[0].handler = handle_accept_serial;
	hfds[1].fd = gdb_sv_fd;
	hfds[1].handler = handle_accept_gdb;

	handle_fds(2, hfds);

#if 0
maybe passar so 2 fds: sv_serial e sv_gdb, qnd esses 2
biscoitarem, fechar a conec deles e invocar uma funcao
q add os novos fds ao handle_fds (e rmove os antigos)
e dai handla normalm


add handler do gdb, nele, checar se o serial ja ta
ativo e com os regs, e se n,emitir erro abortando
td, se s, aceitar de bom grado e trocar os fds
#endif


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
