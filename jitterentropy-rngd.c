/*
 * Non-physical true random number generator based on timing jitter.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * the GNU General Public License, in which case the provisions of the GPL are
 * required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#include <asm/types.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#define _GNU_SOURCE
#include <getopt.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/random.h>
#include <signal.h>

#include "jitterentropy.h"

static int Verbosity = 0;

struct kernel_rng {
	int fd;
	struct rand_data *ec;
	struct rand_pool_info *rpi;
	const char *dev;
};

static struct kernel_rng Random = {
	.fd = 0,
	.ec = NULL,
	.rpi = NULL,
	.dev = "/dev/random"
};

/*
 * handler for /dev/urandom not needed as used IOCTL alters input_pool
static struct kernel_rng Urandom = {
	.fd = 0,
	.ec = NULL,
	.rpi = NULL,
	.dev = "/dev/urandom"
};
*/

static int Pidfile_fd = 0;
/* "/var/run/jitterentropy-rngd.pid" */
static char *Pidfile = NULL;

static int Entropy_avail_fd = 0;

#define RNDBYTES 256
#define ENTROPYTRHESH 1024
#define ENTROPYAVAIL "/proc/sys/kernel/random/entropy_avail"

static void install_alarm(void);
static void dealloc(void);
static void dealloc_rng(struct kernel_rng *rng);

static void usage(void)
{
	fprintf(stderr, "\njitterentropy rngd feeding entropy to input_pool of Linux RNG\n\n");
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "\t-v\tVerbose logging, multiple options increase verbosity\n");
	fprintf(stderr, "\t\tVerbose logging implies running in foreground\n");
	fprintf(stderr, "\t-p\tWrite daemon PID to file\n");
	exit(1);
}

static void parse_opts(int argc, char *argv[])
{
	int c = 0;
	while (1) {
		int opt_index = 0;
		static struct option opts[] = {
			{"verbose", 0, 0, 0},
			{"pid", 1, 0, 0},
			{0, 0, 0, 0}
		};
		c = getopt_long(argc, argv, "vp:", opts, &opt_index);
		if (-1 == c)
			break;
		switch (c) {
		case 'v':
			Verbosity++;
			break;
		case 'p':
			Pidfile = optarg;
			break;
		default:
			usage();
		}
	}
}

#define LOG_DEBUG	3
#define LOG_VERBOSE	2
#define LOG_WARN	1
#define LOG_ERR		0
static void dolog(int severity, const char *fmt, ...)
{
	va_list args;
	char msg[1024];
	char sev[10];

	if (severity <= Verbosity) {
		va_start(args, fmt);
		vsnprintf(msg, sizeof(msg), fmt, args);
		va_end(args);

		switch (severity) {
		case LOG_DEBUG:
			snprintf(sev, sizeof(sev), "Debug");
			break;
		case LOG_VERBOSE:
			snprintf(sev, sizeof(sev), "Verbose");
			break;
		case LOG_WARN:
			snprintf(sev, sizeof(sev), "Warning");
			break;
		case LOG_ERR:
			snprintf(sev, sizeof(sev), "Error");
			break;
		default:
			snprintf(sev, sizeof(sev), "Unknown");
		}
		printf("jitterentropy-rngd - %s: %s\n", sev, msg);
	}

	if (LOG_ERR == severity) {
		dealloc();
		exit(1);
	}
}

/*******************************************************************
 * entropy handler functions
 *******************************************************************/

static size_t write_random(struct kernel_rng *rng, char *buf, size_t len)
{
	size_t written = 0;
	rng->rpi->entropy_count = (RNDBYTES * 8); /* value is in bits */
	rng->rpi->buf_size = RNDBYTES;
	memcpy(rng->rpi->buf, buf, RNDBYTES);
	memset(buf, 0, RNDBYTES);

	if (-1 == ioctl(rng->fd, RNDADDENTROPY, rng->rpi))
		dolog(LOG_WARN, "Error injecting entropy: %s", strerror(errno));
	else {
		dolog(LOG_DEBUG, "Injected %d bytes of entropy", RNDBYTES);
		written = RNDBYTES;
	}

	rng->rpi->entropy_count = 0;
	rng->rpi->buf_size = 0;
	memset(rng->rpi->buf, 0, RNDBYTES);

	return written;
}

static size_t gather_entropy(struct kernel_rng *rng)
{
	char buf[RNDBYTES];
	size_t ret = 0;

	if (0 > jent_read_entropy(rng->ec, buf, RNDBYTES)) {
		dolog(LOG_WARN, "Cannot read entropy");
		return 0;
	}
	ret = write_random(rng, buf, RNDBYTES);
	if (RNDBYTES != ret)
		dolog(LOG_WARN, "Injected %lu bytes into %s, expected %d",
			ret, rng->dev, RNDBYTES);
	memset(buf, 0, RNDBYTES);

	return RNDBYTES;
}

static int read_entropy_avail(int fd)
{
	ssize_t data = 0;
	char buf[5];
	int entropy = 0;

	data = read(fd, buf, sizeof(buf));
	lseek(fd, 0, SEEK_SET);

	if (0 > data) {
		dolog(LOG_WARN, "Error reading data from entropy_avail: %s", strerror(errno));
		return 0;
	}
	if (0 == data) {
		dolog(LOG_WARN, "Could not read data from entropy_avail");
		return 0;
	}

	entropy = atoi(buf);
	if (0 > entropy || 4096 < entropy) {
		dolog(LOG_WARN, "Entropy read from entropy_avail (%d) is outsize of range", entropy);
		return 0;
	}

	return entropy;
}

/*******************************************************************
 * Signal handling functions
 *******************************************************************/

/*
 * Wakeup and check entropy_avail -- this covers the drain of entropy
 * from the nonblocking_pool via get_random_bytes
 */
static void sig_entropy_avail(int sig)
{
	int entropy = 0;
	size_t written = 0;

	dolog(LOG_VERBOSE, "Wakeup call for alarm on %s", ENTROPYAVAIL);
	entropy = read_entropy_avail(Entropy_avail_fd);

	if (0 == entropy)
		goto out;
	if (ENTROPYTRHESH < entropy) {
		dolog(LOG_DEBUG, "Sufficient entropy %d available", entropy);
		goto out;
	}
	dolog(LOG_DEBUG, "Insufficient entropy %d available", entropy);
	written = gather_entropy(&Random);
	dolog(LOG_VERBOSE, "%lu bytes written to /dev/random", written);
out:
	install_alarm();
	return;
}

/* terminate the daemon cleanly */
static void sig_term(int sig)
{
	dolog(LOG_DEBUG, "Shutting down cleanly\n");
	dealloc();
	exit(0);
}

/*
 * Wakeup on insufficient entropy on /dev/random
 */
static void select_fd(void)
{
	fd_set fds;
	int ret = 0;
	size_t written = 0;

	while (1) {
		FD_ZERO(&fds);
		dolog(LOG_DEBUG, "Polling /dev/random");
		FD_SET(Random.fd, &fds);
		/* only /dev/random implements polling */
		ret = select((Random.fd + 1), NULL, &fds, NULL, NULL);

		if (-1 == ret && EINTR != errno)
			dolog(LOG_ERR, "Select returned with error %s", strerror(errno));
		if (0 <= ret) {
			dolog(LOG_VERBOSE, "Wakeup call for select on /dev/random");
			written = gather_entropy(&Random);
			dolog(LOG_VERBOSE, "%lu bytes written to /dev/random", written);
		}
	}
}

static void install_alarm(void)
{
	dolog(LOG_DEBUG, "Install alarm signal handler");
	signal(SIGALRM, sig_entropy_avail);
	alarm(5);
}

static void install_term(void)
{
	dolog(LOG_DEBUG, "Install termination signal handler");
	signal(SIGHUP, sig_term);
	signal(SIGINT, sig_term);
	signal(SIGQUIT, sig_term);
	signal(SIGTERM, sig_term);
}

/*******************************************************************
 * allocation functions
 *******************************************************************/

static void alloc_rng(struct kernel_rng *rng)
{
	rng->ec = jent_entropy_collector_alloc(1, 0);
	if (!rng->ec)
		dolog(LOG_ERR, "Allocation of entropy collector failed");

	rng->rpi = malloc((sizeof(struct rand_pool_info) +
			  (RNDBYTES * sizeof(char))));
	if (!rng->rpi)
		dolog(LOG_ERR, "Cannot allocate memory for random bytes");

	rng->fd = open(rng->dev, O_WRONLY);
	if (-1 == rng->fd)
		dolog(LOG_ERR, "Open of %s failed: %s", rng->dev, strerror(errno));
}

static void alloc(void)
{
	int ret = 0;
	size_t written = 0;

	ret = jent_entropy_init();
	if (ret)
		dolog(LOG_ERR, "The initialization of CPU Jitter RNG failed with error code %d\n", ret);

	alloc_rng(&Random);

	Entropy_avail_fd = open(ENTROPYAVAIL, O_RDONLY);
	if (-1 == Entropy_avail_fd)
		dolog(LOG_ERR, "Open of %s failed: %s", ENTROPYAVAIL, strerror(errno));

	written = gather_entropy(&Random);
	dolog(LOG_VERBOSE, "%lu bytes written to /dev/random", written);
}

static void dealloc_rng(struct kernel_rng *rng)
{
	if (NULL != rng->ec) {
		jent_entropy_collector_free(rng->ec);
		rng->ec = NULL;
	}
	if (NULL != rng->rpi) {
		memset(rng->rpi, 0,(sizeof(struct rand_pool_info) +
				    (RNDBYTES * sizeof(char))));
		free(rng->rpi);
		rng->rpi = NULL;
	}
	if (0 != rng->fd) {
		close(rng->fd);
		rng->fd = 0;
	}
}

static void dealloc(void)
{
	dealloc_rng(&Random);
	if(0 != Entropy_avail_fd) {
		close(Entropy_avail_fd);
		Entropy_avail_fd = 0;
	}

	if (0 != Pidfile_fd) {
		close(Pidfile_fd);
		Pidfile_fd = 0;
		if (NULL != Pidfile)
			unlink(Pidfile);
	}

	
}

static void create_pid_file(const char *pid_file)
{
	char pid_str[12];	/* max. integer length + '\n' + null */

	/* Ensure only one copy */
	Pidfile_fd = open(pid_file, O_RDWR|O_CREAT|O_EXCL, S_IRUSR|S_IWUSR);
	if (Pidfile_fd == -1)
		dolog(LOG_ERR, "Cannot open pid file\n");

	if (lockf(Pidfile_fd, F_TLOCK, 0) == -1) {
		if (errno == EAGAIN || errno == EACCES) {
			dolog(LOG_ERR, "PID file already locked\n");
			exit(1);
		} else
			dolog(LOG_ERR, "Cannot lock pid file\n");
	}

	if (ftruncate(Pidfile_fd, 0) == -1) {
		dolog(LOG_ERR, "Cannot truncate pid file\n");
		exit(1);
	}

	/* write our pid to the pid file */
	snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
	if (write(Pidfile_fd, pid_str, strlen(pid_str)) != strlen(pid_str)) {
		dolog(LOG_ERR, "Cannot write to pid file\n");
		exit(1);
	}
}


static void daemonize(void)
{
	pid_t pid;
	
	/* already a daemon */
	if (1 == getppid())
	       return;

	pid = fork();
	if (pid < 0)
		dolog(LOG_ERR, "Cannot fork to daemonize\n");

	/* the parent process exits -- nothing has been allocated, nothing
	 * needs to be freed */
	if (0 < pid)
            exit(0);

	/* we are the child now */

	/* new SID for the child process */
	if (setsid() < 0)
		dolog(LOG_ERR, "Cannot obtain new SID for child\n");

	/* Change the current working directory.  This prevents the current
	 * directory from being locked; hence not being able to remove it. */
	if ((chdir("/")) < 0)
		dolog(LOG_ERR, "Cannot change directory\n");
	
	if (Pidfile && strlen(Pidfile))
		create_pid_file(Pidfile);

	/* Redirect standard files to /dev/null */
	freopen( "/dev/null", "r", stdin);
	freopen( "/dev/null", "w", stdout);
	freopen( "/dev/null", "w", stderr);
}


int main(int argc, char *argv[])
{
	if (geteuid())
		dolog(LOG_ERR, "Program must start as root!");

	parse_opts(argc, argv);
	if (0 == Verbosity)
		daemonize();
	alloc();
	install_term();
	install_alarm();
	select_fd();
	/* NOTREACHED */
	dealloc();
	return 0;
}
