#ifndef NET_H
#define NET_H

	struct handler_fd
	{
		int fd;
		void (*handler)(struct handler_fd *fd); /* void func(int fd) */
	};

	extern ssize_t send_all(
		int conn, const void *buf, size_t len, int flags);
	extern void setup_server(int *srv_fd, uint16_t port);
	extern void setup_serial(int *sfd, const char *sdev);
	extern void close_handled_fd(int fd);
	extern void change_handled_fd(int fd_old,
		struct handler_fd *new_hfd);
	extern void handle_fds(int nfds, struct handler_fd *hfds);

#endif /* NET_H */
