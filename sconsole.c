/* sconsole - cheap serial console (for xterm, etc)
 *
 * Copyright (c) 2005-2010
 *	Brian Swetland.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the authors nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <termios.h>
#include <signal.h>
#include <poll.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/types.h>

static struct termios tio_save;

static void stdin_raw_init(void)
{
	struct termios tio;

	if (tcgetattr(0, &tio))
		return;
	if (tcgetattr(0, &tio_save))
		return;

	/* disable CANON, ECHO*, etc */
	tio.c_lflag = 0;

	/* no timeout but request at least one character per read */
	tio.c_cc[VTIME] = 0;
	tio.c_cc[VMIN] = 1;

	tcsetattr(0, TCSANOW, &tio);
	tcflush(0, TCIFLUSH);
}

static void stdin_raw_restore(void)
{
	tcsetattr(0, TCSANOW, &tio_save);
	tcflush(0, TCIFLUSH);
}

void oops(int x)
{
	char *msg = "\n[ killed by signal ]\n";
	write(2, msg, strlen(msg));
	stdin_raw_restore();
	exit(1);
}

int text2speed(const char *s)
{
	int n = atoi(s);
	switch (n) {
	case 115200:
		return B115200;
	case 38400:
		return B38400;
	case 19200:
		return B19200;
	case 9600:
		return B9600;
	default:
		return B115200;
	}
}

int openserial(const char *device, int speed)
{
	struct termios tio;
	int fd;
	fd = open(device, O_RDWR | O_NOCTTY);

	if (fd < 0)
		return -1;

	if (tcgetattr(fd, &tio))
		memset(&tio, 0, sizeof(tio));

	tio.c_cflag = B57600 | CS8 | CLOCAL | CREAD;
	tio.c_iflag = IGNPAR;
	tio.c_lflag = 0; /* turn of CANON, ECHO*, etc */
	tio.c_cc[VTIME] = 0;
	tio.c_cc[VMIN] = 1;
	tcsetattr(fd, TCSANOW, &tio);
	tcflush(fd, TCIFLUSH);

	tio.c_cflag = speed | CS8 | CLOCAL | CREAD;
	tcsetattr(fd, TCSANOW, &tio);
	tcflush(fd, TCIFLUSH);
	return fd;
}

static unsigned char valid[256];

#define STATE_IDLE    0
#define STATE_PREFIX  1
#define STATE_COMMAND 2

void usage(char *prog_name)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "%s [-t] [-l[<logfile>]] [-d <tty device>] [-b <baud rate>] [-h]\n", prog_name);
	fprintf(stderr, "\t-t: transparent mode\n");
	fprintf(stderr, "\t-l: log the output to <logfile>.  If <logfile> is not specified, the default log file name is \"console.log\"\n");
	fprintf(stderr, "\t-d <tty device>: change to open <tty device> (the default is \"/dev/ttyUSB0\")\n");
	fprintf(stderr, "\t-b <baud rate>: change to set baud rate as <baud rate> (the default is \"115200\")\n");
	fprintf(stderr, "\t-h: this help page\n");
	fprintf(stderr, "example: %s -lconsole.log -d /dev/ttyUSB0 -b 115200\n", prog_name);
	fprintf(stderr, "\nNOTE: <ESC>-<ESC>-x to exit %s\n", prog_name);
	fprintf(stderr, "\n");
}

int main(int argc, char *argv[])
{
	struct pollfd fds[2];
	int speed = B115200; /* default baud rate */
	const char *device = "/dev/ttyUSB0"; /* the default tty device to open */
	const char *logfile = "console.log"; /* the default log file name */
	int fd, n;
	int escape = 0;
	int logfd = -1;
	unsigned char ESC = 27;
	int ch;

	for (n = ' '; n < 127; n++)
		valid[n] = 1;

	valid[8] = 1; /* backspace */
	valid[9] = 1; /* tab */
	valid[10] = 1; /* newline */
	valid[13] = 1; /* carriage return */

	while ((ch = getopt(argc, argv, "tl::d:b:h")) != -1) {
		switch (ch) {
		case 't':
			/* transparent mode */
			for (n = 0; n < 256; n++)
				valid[n] = 1;
			break;
		case 'l':
			/* log */
			if (optarg) {
				/* log file name is specified */
				logfile = optarg;
			}
			logfd = open(logfile, O_CREAT | O_WRONLY, 0644);
			break;
		case 'd':
			device = optarg;
			break;
		case 'b':
			speed = text2speed(optarg);
			fprintf(stderr, "SPEED: %s %08x\n", optarg, speed);
			break;
		default:
			fprintf(stderr, "unknown option %s\n", argv[1]);
		case 'h':
			usage(argv[0]);
			return 1;
		}
	}

	fd = openserial(device, speed);
	if (fd < 0) {
		fprintf(stderr, "stderr open '%s'\n", device);
		return -1;
	}

	stdin_raw_init();
	signal(SIGINT, oops);

	fds[0].fd = 0;
	fds[0].events = POLLIN;

	fds[1].fd = fd;
	fds[1].events = POLLIN;

	fprintf(stderr, "[ %s ]\n", device);

	for (;;) {
		unsigned char x, t;

		if (poll(fds, 2, -1) > 0) {
			if (fds[0].revents & (POLLERR | POLLHUP)) {
				fprintf(stderr, "\n[ stdin port closed ]\n");
				break;
			}
			if (fds[1].revents & (POLLERR | POLLHUP)) {
				fprintf(stderr, "\n[ serial port closed ]\n");
				break;
			}
			if ((fds[0].revents & POLLIN) && (read(0, &x, 1) == 1)) {
				switch (escape) {
				case 0:
					if (x == 27) {
						escape = 1;
					} else {
						write(fd, &x, 1);
					}
					break;
				case 1:
					if (x == 27) {
						escape = 2;
						fprintf(stderr, "\n[ (b)reak? e(x)it? ]\n");
					} else {
						escape = 0;
						write(fd, &ESC, 1);
						write(fd, &x, 1);
					}
					break;
				case 2:
					escape = 0;
					switch (x) {
					case 27:
						write(fd, &x, 1);
						break;
					case 'b':
						fprintf(stderr, "[ break ]\n");
						tcsendbreak(fd, 0);
						break;
					case 'x':
						fprintf(stderr, "[ exit ]\n");
						goto done;
					default:
						fprintf(stderr, "[ huh? ]\n");
						break;
					}
					break;
				}
			}
			if ((fds[1].revents & POLLIN) && (read(fd, &x, 1) == 1)) {
				if (!valid[x])
					x = '.';
				write(1, &x, 1);
				if (logfd != -1)
					write(logfd, &x, 1);
			}
		}
	}

done:
	stdin_raw_restore();
	return 0;
}
