#ifndef GDB_H
#define GDB_H

	struct handler_fd;
	extern void handle_gdb_msg(struct handler_fd *hfd);
	extern void handle_serial_msg(struct handler_fd *hfd);
	extern char *encode_hex(const char *data, size_t len);
	extern void handle_accept_gdb(struct handler_fd *hfd);
	extern void handle_accept_serial(struct handler_fd *hfd);

#endif /* GDH_H */
