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

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "net.h"
#include "util.h"

/*
 * Serial speed
 *
 * If anything goes wrong here, please lower this value,
 * like: B9600.
 *
 * Any changes here should also be reflected on
 * 'constants.inc' too.
 */
#define BAUD_RATE B115200

#define MAX_FDS 4

static int nfds;
static int started;
static struct pollfd     pfds[MAX_FDS];
static struct handler_fd hfds[MAX_FDS];

static int serial_fd;
static struct termios savetty;

/**
 * @brief Write @p len bytes from @p buf to @p conn.
 *
 * Contrary to send(2)/write(2) that might return with
 * less bytes written than specified, this function
 * attempts to write the entire buffer, because...
 * thats the most logical thing to do...
 *
 * @param conn Target file descriptor.
 * @param buf Buffer to be sent.
 * @param len Amount of bytes to be sent.
 *
 * @return Returns 0 if success, -1 otherwise.
 */
ssize_t send_all(
	int conn, const void *buf, size_t len)
{
	const char *p;
	ssize_t ret;

	if (conn < 0)
		return (-1);

	p = buf;
	while (len)
	{
		ret = write(conn, p, len);
		if (ret == -1)
			return (-1);
		p += ret;
		len -= ret;
	}
	return (0);
}

/**
 * @brief Configure a TCP server to listen to the
 * specified port @p port.
 *
 * @param srv_fd Returned server fd.
 * @param port Port to listen.
 */
void setup_server(int *srv_fd, uint16_t port)
{
	struct sockaddr_in server;
	int reuse = 1;

	*srv_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (srv_fd < 0)
		errx("Unable to open socket!\n");

	setsockopt(*srv_fd, SOL_SOCKET, SO_REUSEADDR,
		(const char *)&reuse, sizeof(reuse));

	/* Prepare the sockaddr_in structure. */
	memset((void*)&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY;
	server.sin_port = htons(port);

	/* Bind. */
	if (bind(*srv_fd, (struct sockaddr *)&server, sizeof(server)) < 0)
		errx("Bind failed");

	/* Listen. */
	listen(*srv_fd, 1);
}

/* Restore the tty/device while exiting. */
static void restore_tty(void) {
	tcsetattr(serial_fd, TCSANOW, &savetty);
}

/**
 * @brief Initial setup for the serial device.
 *
 * @param sfd Returned serial device fd.
 * @param sdev Serial device path, like: /dev/ttyUSB0
 */
void setup_serial(int *sfd, const char *sdev)
{
	speed_t spd;
	struct termios tty;

	/* Open device. */
	if ((*sfd = open(sdev, O_RDWR | O_NOCTTY)) < 0)
		errx("Failed to open: %s, (%s)", sdev, strerror(errno));

	/* Attributes. */
	if (tcgetattr(*sfd, &tty) < 0)
		errx("Failed to get attr: (%s)", strerror(errno));

	savetty = tty;
	cfsetospeed(&tty, (speed_t)BAUD_RATE);
	cfsetispeed(&tty, (speed_t)BAUD_RATE);
	cfmakeraw(&tty);

	/* TTY settings. */
	tty.c_cc[VMIN]  = 1;
	tty.c_cc[VTIME] = 10;
	tty.c_cflag &= ~CSTOPB;
	tty.c_cflag &= ~CRTSCTS; /* no HW flow control? */
	tty.c_cflag |= CLOCAL | CREAD;

	if (tcsetattr(*sfd, TCSANOW, &tty) < 0)
		errx("Failed to set attr: (%s)", strerror(errno));

	/* Restore tty. */
	atexit(restore_tty);
}

/**
 * @brief Check for errors on a given pollfd @p p.
 *
 * @return Returns 0 if no errors were found, 1
 * otherwise.
 */
static inline int events_error(struct pollfd *p)
{
	int ev;
	int i;

	for (i = 0; i < nfds; i++, p++)
	{
		ev = p->events;
		if ((ev & POLLHUP) ||
			(ev & POLLERR) ||
			(ev & POLLNVAL))
		{
			return (1);
		}
	}
	return (0);
}

/**
 * @brief Change a given fd to another.
 *
 * This routine is required due to how the communication
 * works: when a connection is accepted, the server fd
 * is no longer needed, so it should be closed and
 * replaced with the fd of the accepted connection.
 *
 * @param fd_old Old fd to be closed and replaced.
 * @param new_hfd Handler_fd structure containing the new
 *                fd and handler routine.
 */
void change_handled_fd(int fd_old, struct handler_fd *new_hfd)
{
	int i;

	for (i = 0; i < nfds; i++)
		if (pfds[i].fd == fd_old)
			break;

	if (i == nfds)
		errx("FD: %d not found! error\n", fd_old);

	/* Close old connection, since its no longer needed. */
	close(fd_old);

	/* Configure our new handler and fd to the same slot. */
	pfds[i].fd      = new_hfd->fd;
	pfds[i].events  = POLLIN;
	hfds[i].fd      = new_hfd->fd;
	hfds[i].handler = new_hfd->handler;
}

/**
 * @brief Fill initial value to the fd list.
 */
static void init_handle_fds(void)
{
	int i;
	for (i = 0; i < MAX_FDS; i++)
		pfds[i].fd = -1;
	started = 1;
}

/**
 * @brief Polls any changes in the fd list and
 * call the respective handler.
 *
 * All the communication is done here: whenever
 * there is something to handle (like serial or
 * GDB), this routine calls the appropriate
 * handler.
 *
 * That way, there's no need to use threads nor
 * any more elaborate mechanisms.
 *
 * @param n Number of fds to handle.
 * @param hfd_list Handler_fd list.
 */
void handle_fds(int n, struct handler_fd *hfd_list)
{
	int i, j;

	if (!started)
		init_handle_fds();

	if (n + nfds >= MAX_FDS || !hfd_list)
		errx("Invalid number of fds!\n");

	/* For each new to-be-added fd. */
	for (i = 0; i < n; i++)
	{
		/* Find an empty slot. */
		for (j = 0; j < MAX_FDS; j++)
		{
			if (pfds[j].fd < 0)
			{
				pfds[j].fd      = hfd_list[i].fd;
				pfds[j].events  = POLLIN;
				hfds[j].fd      = hfd_list[i].fd;
				hfds[j].handler = hfd_list[i].handler;
				nfds++;
				break;
			}
		}
	}

	/* Handle events. */
	while (poll(pfds, nfds, -1) != -1)
	{
		if (events_error(pfds))
			break;

		for (i = 0; i < nfds; i++)
			if (pfds[i].revents & POLLIN)
				hfds[i].handler(&hfds[i]);
	}
}
