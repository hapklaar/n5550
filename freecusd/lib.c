/*
 * Copyright 2013-2014, 2016-2017, 2020 Ian Pilcher <arequipeno@gmail.com>
 *
 * This program is free software.  You can redistribute it or modify it under
 * the terms of version 2 of the GNU General Public License (GPL), as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY -- without even the implied warranty of MERCHANTIBILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the text of the GPL for more details.
 *
 * Version 2 of the GNU General Public License is available at:
 *
 *   http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 */

#include "freecusd.h"

#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <stdarg.h>

#define FCD_LIB_BUF_CHUNK	2000

sigset_t fcd_mon_ppoll_sigmask;

/*
 * Sleeps for the specified number of seconds, unless interrupted by a signal
 * (SIGUSR1).  Returns the thread-local value of fcd_thread_exit_flag (or -1 on
 * error).
 *
 * NOTE: Does not check fcd_thread_exit_flag before sleeping (assumes that
 * 	 SIGUSR1 has been blocked).
 */
int fcd_lib_monitor_sleep(time_t seconds)
{
	struct timespec ts;

	ts.tv_sec = seconds;
	ts.tv_nsec = 0;

	if (ppoll(NULL, 0, &ts, &fcd_mon_ppoll_sigmask) == -1
						&& errno != EINTR) {
		FCD_PERROR("ppoll");
		return -1;
	}

	return fcd_thread_exit_flag;
}

/*
 * Calculates *deadline, based on current time and timeout.  Returns 0 on
 * success, -1 on error.
 */
static int fcd_lib_deadline(struct timespec *deadline,
			    const struct timespec *timeout)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC_COARSE, &now) == -1) {
		FCD_PERROR("clock_gettime");
		return -1;
	}

	deadline->tv_sec  = now.tv_sec  + timeout->tv_sec;
	deadline->tv_nsec = now.tv_nsec + timeout->tv_nsec;

	if (deadline->tv_nsec >= 1000000000L)
	{
		deadline->tv_nsec -= 1000000000L;
		++(deadline->tv_sec);
	}

	return 0;
}

/*
 * Calculates *remaining time, based on current time and deadline (but "rounds"
 * negative result up to zero).  Returns 0 on success, -1 on error.
 */
static int fcd_lib_remaining(struct timespec *remaining,
			     const struct timespec *deadline)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC_COARSE, &now) == -1) {
		FCD_PERROR("clock_gettime");
		return -1;
	}

	/* Linux time_t is signed */

	remaining->tv_sec  = deadline->tv_sec  - now.tv_sec;
	remaining->tv_nsec = deadline->tv_nsec - now.tv_nsec;

	if (remaining->tv_nsec < 0)
	{
		remaining->tv_nsec += 1000000000L;
		--(remaining->tv_sec);
	}

	if (remaining->tv_sec < 0)
	{
		remaining->tv_sec = 0;
		remaining->tv_nsec = 0;
	}

	return 0;
}

/*
 * Acts as a wrapper around read(2) with a timeout.  Updates *timeout with
 * remaining time on successful return (>= 0).  Returns # of bytes read (0 =
 * EOF, -1 = error, -2 = timeout, -3 = thread exit signal received).
 */
ssize_t fcd_lib_read(int fd, void *buf, size_t count, struct timespec *timeout)
{
	struct timespec deadline;
	struct pollfd pfd;
	ssize_t ret;

	if (fcd_lib_deadline(&deadline, timeout) == -1)
		return -1;

	pfd.fd = fd;
	pfd.events = POLLIN;

	while (!fcd_thread_exit_flag)
	{
		if (fcd_lib_remaining(timeout, &deadline) == -1)
			return -1;

		ret = ppoll(&pfd, 1, timeout, &fcd_mon_ppoll_sigmask);
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			FCD_PERROR("ppoll");
			return -1;
		}

		if (ret == 0)
			return -2;

		/*
		 * Different file descriptors (regular files, pipes, sysfs/proc
		 * files, etc.) behave so differently that it's impossible to
		 * check revents in a meaningful way.
		 */

		ret = read(fd, buf, count);
		if (ret == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			FCD_PERROR("read");
			return -1;
		}
		else {
			if (fcd_lib_remaining(timeout, &deadline) == -1)
				return -1;
			return ret;
		}
	}

	return -3;
}

/*
 * Called as necessary to grow an input buffer.  Returns 0 on success, -1 on
 * error, -4 if max buffer size would be exceeded.
 *
 * NOTE: Buffer size is actually limited to the smallest multiple of
 *	 FCD_LIB_BUF_CHUNK that is greater than or equal to max_size.
 */
static int fcd_lib_grow_buf(char **buf, size_t *buf_size, size_t max_size)
{
	size_t new_size;
	char *new_buf;

	if (*buf == NULL || *buf_size == 0)
		new_size = FCD_LIB_BUF_CHUNK;
	else
		new_size = *buf_size + FCD_LIB_BUF_CHUNK;

	/*
	 * Ensure max_size is a multiple of FCD_LIB_BUF_CHUNK, rounding up if
	 * necessary
	 */
	max_size = ((max_size + FCD_LIB_BUF_CHUNK - 1) / FCD_LIB_BUF_CHUNK)
						* FCD_LIB_BUF_CHUNK;

	if (new_size > max_size)
		return -4;

	new_buf = realloc(*buf, new_size);
	if (new_buf == NULL) {
		FCD_PERROR("realloc");
		return -1;
	}

	*buf = new_buf;
	*buf_size = new_size;

	return 0;
}

/*
 * Reads from fd until EOF, timeout, interrupted by signal (SIGUSR1), max buffer
 * size is exceeded or error occurs.  Input buffer is grown as necessary.
 * Updates *timeout with remaining time on successful return (>= 0).  Returns #
 * of bytes read, which may be 0 (-1 = error, -2 = timeout, -3 = interrupted by
 * thread exit signal, -4 max buffer size would be exceeded).
 */
ssize_t fcd_lib_read_all(int fd, char **buf, size_t *buf_size, size_t max_size,
			 struct timespec *timeout)
{
	size_t total;
	ssize_t ret;

	total = 0;

	do {
		if (total == *buf_size) {
			ret = fcd_lib_grow_buf(buf, buf_size, max_size);
			if (ret < 0)
				return ret;	/* -1 or -4 */
		}

		ret = fcd_lib_read(fd, *buf + total, *buf_size - total,
				   timeout);
		if (ret < 0)
			return ret;	/* -1, -2, or -3 */

		total += ret;

	} while (ret != 0);

	/*
	 * If the number of bytes read is an exact multiple of FCD_BUF_CHUNK,
	 * buffer will have been grown immediately before fcd_read returned 0.
	 */

	(*buf)[total] = 0;

	return total;
}

/*
 * Mark a monitor as failed
 */
void fcd_lib_fail(struct fcd_monitor *const mon)
{
	static const char disabled_msg[20] = "ERROR: NOT AVAILABLE";
	int ret;

	FCD_WARN("Disabling %s monitor\n", mon->name);

	ret = pthread_mutex_lock(&mon->mutex);
	if (ret != 0)
		FCD_PT_ABRT("pthread_mutex_lock", ret);

	fcd_alert_update(FCD_ALERT_SET_REQ, &mon->sys_fail);
	memcpy(mon->buf + 45, disabled_msg, 20);

	ret = pthread_mutex_unlock(&mon->mutex);
	if (ret != 0)
		FCD_PT_ABRT("pthread_mutex_unlock", ret);
}

/*
 * Called by a monitor thread to disable itself when an error occurs.
 *
 * Never returns.
 */
__attribute__((noreturn))
void fcd_lib_fail_and_exit(struct fcd_monitor *mon)
{
	fcd_lib_fail(mon);
	pthread_exit(NULL);
}

/*
 * Called by a monitor thread to clean up child process communication resources
 * (reaper thread pipe and buffer) and disable itself when an error occurs.
 *
 * Never returns.
 *
 * NOTE: buf may be NULL
 */
__attribute__((noreturn))
void fcd_lib_parent_fail_and_exit(struct fcd_monitor *mon, const int *pipe_fds, char *buf)
{
	free(buf);
	fcd_proc_close_pipe(pipe_fds);
	fcd_lib_fail_and_exit(mon);
}

/*
 * Called by monitor threads to update message buffer, alerts, and PWM flags in
 * monitor structure - where main thread will act upon them.
 */
void fcd_lib_set_mon_status2(struct fcd_monitor *const mon,
			     const char *const restrict upper,
			     const char *const restrict lower,
			     const int warn,
			     const int fail,
			     const int *const disks,
			     const uint8_t pwm_flags)
{
	unsigned i, led;
	int ret;

	ret = pthread_mutex_lock(&mon->mutex);
	if (ret != 0)
		FCD_PT_ABRT("pthread_mutex_lock", ret);

	if (upper != NULL)
		memcpy(mon->buf + 5, upper, 20);

	memcpy(mon->buf + 45, lower, 20);

	fcd_alert_update(warn ? FCD_ALERT_SET_REQ : FCD_ALERT_CLR_REQ,
			 &mon->sys_warn);
	fcd_alert_update(fail ? FCD_ALERT_SET_REQ : FCD_ALERT_CLR_REQ,
			 &mon->sys_fail);

	mon->new_pwm_flags = pwm_flags;

	if (disks != NULL) {

		for (i = 0; i < fcd_conf_disk_count; ++i) {

			led = fcd_conf_disks[i].port_no - 2;

			fcd_alert_update(disks[i] ? FCD_ALERT_SET_REQ :
						    FCD_ALERT_CLR_REQ,
					 &mon->disk_alerts[led]);
		}
	}

	ret = pthread_mutex_unlock(&mon->mutex);
	if (ret != 0)
		FCD_PT_ABRT("pthread_mutex_unlock", ret);
}

void fcd_lib_set_mon_status(struct fcd_monitor *const mon,
			    const char *const buf,
			    const int warn,
			    const int fail,
			    const int *const disks,
			    const uint8_t pwm_flags)
{
	fcd_lib_set_mon_status2(mon, NULL, buf, warn, fail, disks, pwm_flags);
}

/*
 * Called only from fcd_lib_cmd_child().  Sets the CLOEXEC flag on fd.  Aborts
 * on error.
 */
static void fcd_lib_child_set_cloexec(int fd)
{
	int current_flags;

	current_flags = fcntl(fd, F_GETFD);
	if (current_flags == -1)
		FCD_CHILD_PABORT("fcntl");

	if (fcntl(fd, F_SETFD, current_flags | FD_CLOEXEC) == -1)
		FCD_CHILD_PABORT("fcntl");
}

/*
 * Called in child process to set up STDOUT/STDERR and exec external program.
 * Never returns (aborts on error).
 */
__attribute__((noreturn))
static void fcd_lib_cmd_child(int fd, char **cmd)
{
	/*
	 * This flow is a bit ugly.  If we created an output pipe (fd != -1),
	 * then replace STDOUT with the pipe.  If we did NOT create an output
	 * pipe (fd == -1), then set the CLOEXEC flag on STDOUT -- unless we're
	 * running in the foreground.
	 *
	 * STDERR also gets its CLOEXEC flag set, unless we're running in the
	 * foreground.  (It doesn't matter if we're creating an output pipe or
	 * not.)
	 */

	if (fd != -1) {

		/* CLOEXEC is NOT inherited by dup2'ed descriptor */
		if (dup2(fd, STDOUT_FILENO) == -1)
			FCD_CHILD_PABORT("dup2");
	}

	if (!fcd_err_foreground) {

		if (fd == -1)
			fcd_lib_child_set_cloexec(STDOUT_FILENO);

		fcd_lib_child_set_cloexec(STDERR_FILENO);
	}

	execv(cmd[0], cmd + 1);

	FCD_CHILD_PABORT("execv");
}

/*
 * Spawns child process, creating pipe to read child command's output if
 * requested (create_output_pipe != 0).  On success, returns read fd of child
 * output pipe (or 0 if create_output_pipe == 0); returns -1 on error.
 */
static int fcd_lib_cmd_spawn(pid_t *child, char **cmd, const int *reaper_pipe,
			     int create_output_pipe)
{
	int output_pipe[2];

	if (create_output_pipe) {

		/* CLOEXEC will not be inherited by dup2'ed file descriptor */
		if (pipe2(output_pipe, O_CLOEXEC) == -1) {
			FCD_PERROR("pipe2");
			return -1;
		}
	}

	*child = fcd_proc_fork(reaper_pipe);
	if (*child == -1) {
		FCD_PERROR("fork");
		if (create_output_pipe) {
			if (close(output_pipe[0]) == -1)
				FCD_PERROR("close");
			if (close(output_pipe[1]) == -1)
				FCD_PERROR("close");
		}
		return -1;
	}

	if (*child == 0) {
		fcd_lib_cmd_child(create_output_pipe ? output_pipe[1] : -1,
				  cmd);
	}

	if (create_output_pipe)	{

		if (close(output_pipe[1]) == -1) {
			FCD_PERROR("close");
			if (close(output_pipe[0]) == -1) {
				FCD_PERROR("close");
				FCD_ABORT("Failed to close child pipe\n");
			}
			fcd_proc_kill(*child, reaper_pipe);
			return -1;
		}
	}

	return create_output_pipe ? output_pipe[0] : 0;
}

/*
 * Executes an external program in a child process, reads its output into the
 * buffer at buf (which is grown as necessary, up to max_size bytes), and
 * stores its exit status (0 - 255) in *status.  Returns the number of bytes
 * read (which may be 0), -1 on error, -2 if the timeout expires, -3 if the
 * thread exit signal is received, or -4 if the maximum buffer size is exceeded.
 * (If necessary, the child process is killed.)
 */
ssize_t fcd_lib_cmd_output(int *status, char **cmd, char **buf,
			   size_t *buf_size, size_t max_size,
			   struct timespec *timeout, const int *pipe_fds)
{
	ssize_t bytes_read;
	int ret, fd;
	pid_t child;

	fd = fcd_lib_cmd_spawn(&child, cmd, pipe_fds, 1);
	if (fd == -1)
		return -1;

	bytes_read = fcd_lib_read_all(fd, buf, buf_size, max_size, timeout);
	if (bytes_read < 0) {
		if (close(fd) == -1)
			FCD_PERROR("close");
		fcd_proc_kill(child, pipe_fds);
		return bytes_read;
	}

	if (close(fd) == -1) {
		FCD_PERROR("close");
		fcd_proc_kill(child, pipe_fds);
		return -1;
	}

	ret = fcd_proc_wait(status, pipe_fds, timeout);
	if (ret < 0) {
		fcd_proc_kill(child, pipe_fds);
		return ret;
	}

	if (!WIFEXITED(*status)) {
		FCD_WARN("Child process did not terminate normally\n");
		return -1;
	}

	*status = WEXITSTATUS(*status);

	return bytes_read;
}

/*
 * Executes an external program in a child process and returns its exit status
 * (0 - 255).  Returns -1 on error, -2 if timeout expires, or -3 if the thread
 * exit signal is received.  (If necessary, the child process is killed.)
 */
int fcd_lib_cmd_status(char **cmd, struct timespec *timeout,
		       const int *pipe_fds)
{
	int status, ret;
	pid_t child;

	if (fcd_lib_cmd_spawn(&child, cmd, pipe_fds, 0) == -1)
		return -1;

	ret = fcd_proc_wait(&status, pipe_fds, timeout);
	if (ret < 0) {
		fcd_proc_kill(child, pipe_fds);
		return ret;
	}

	if (!WIFEXITED(status)) {
		FCD_WARN("Child process did not terminate normally\n");
		return -1;
	}

	return WEXITSTATUS(status);
}

/*
 * Returns the index of the RAID disk identified by c, -1 on error
 */
int fcd_lib_disk_index(char c)
{
	int i;

	for (i = 0; i < (int)fcd_conf_disk_count; ++i) {

		if (c == fcd_conf_disks[i].name[FCD_DISK_NAME_SIZE - 2])
			return i;
	}

	return -1;
}

#if 0
/*
 * Mutex to prevent HDD temperature & S.M.A.R.T. threads from stepping on each
 * other
 */
static pthread_mutex_t fcd_lib_disk_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Locks the disk mutex
 */
void fcd_lib_disk_mutex_lock(void)
{
	int ret;

	ret = pthread_mutex_lock(&fcd_lib_disk_mutex);
	if (ret != 0)
		FCD_PT_ABRT("pthread_mutex_lock", ret);
}

/*
 * Unlocks the disk mutex
 */
void fcd_lib_disk_mutex_unlock(void)
{
	int ret;

	ret = pthread_mutex_unlock(&fcd_lib_disk_mutex);
	if (ret != 0)
		FCD_PT_ABRT("pthread_mutex_unlock", ret);
}
#endif

int fcd_lib_snprintf(char *const restrict str,
		     const size_t size,
		     const char *const restrict format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = vsnprintf(str, size, format, ap);
	va_end(ap);

	if (ret < 0) {
		FCD_PERROR("vsnprintf");
	}
	else if (ret < (int)size) {
		str[ret] = ' ';
	}

	return ret;
}

void fcd_lib_dump_temp_cfg(const int *const cfg)
{
	FCD_DUMP("\t\twarning: %d\n", cfg[FCD_CONF_TEMP_WARN]);
	FCD_DUMP("\t\tcritical: %d\n", cfg[FCD_CONF_TEMP_FAIL]);
	FCD_DUMP("\t\tfan max on: %d\n", cfg[FCD_CONF_TEMP_FAN_MAX_ON]);
	FCD_DUMP("\t\tfan max hysteresis: %d\n", cfg[FCD_CONF_TEMP_FAN_MAX_HYST]);
	FCD_DUMP("\t\tfan high on: %d\n", cfg[FCD_CONF_TEMP_FAN_HIGH_ON]);
	FCD_DUMP("\t\tfan high hysteresis: %d\n", cfg[FCD_CONF_TEMP_FAN_HIGH_HYST]);
}
