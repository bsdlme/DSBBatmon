/*-
 * Copyright (c) 2017 Marcel Kaiser. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>
#include <paths.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/param.h>
#include <sys/linker.h>
#include <sys/pciio.h>

#include "dsbbatmon.h"

#define DEVD_SUBS_ACAD	     1
#define DEVD_SUBS_CMBAT	     2

#define SOCK_ERR_IO_ERROR    2
#define SOCK_ERR_CONN_CLOSED 1

#define FATAL_SYSERR	     (DSBBATMON_ERR_SYS|DSBBATMON_ERR_FATAL)

#define ERROR(bm, ret, error, prepend, fmt, ...) do { \
	set_error(bm, error, prepend, fmt, ##__VA_ARGS__); \
	return (ret); \
} while (0)

static int   devd_connect(void);
static int   uconnect(const char *path);
static int   poll_acpi(dsbbatmon_t *bm);
static int   init_acpi(dsbbatmon_t *bm);
static bool  parse_devd_event(char *str);
static char *read_devd_event(dsbbatmon_t *bm, int *error);

int
dsbbatmon_update(dsbbatmon_t *bm)
{
	if (poll_acpi(bm) == -1) {
		if (get_units(bm) == -1)
			ERROR(bm, -1, 0, true, "get_units()");
		return (0);
	}
	if (bm->units < 1) {
		if (get_units(bm) == -1)
			ERROR(bm, -1, 0, true, "get_units()");
	}
	return (0);
}

int
dsbbatmon_init(dsbbatmon_t *bm)
{
	bm->lnbuf = NULL;
	bm->rd = bm->slen = bm->bufsz = 0;

	if (init_acpi(bm) == -1)
		return (-1);
	if ((bm->socket = devd_connect()) == -1) 
		ERROR(bm, -1, 0, true, "devd_connect()");
	return (0);
}

int
dsbbatmon_check_for_batt_event(dsbbatmon_t *bm)
{
	int   error;
	char *ln;

	while ((ln = read_devd_event(bm, &error)) != NULL) {
		if (is_batt_event(ln))
			return (1);
	}
	if (error == SOCK_ERR_CONN_CLOSED) {
		/* Lost connection to devd. */
		(void)close(bm->socket);
		warnx("Lost connection to devd. Reconnecting ...");
		if ((bm->socket = devd_connect()) == -1) {
			ERROR(bm, -1, 0, false,
			    "Connecting to devd failed. Giving up.");
		}
	} else if (error == SOCK_ERR_IO_ERROR)
		ERROR(bm, -1, FATAL_SYSERR, false, "read_devd_event()");
	return (0);
}

const char *
dsbbatmon_strerror(dsbbatmon_t *bm)
{
	return (bm->errmsg);
}

static void
set_error(dsbbatmon_t *bm, int error, bool prepend, const char *fmt, ...)
{
	int	_errno;
	char	errbuf[DSBBATMON_ERRBUF_SZ];
	size_t  len;
	va_list ap;

	_errno = errno;

	va_start(ap, fmt);
	if (prepend) {
		if (error & DSBBATMON_ERR_FATAL) {
			if (strncmp(bm->errmsg, "Fatal: ", 7) == 0) {
				(void)memmove(bm->errmsg, bm->errmsg + 7,
				    strlen(bm->errmsg) - 6);
			}
			(void)strncpy(errbuf, "Fatal: ", sizeof(errbuf) - 1);
			len = strlen(errbuf);
		} else
			len = 0;
		(void)vsnprintf(errbuf + len, sizeof(errbuf) - len, fmt, ap);

		len = strlen(errbuf);
		(void)snprintf(errbuf + len, sizeof(errbuf) - len, ":%s",
		    bm->errmsg);
		(void)strcpy(bm->errmsg, errbuf);
	} else {
		(void)vsnprintf(bm->errmsg, sizeof(bm->errmsg), fmt, ap);
		if (error == DSBBATMON_ERR_FATAL) {
			(void)snprintf(errbuf, sizeof(errbuf), "Fatal: %s",
			    bm->errmsg);
			(void)strcpy(bm->errmsg, errbuf);
		}
		
	}
	if ((error & DSBBATMON_ERR_SYS) && _errno != 0) {
		len = strlen(bm->errmsg);
		(void)snprintf(bm->errmsg + len, sizeof(bm->errmsg) - len,
		    ":%s", strerror(_errno));
		errno = 0;
	}
}

#ifdef TEST
static int
get_units(dsbbatmon_t *bm)
{
	int   c;
	char  n[2];
	FILE *fp;

	if ((fp = fopen(PATH_TEST_UNIT_FILE, "r")) == NULL) {
		err("fopen(%s)", PATH_TEST_UNIT_FILE);
	}
	if ((c = fgetc(fp)) == EOF) {
		if (ferror(fp))
			err(EXIT_FAILURE, "fgetc()");
	} else {
		n[0] = c; n[1] = '\0';
		bm->units = strtol(n, NULL, 10);
	}
	(void)fclose(fp);

	return (bm->units);
}
#else
static int
get_units(dsbbatmon_t *bm)
{
	int    units;
	size_t sz;

	bm->units = 0; sz = sizeof(int);
	if (sysctlbyname("hw.acpi.battery.units", &units, &sz, NULL, 0) != 0) {
		if (errno == ENOENT)
			return (0);
		ERROR(bm, -1, FATAL_SYSERR,
		    "sysctlbyname(hw.acpi.battery.units)");
	}
	bm->units = units;

	return (units);
}
#endif	/* TEST */

int
dsbbatmon_wait_for_batt_event()
{
	int     error;
	char   *ln;
	fd_set  rset;

	for (;;) {
		FD_ZERO(&rset); FD_SET(bm->socket, &rset);
		while (select(bm->socket + 1, &rset, NULL, NULL, NULL) == -1) {
			if (errno == EINTR)
				continue;
			else
				ERROR(bm, -1, FATAL_SYSERR, false, "select()");
		}
		if (!FD_ISSET(bm->socket, &rset))
			continue;
		while ((ln = read_devd_event(bm, &error)) != NULL) {
			if (!is_batt_event(ln))
				continue;
			return (poll_acpi(bm));
		}
		if (error == SOCK_ERR_CONN_CLOSED) {
			/* Lost connection to devd. */
			(void)close(bm->socket);
			warnx("Lost connection to devd. Reconnecting ...");
			if ((bm->socket = devd_connect()) == -1) {
				ERROR(bm, -1, 0, false,
				    "Connecting to devd failed. Giving up.");
			}
		} else if (error == SOCK_ERR_IO_ERROR) {
			ERROR(bm, -1, FATAL_SYSERR, false,
			    "read_devd_event()");
		}
	}
	/* NOTREACHED */
	return (-1);
}

#ifdef TEST
char *
read_cmd()
{
	static int  init = 1;
	static char buf[128];

	if (init) {
		setvbuf(stdin, (char *)NULL, _IONBF, 0);
		if (fcntl(fileno(stdin), F_SETFL, fcntl(fileno(stdin),
		    F_GETFL) | O_NONBLOCK) == -1) {
			err(EXIT_FAILURE, "fcntl()");
		}
		init = 0;
	}
	if (fgets(buf, sizeof(buf) - 1, stdin) != NULL)
		return (buf);
	return (NULL);
}
#endif

static int
init_acpi(dsbbatmon_t *bm)
{
#ifdef TEST
	return (poll_acpi(bm));
#endif
	if ((bm->acpi.acpifd = open(ACPIDEV, O_RDONLY)) == -1)
		ERROR(bm, -1, FATAL_SYSERR, false, "open(%s)", ACPIDEV);
	return (dsbbatmon_update(bm));
}

static int
poll_acpi(dsbbatmon_t *bm)
{
	int	     ac;
	static union acpi_battery_ioctl_arg battio;
#ifdef TEST
	static time_t t0 = 0;
	char *p;
	int d = rand() % 5;
	if (bm->units < 1)
		return (-1);
	if (t0 == 0) {
		t0 = time(NULL);
		bm->acpi.cap = 60;
		bm->acpi.status = ACPI_STATUS_DISCHARGING;
	}
	if ((p = read_cmd()) != NULL) {
		switch (p[0]) {
		case 'a':
			bm->acpi.status = ACPI_STATUS_ACLINE;
			break;
		case 'c':
			bm->acpi.status = ACPI_STATUS_CHARGING;
			break;
		case 'd':
			bm->acpi.status = ACPI_STATUS_DISCHARGING;
		}
	}
	if (time(NULL) - t0 >= 10) {
		if (bm->acpi.status == ACPI_STATUS_DISCHARGING)
			bm->acpi.cap -= d;
		else if (bm->acpi.status == ACPI_STATUS_CHARGING)
			bm->acpi.cap += d;
		else if (bm->acpi.status == ACPI_STATUS_ACLINE)
			return (0);
		if (bm->acpi.cap <= 0)
			bm->acpi.cap = 0;
		else if (bm->acpi.cap >= 100) {
			bm->acpi.cap = 100;
			bm->acpi.status = ACPI_STATUS_ACLINE;
		}
		t0 = time(NULL);
	}
	bm->acpi.min = 100;

	return (0);
#endif
	battio.unit = 0;
	if (ioctl(bm->acpi.acpifd, ACPIIO_BATT_GET_BATTINFO, &battio) == -1) {
		ERROR(bm, -1, FATAL_SYSERR, false,
		    "ioctl(ACPIIO_BATT_GET_BATTINFO)");
	}
	if (ioctl(bm->acpi.acpifd, ACPIIO_ACAD_GET_STATUS, &ac) == -1) {
		ERROR(bm, -1, FATAL_SYSERR, false,
		    "ioctl(ACPIIO_ACAD_GET_STATUS)");
	}
	bm->acpi.cap = battio.battinfo.cap;
	bm->acpi.min = battio.battinfo.min;

	if (battio.battinfo.state & ACPI_BATT_STAT_DISCHARG)
		bm->acpi.status = ACPI_STATUS_DISCHARGING;
	else if (battio.battinfo.state & ACPI_BATT_STAT_CHARGING)
		bm->acpi.status = ACPI_STATUS_CHARGING;
	else if (ac > 0)
		bm->acpi.status = ACPI_STATUS_ACLINE;
	else
		bm->acpi.status = ACPI_STATUS_UNKNOWN;
	return (0);
}

static char *
read_devd_event(dsbbatmon_t *bm, int *error)
{
	int  i, n, s;

	dsbbatmon_t *p = bm;

	if (p->lnbuf == NULL) {
		if ((p->lnbuf = malloc(_POSIX2_LINE_MAX)) == NULL)
			return (NULL);
		p->bufsz = _POSIX2_LINE_MAX;
	}
	errno = *error = n = 0; s = p->socket;
	do {
		p->rd += n;
		if (p->slen > 0) {
			(void)memmove(p->lnbuf, p->lnbuf + p->slen,
			    p->rd - p->slen);
		}
		p->rd  -= p->slen;
		p->slen = 0;
		for (i = 0; i < p->rd && p->lnbuf[i] != '\n'; i++)
			;
		if (i < p->rd) {
			p->lnbuf[i] = '\0'; p->slen = i + 1;
			if (p->slen == p->bufsz)
				p->slen = p->rd = 0;
			return (p->lnbuf);
		}
		if (p->rd == p->bufsz - 1) {
			p->lnbuf = realloc(p->lnbuf, p->bufsz +
			    _POSIX2_LINE_MAX);
			if (p->lnbuf == NULL)
				err(EXIT_FAILURE, "realloc()");
			p->bufsz += 64;
		}

	} while ((n = read(s, p->lnbuf + p->rd, p->bufsz - p->rd - 1)) > 0);

	if (n < 0) {
		if (errno == EAGAIN || errno == EINTR) {
			/* Just retry */
			return (NULL);
		} else
			*error = SOCK_ERR_IO_ERROR;
	} else if (errno == 0 || errno == ECONNRESET)
		*error = SOCK_ERR_CONN_CLOSED;
	/* Error or lost connection */
	p->slen = p->rd = 0;

	return (NULL);
}

static int
uconnect(const char *path)
{
	int s;
	struct sockaddr_un saddr;

	if ((s = socket(PF_LOCAL, SOCK_STREAM, 0)) == -1)
		return (-1);
	(void)memset(&saddr, (unsigned char)0, sizeof(saddr));
	(void)snprintf(saddr.sun_path, sizeof(saddr.sun_path), "%s", path);
	saddr.sun_family = AF_LOCAL;
	if (connect(s, (struct sockaddr *)&saddr, sizeof(saddr)) == -1)
		return (-1);
	if (fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK) == -1)
		return (-1);
	return (s);
}

static int
devd_connect()
{
	int  i, s;

	for (i = 0, s = -1; i < 30 && s == -1; i++) {
		if ((s = uconnect(PATH_DEVD_SOCKET)) == -1)
			(void)sleep(1);
	}
	return (s);
}

static bool
is_batt_event(char *str)
{
	char *p, *q;

	if (str[0] != '!')
		return (false);
	for (p = str + 1; (p = strtok(p, " \n")) != NULL; p = NULL) {
		if ((q = strchr(p, '=')) == NULL)
			continue;
		*q++ = '\0';
		if (strcmp(p, "system") == 0) {
			if (strcmp(q, "ACPI") != 0)
				return (false);
		} else if (strcmp(p, "subsystem") == 0) {
			if (strcmp(q, "CMBAT") == 0)
				return (true);
		}
	}
	return (false);
}
