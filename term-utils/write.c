/*
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Jef Poskanzer and Craig Leres of the Lawrence Berkeley Laboratory.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
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
 *
 * Modified for Linux, Mon Mar  8 18:16:24 1993, faith@cs.unc.edu
 * Wed Jun 22 21:41:56 1994, faith@cs.unc.edu:
 *      Added fix from Mike Grupenhoff (kashmir@umiacs.umd.edu)
 * Mon Jul  1 17:01:39 MET DST 1996, janl@math.uio.no:
 *      - Added fix from David.Chapell@mail.trincoll.edu enabling daemons
 *	  to use write.
 *      - ANSIed it since I was working on it anyway.
 * 1999-02-22 Arkadiusz Miśkiewicz <misiek@pld.ORG.PL>
 * - added Native Language Support
 *
 */

#include <errno.h>
#include <getopt.h>
#include <paths.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utmp.h>

#include "c.h"
#include "carefulputc.h"
#include "closestream.h"
#include "nls.h"
#include "strutils.h"
#include "xalloc.h"

static sig_atomic_t signal_received = 0;

struct write_control {
	uid_t src_uid;
	const char *src_login;
	char *src_tty;
	const char *dst_login;
	char dst_tty[PATH_MAX];
};

static void __attribute__((__noreturn__)) usage(FILE *out)
{
	fputs(USAGE_HEADER, out);
	fprintf(out,
	      _(" %s [options] <user> [<ttyname>]\n"),
	      program_invocation_short_name);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Send a message to another user.\n"), out);

	fputs(USAGE_OPTIONS, out);
	fputs(USAGE_HELP, out);
	fputs(USAGE_VERSION, out);

	fprintf(out, USAGE_MAN_TAIL("write(1)"));
	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

/*
 * check_tty - check that a terminal exists, and get the message bit
 *     and the access time
 */
static int check_tty(char *tty, int *tty_writeable, time_t *tty_atime, int showerror)
{
	struct stat s;
	char path[PATH_MAX];

	if (sizeof(path) < strlen(tty) + 6)
		return 1;
	sprintf(path, "/dev/%s", tty);
	if (stat(path, &s) < 0) {
		if (showerror)
			warn("%s", path);
		return 1;
	}
	if (getuid() == 0)	/* root can always write */
		*tty_writeable = 1;
	else
		*tty_writeable = (s.st_mode & S_IWGRP) && (getegid() == s.st_gid);
	if (tty_atime)
		*tty_atime = s.st_atime;
	return 0;
}

/*
 * check_utmp - checks that the given user is actually logged in on
 *     the given tty
 */
static int check_utmp(const struct write_control *ctl)
{
	struct utmp *u;
	int res = 1;

	utmpname(_PATH_UTMP);
	setutent();

	while ((u = getutent())) {
		if (strncmp(ctl->dst_login, u->ut_user, sizeof(u->ut_user)) == 0 &&
		    strncmp(ctl->dst_tty, u->ut_line, sizeof(u->ut_line)) == 0) {
			res = 0;
			break;
		}
	}

	endutent();
	return res;
}

/*
 * search_utmp - search utmp for the "best" terminal to write to
 *
 * Ignores terminals with messages disabled, and of the rest, returns
 * the one with the most recent access time.  Returns as value the number
 * of the user's terminals with messages enabled, or -1 if the user is
 * not logged in at all.
 *
 * Special case for writing to yourself - ignore the terminal you're
 * writing from, unless that's the only terminal with messages enabled.
 */
static void search_utmp(struct write_control *ctl)
{
	struct utmp *u;
	time_t best_atime = 0, tty_atime;
	int num_ttys = 0, valid_ttys = 0, tty_writeable = 0, user_is_me = 0;

	utmpname(_PATH_UTMP);
	setutent();

	while ((u = getutent())) {
		if (strncmp(ctl->dst_login, u->ut_user, sizeof(u->ut_user)) != 0)
			continue;
		num_ttys++;
		if (check_tty(u->ut_line, &tty_writeable, &tty_atime, 0))
			/* bad term? skip */
			continue;
		if (ctl->src_uid && !tty_writeable)
			/* skip ttys with msgs off */
			continue;
		if (strcmp(u->ut_line, ctl->src_tty) == 0) {
			user_is_me = 1;
			/* don't write to yourself */
			continue;
		}
		if (u->ut_type != USER_PROCESS)
			/* it's not a valid entry */
			continue;
		valid_ttys++;
		if (best_atime < tty_atime) {
			best_atime = tty_atime;
			xstrncpy(ctl->dst_tty, u->ut_line,
				 sizeof(ctl->dst_tty));
		}
	}

	endutent();
	if (num_ttys == 0)
		errx(EXIT_FAILURE, _("%s is not logged in"), ctl->dst_login);
	if (valid_ttys == 0) {
		if (user_is_me) {
			/* ok, so write to yourself! */
			xstrncpy(ctl->dst_tty, ctl->src_tty, sizeof(ctl->dst_tty));
			return;
		}
		errx(EXIT_FAILURE, _("%s has messages disabled"), ctl->dst_login);
	}
	if (1 < valid_ttys)
		warnx(_("%s is logged in more than once; writing to %s"),
		      ctl->dst_login, ctl->dst_tty);
}

/*
 * signal_handler - cause write loop to exit
 */
static void signal_handler(int signo)
{
	signal_received = signo;
}

/*
 * write_line - like fputs(), but makes control characters visible and
 *     turns \n into \r\n.
 */
static void write_line(char *s)
{
	while (*s) {
		const int c = *s++;

		if ((c == '\n' && fputc_careful('\r', stdout, '^') == EOF)
		    || fputc_careful(c, stdout, '^') == EOF)
			err(EXIT_FAILURE, _("carefulputc failed"));
	}
}

/*
 * do_write - actually make the connection
 */
static void do_write(const struct write_control *ctl)
{
	char *login, *pwuid, timestamp[6];
	struct passwd *pwd;
	time_t now;
	struct tm *tm;
	char path[PATH_MAX], *host, line[512];
	struct sigaction sigact;

	/* Determine our login name(s) before the we reopen() stdout */
	if ((pwd = getpwuid(ctl->src_uid)) != NULL)
		pwuid = pwd->pw_name;
	else
		pwuid = "???";
	if ((login = getlogin()) == NULL)
		login = pwuid;

	if (sizeof(path) < strlen(ctl->dst_tty) + 6)
		errx(EXIT_FAILURE, _("tty path %s too long"), ctl->dst_tty);
	snprintf(path, sizeof(path), "/dev/%s", ctl->dst_tty);
	if ((freopen(path, "w", stdout)) == NULL)
		err(EXIT_FAILURE, "%s", path);

	sigact.sa_handler = signal_handler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGHUP, &sigact, NULL);

	host = xgethostname();
	if (!host)
		host = xstrdup("???");

	now = time((time_t *)NULL);
	tm = localtime(&now);
	strftime(timestamp, sizeof(timestamp), "%H:%M", tm);
	/* print greeting */
	printf("\r\n\a\a\a");
	if (strcmp(login, pwuid))
		printf(_("Message from %s@%s (as %s) on %s at %s ..."),
		       login, host, pwuid, ctl->src_tty, timestamp);
	else
		printf(_("Message from %s@%s on %s at %s ..."),
		       login, host, ctl->src_tty, timestamp);
	free(host);
	printf("\r\n");

	while (fgets(line, sizeof(line), stdin) != NULL) {
		if (signal_received)
			break;
		write_line(line);
	}
	printf("EOF\r\n");
}

int main(int argc, char **argv)
{
	int tty_writeable = 0, src_fd, c;
	struct write_control ctl = { 0 };

	static const struct option longopts[] = {
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0}
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	while ((c = getopt_long(argc, argv, "Vh", longopts, NULL)) != -1)
		switch (c) {
		case 'V':
			printf(UTIL_LINUX_VERSION);
			return EXIT_SUCCESS;
		case 'h':
			usage(stdout);
		default:
			usage(stderr);
		}

	/* check that sender has write enabled */
	if (isatty(STDIN_FILENO))
		src_fd = STDIN_FILENO;
	else if (isatty(STDOUT_FILENO))
		src_fd = STDOUT_FILENO;
	else if (isatty(STDERR_FILENO))
		src_fd = STDERR_FILENO;
	else
		src_fd = -1;

	if (src_fd != -1) {
		if (!(ctl.src_tty = ttyname(src_fd)))
			errx(EXIT_FAILURE,
			     _("can't find your tty's name"));
		/*
		 * We may have /dev/ttyN but also /dev/pts/xx. Below,
		 * check_tty() will put "/dev/" in front, so remove that
		 * part.
		 */
		if (!strncmp(ctl.src_tty, "/dev/", 5))
			ctl.src_tty += 5;
		if (check_tty(ctl.src_tty, &tty_writeable, NULL, 1))
			exit(EXIT_FAILURE);
		if (!tty_writeable)
			errx(EXIT_FAILURE,
			     _("you have write permission turned off"));
		tty_writeable = 0;
	} else
		ctl.src_tty = "<no tty>";

	ctl.src_uid = getuid();

	/* check args */
	switch (argc) {
	case 2:
		ctl.dst_login = argv[1];
		search_utmp(&ctl);
		do_write(&ctl);
		break;
	case 3:
		ctl.dst_login = argv[1];
		if (!strncmp(argv[2], "/dev/", 5))
			xstrncpy(ctl.dst_tty, argv[2] + 5, sizeof(ctl.dst_tty));
		else
			xstrncpy(ctl.dst_tty, argv[2], sizeof(ctl.dst_tty));
		if (check_utmp(&ctl))
			errx(EXIT_FAILURE,
			     _("%s is not logged in on %s"),
			     ctl.dst_login, ctl.dst_tty);
		if (check_tty(ctl.dst_tty, &tty_writeable, NULL, 1))
			exit(EXIT_FAILURE);
		if (ctl.src_uid && !tty_writeable)
			errx(EXIT_FAILURE,
			     _("%s has messages disabled on %s"),
			     ctl.dst_login, ctl.dst_tty);
		do_write(&ctl);
		break;
	default:
		usage(stderr);
	}
	return EXIT_SUCCESS;
}
