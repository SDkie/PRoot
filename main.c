/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of PRoot: a PTrace based chroot alike.
 *
 * Copyright (C) 2010 STMicroelectronics
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 *
 * Author: Cedric VINCENT (cedric.vincent@st.com)
 * Inspired by: strace.
 */

#include <unistd.h>     /* fork(2), chdir(2), exec*(3), */
#include <stdlib.h>     /* exit(3), EXIT_*, */
#include <stdio.h>      /* puts(3), */
#include <sys/wait.h>   /* wait(2), */
#include <sys/ptrace.h> /* ptrace(2), */
#include <limits.h>     /* PATH_MAX, */
#include <string.h>     /* strcmp(3), */

#include "path.h"
#include "child_info.h"
#include "syscall.h" /* translate_syscall(), */
#include "execve.h"
#include "notice.h"

static const char *opt_new_root = NULL;
static char *opt_args_default[] = { "/bin/sh", NULL };
static char **opt_args = &opt_args_default[0];

static const char *opt_runner = NULL;

static int opt_many_jobs = 0;
static int opt_allow_unknown = 0;
static int opt_allow_ptrace = 0;

static int opt_check_fd = 0;
static int opt_check_syscall = 0;

static void exit_usage(void)
{
	puts("");
	puts("Usages:");
	puts("  proot [options] <fake_root>");
	puts("  proot [options] <fake_root> <program> [args]");
	puts("  proot [options] <fake_root> <pid>");
	puts("");
	puts("Arguments:");
	puts("  <fake_root>   is the path to the fake root file system");
	puts("  <program>     is the path of the program to launch, default is $SHELL");
	puts("  [args]        are the optional arguments passed to <program>");
	puts("  <pid>         is the identifier of the process to attach on-the-fly");
	puts("");
	puts("Common options:");
	puts("  -x <path>     don't translate access to <path> (can be repeated)");
	puts("  -r <program>  use <program> to run each process");
	puts("  -v            increase the verbose level");
	puts("");
	puts("Insecure options:");
	puts("  -j <integer>  use <integer> jobs (faster but prone to race condition exploit)");
	puts("  -u            don't block unknown syscalls");
	puts("  -p            don't block ptrace(2)");
	puts("");
	puts("Debug options:");
	puts("  -d            check every /proc/$pid/fd/* point to a translated path (slow!)");
	puts("  -s            check /proc/$pid/syscall agrees with the internal state");
	puts("");

	exit(EXIT_FAILURE);
}

static void parse_options(int argc, char *argv[])
{
	int i;

	/* Stupid command-line parser. */
	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-')
			break; /* End of PRoot options. */

		if (argv[i][2] != '\0') {
			exit_usage();
			exit(EXIT_FAILURE);			
		}

		switch (argv[i][1]) {
		case 'x':
			i++;
			if (i >= argc)
				notice(ERROR, USER, "missing value for the option -x");
			exclude_path(argv[i]);
			break;

		case 'r':
			i++;
			if (i >= argc)
				notice(ERROR, USER, "missing value for the option -t");
			opt_runner = argv[i];
			break;

		case 'v':
			verbose_level++;
			break;

		case 'j':
			notice(WARNING, USER, "option -j not yet supported");
			i++;
			if (i >= argc)
				notice(ERROR, USER, "missing value for the option -j");
			opt_many_jobs = atoi(argv[i]);
			if (opt_many_jobs == 0)
				notice(ERROR, USER, "-j \"%s\" is not valid", argv[i]);
			break;

		case 'u':
			opt_allow_unknown = 1;
			break;

		case 'p':
			opt_allow_ptrace = 1;
			break;

		case 'd':
			opt_check_fd = 1;
			break;

		case 's':
			opt_check_syscall = 1;
			break;

		default:
			exit_usage();
		}
	}

	if (argc == i)
		exit_usage();

	opt_new_root = argv[i];

	if (argc - i == 1) {
		char *shell = getenv("SHELL");
		if (shell != NULL)
			opt_args_default[0] = shell;
	}
	else  {
		opt_args = &argv[i + 1];

		if (atoi(opt_args[0]) != 0 && opt_args[1] == NULL)
			notice(ERROR, USER, "attaching a process on-the-fly not yet supported");
	}

	init_module_path(opt_new_root);
	init_module_child_info();
	init_module_syscall(opt_check_syscall, opt_allow_unknown, opt_allow_ptrace);
	init_module_execve(opt_runner);
}

static void launch_process(const char *argv0)
{
	char launcher[PATH_MAX];
	long status;
	pid_t pid;

	pid = fork();
	switch(pid) {
	case -1:
		notice(ERROR, SYSTEM, "fork()");

	case 0: /* child */

		if (realpath(argv0, launcher) == NULL)
			notice(ERROR, SYSTEM, "realpath(\"%s\")", argv0);

		/* Declare myself as ptraceable before executing the
		 * requested program. */
		status = ptrace(PTRACE_TRACEME, 0, NULL, NULL);
		if (status < 0)
			notice(ERROR, SYSTEM, "ptrace(TRACEME)");

		/* Ensure the child starts in a valid cwd within the
		 * new root. */
		status = chdir(opt_new_root);
		if (status < 0)
			notice(ERROR, SYSTEM, "chdir(\"%s\")", opt_new_root);

		status = setenv("PWD", "/", 1);
		if (status < 0)
			notice(ERROR, SYSTEM, "setenv(\"PWD\")");

		status = setenv("OLDPWD", "/", 1);
		if (status < 0)
			notice(ERROR, SYSTEM, "setenv(\"OLDPWD\")");

		status = execvp(launcher, opt_args);
		notice(ERROR, SYSTEM, "execvp(\"%s\")", launcher);

	default: /* parent */
		if (new_child(pid) == NULL)
			exit(EXIT_FAILURE);
		break;
	}
}

static int main_loop()
{
	int last_exit_status;
	int child_status;
	long status;
	int signal;
	pid_t pid;

	signal = 0;
	while (get_nb_children() > 0) {
		/* Wait for the next child's stop. */
		pid = wait(&child_status);
		if (pid < 0)
			notice(ERROR, SYSTEM, "wait()");

		/* Check every child file descriptors. */
		if (opt_check_fd != 0)
			foreach_child(check_fd);

		if (WIFEXITED(child_status)) {
			VERBOSE(1, "pid %d: exited with status %d",
			           pid, WEXITSTATUS(child_status));
			last_exit_status = WEXITSTATUS(child_status);
			delete_child(pid);
			continue; /* Skip the call to ptrace(SYSCALL). */
		}
		else if (WIFSIGNALED(child_status)) {
			VERBOSE(1, "pid %d: terminated with signal %d",
			           pid, WTERMSIG(child_status));
			delete_child(pid);
			continue; /* Skip the call to ptrace(SYSCALL). */
		}
		else if (WIFCONTINUED(child_status)) {
			VERBOSE(1, "pid %d: continued", pid);
			signal = SIGCONT;
		}
		else if (WIFSTOPPED(child_status)) {

			/* Don't WSTOPSIG() to extract the signal
			 * since it clears the PTRACE_EVENT_* bits. */
			signal = (child_status & 0xfff00) >> 8;

			switch (signal) {
			case SIGTRAP | 0x80:
				translate_syscall(pid);
				signal = 0;
				break;

			case SIGTRAP:
				/* Distinguish some events from others and
				 * automatically trace each new process with
				 * the same options.  Note: only the first
				 * process should come here (because of
				 * TRACEEXEC). */
				status = ptrace(PTRACE_SETOPTIONS, pid, NULL,
						PTRACE_O_TRACESYSGOOD |
						PTRACE_O_TRACEFORK    |
						PTRACE_O_TRACEVFORK   |
						PTRACE_O_TRACECLONE   |
						PTRACE_O_TRACEEXEC);
				if (status < 0)
					notice(ERROR, SYSTEM, "ptrace(PTRACE_SETOPTIONS)");

				signal = 0;
				break;

			case SIGTRAP | PTRACE_EVENT_FORK  << 8:
			case SIGTRAP | PTRACE_EVENT_VFORK << 8:
			case SIGTRAP | PTRACE_EVENT_CLONE << 8:
			case SIGTRAP | PTRACE_EVENT_EXEC  << 8:
				/* Ignore these signals. */
				signal = 0;
				break;

			default:
				/* Propagate all other signals. */
				break;
			}
		}
		else {
			notice(WARNING, INTERNAL, "unknown trace event");
			signal = 0;
		}

		/* Restart the child and stop it at the next entry or exit of
		 * a system call. */
		status = ptrace(PTRACE_SYSCALL, pid, NULL, signal);
		if (status < 0)
			notice(ERROR, SYSTEM, "ptrace(SYSCALL)");
	}

	return last_exit_status;
}

int main(int argc, char *argv[])
{
	size_t length_argv0 = strlen(argv[0]);
	size_t length_proot = strlen("proot");

	/* Use myself as a launcher. We need a launcher to
	 * ensure the first execve(2) is catched by PRoot. */
	if (length_argv0 < length_proot
	    || strcmp(argv[0] + length_argv0 - length_proot, "proot") != 0) {
		int i;

		fprintf(stderr, "proot:");
		for (i = 0; i < argc; i++)
			fprintf(stderr, " %s", argv[i]);
		fprintf(stderr, "\n");

		execv(argv[0], argv);

		notice(ERROR, SYSTEM, "execv(\"%s\"): ", argv[0]);
	}

	parse_options(argc, argv);
	launch_process(argv[0]);
	return main_loop();
}
