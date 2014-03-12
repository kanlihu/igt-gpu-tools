/*
 * Copyright © 2013 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <sys/stat.h>
#include <sys/mount.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "drmtest.h"
#include "igt_display.h"
#include "igt_debugfs.h"

/*
 * General debugfs helpers
 */
int igt_debugfs_init(igt_debugfs_t *debugfs)
{
	const char *path = "/sys/kernel/debug";
	struct stat st;
	int n;

	if (stat("/debug/dri", &st) == 0) {
		path = "/debug/dri";
		goto find_minor;
	}

	if (stat("/sys/kernel/debug/dri", &st) == 0)
		goto find_minor;

	if (stat("/sys/kernel/debug", &st))
		return errno;

	if (mount("debug", "/sys/kernel/debug", "debugfs", 0, 0))
		return errno;

find_minor:
	strcpy(debugfs->root, path);
	for (n = 0; n < 16; n++) {
		int len = sprintf(debugfs->dri_path, "%s/dri/%d", path, n);
		sprintf(debugfs->dri_path + len, "/i915_error_state");
		if (stat(debugfs->dri_path, &st) == 0) {
			debugfs->dri_path[len] = '\0';
			return 0;
		}
	}

	debugfs->dri_path[0] = '\0';
	return ENOENT;
}

int igt_debugfs_open(igt_debugfs_t *debugfs, const char *filename, int mode)
{
	char buf[1024];

	sprintf(buf, "%s/%s", debugfs->dri_path, filename);
	return open(buf, mode);
}

FILE *igt_debugfs_fopen(igt_debugfs_t *debugfs, const char *filename,
			const char *mode)
{
	char buf[1024];

	sprintf(buf, "%s/%s", debugfs->dri_path, filename);
	return fopen(buf, mode);
}

/*
 * Pipe CRC
 */

bool igt_crc_is_null(igt_crc_t *crc)
{
	int i;

	for (i = 0; i < crc->n_words; i++)
		if (crc->crc[i])
			return false;

	return true;
}

bool igt_crc_equal(igt_crc_t *a, igt_crc_t *b)
{
	int i;

	if (a->n_words != b->n_words)
		return false;

	for (i = 0; i < a->n_words; i++)
		if (a->crc[i] != b->crc[i])
			return false;

	return true;
}

char *igt_crc_to_string(igt_crc_t *crc)
{
	char buf[128];

	if (crc->n_words == 5)
		sprintf(buf, "%08x %08x %08x %08x %08x", crc->crc[0],
			crc->crc[1], crc->crc[2], crc->crc[3], crc->crc[4]);
	else
		igt_assert(0);

	return strdup(buf);
}

/* (6 fields, 8 chars each, space separated (5) + '\n') */
#define PIPE_CRC_LINE_LEN       (6 * 8 + 5 + 1)
/* account for \'0' */
#define PIPE_CRC_BUFFER_LEN     (PIPE_CRC_LINE_LEN + 1)

struct _igt_pipe_crc {
	int drm_fd;

	int ctl_fd;
	int crc_fd;
	int line_len;
	int buffer_len;

	enum pipe pipe;
	enum intel_pipe_crc_source source;
};

static const char *pipe_crc_sources[] = {
	"none",
	"plane1",
	"plane2",
	"pf",
	"pipe",
	"TV",
	"DP-B",
	"DP-C",
	"DP-D",
	"auto"
};

static const char *pipe_crc_source_name(enum intel_pipe_crc_source source)
{
        return pipe_crc_sources[source];
}

static bool igt_pipe_crc_do_start(igt_pipe_crc_t *pipe_crc)
{
	char buf[64];

	sprintf(buf, "pipe %c %s", pipe_name(pipe_crc->pipe),
		pipe_crc_source_name(pipe_crc->source));
	errno = 0;
	write(pipe_crc->ctl_fd, buf, strlen(buf));
	if (errno != 0)
		return false;

	return true;
}

static void igt_pipe_crc_pipe_off(int fd, enum pipe pipe)
{
	char buf[32];

	sprintf(buf, "pipe %c none", pipe_name(pipe));
	write(fd, buf, strlen(buf));
}

static void igt_pipe_crc_reset(void)
{
	igt_debugfs_t debugfs;
	int fd;

	igt_debugfs_init(&debugfs);
	fd = igt_debugfs_open(&debugfs, "i915_display_crc_ctl", O_WRONLY);

	igt_pipe_crc_pipe_off(fd, PIPE_A);
	igt_pipe_crc_pipe_off(fd, PIPE_B);
	igt_pipe_crc_pipe_off(fd, PIPE_C);

	close(fd);
}

static void pipe_crc_exit_handler(int sig)
{
	igt_pipe_crc_reset();
}

void igt_pipe_crc_check(igt_debugfs_t *debugfs)
{
	const char *cmd = "pipe A none";
	FILE *ctl;
	size_t written;
	int ret;

	ctl = igt_debugfs_fopen(debugfs, "i915_display_crc_ctl", "r+");
	igt_require_f(ctl,
		      "No display_crc_ctl found, kernel too old\n");
	written = fwrite(cmd, 1, strlen(cmd), ctl);
	ret = fflush(ctl);
	igt_require_f((written == strlen(cmd) && ret == 0) || errno != ENODEV,
		      "CRCs not supported on this platform\n");

	fclose(ctl);
}

igt_pipe_crc_t *
igt_pipe_crc_new(igt_debugfs_t *debugfs, int drm_fd, enum pipe pipe,
		 enum intel_pipe_crc_source source)
{
	igt_pipe_crc_t *pipe_crc;
	char buf[128];

	igt_install_exit_handler(pipe_crc_exit_handler);

	pipe_crc = calloc(1, sizeof(struct _igt_pipe_crc));

	pipe_crc->ctl_fd = igt_debugfs_open(debugfs,
					    "i915_display_crc_ctl", O_WRONLY);
	igt_assert(pipe_crc->ctl_fd != -1);

	sprintf(buf, "i915_pipe_%c_crc", pipe_name(pipe));
	pipe_crc->crc_fd = igt_debugfs_open(debugfs, buf, O_RDONLY);
	igt_assert(pipe_crc->crc_fd != -1);

	pipe_crc->line_len = PIPE_CRC_LINE_LEN;
	pipe_crc->buffer_len = PIPE_CRC_BUFFER_LEN;
	pipe_crc->drm_fd = drm_fd;
	pipe_crc->pipe = pipe;
	pipe_crc->source = source;

	/* make sure this source is actually supported */
	if (!igt_pipe_crc_do_start(pipe_crc)) {
		igt_pipe_crc_free(pipe_crc);
		return NULL;
	}

	igt_pipe_crc_stop(pipe_crc);

	return pipe_crc;
}

void igt_pipe_crc_free(igt_pipe_crc_t *pipe_crc)
{
	if (!pipe_crc)
		return;

	close(pipe_crc->ctl_fd);
	close(pipe_crc->crc_fd);
	free(pipe_crc);
}

void igt_pipe_crc_start(igt_pipe_crc_t *pipe_crc)
{
	igt_crc_t *crcs = NULL;

	igt_assert(igt_pipe_crc_do_start(pipe_crc));

	/*
	 * For some no yet identified reason, the first CRC is bonkers. So
	 * let's just wait for the next vblank and read out the buggy result.
	 */
	igt_pipe_crc_get_crcs(pipe_crc, 1, &crcs);
	free(crcs);
}

void igt_pipe_crc_stop(igt_pipe_crc_t *pipe_crc)
{
	char buf[32];

	sprintf(buf, "pipe %c none", pipe_name(pipe_crc->pipe));
	write(pipe_crc->ctl_fd, buf, strlen(buf));
}

static bool pipe_crc_init_from_string(igt_crc_t *crc, const char *line)
{
	int n;

	crc->n_words = 5;
	n = sscanf(line, "%8u %8x %8x %8x %8x %8x", &crc->frame, &crc->crc[0],
		   &crc->crc[1], &crc->crc[2], &crc->crc[3], &crc->crc[4]);
	return n == 6;
}

static bool read_one_crc(igt_pipe_crc_t *pipe_crc, igt_crc_t *out)
{
	ssize_t bytes_read;
	char buf[pipe_crc->buffer_len];

	bytes_read = read(pipe_crc->crc_fd, &buf, pipe_crc->line_len);
	igt_assert_cmpint(bytes_read, ==, pipe_crc->line_len);
	buf[bytes_read] = '\0';

	if (!pipe_crc_init_from_string(out, buf))
		return false;

	return true;
}

/*
 * Read @n_crcs from the @pipe_crc. This function blocks until @n_crcs are
 * retrieved.
 */
void
igt_pipe_crc_get_crcs(igt_pipe_crc_t *pipe_crc, int n_crcs,
		      igt_crc_t **out_crcs)
{
	igt_crc_t *crcs;
	int n = 0;

	crcs = calloc(n_crcs, sizeof(igt_crc_t));

	do {
		igt_crc_t *crc = &crcs[n];

		if (!read_one_crc(pipe_crc, crc))
			continue;

		n++;
	} while (n < n_crcs);

	*out_crcs = crcs;
}

/*
 * Read 1 CRC from @pipe_crc. This function blocks until the CRC is retrieved.
 * @out_crc must be allocated by the caller.
 *
 * This function takes care of the pipe_crc book-keeping, it will start/stop
 * the collection of the CRC.
 */
void igt_pipe_crc_collect_crc(igt_pipe_crc_t *pipe_crc, igt_crc_t *out_crc)
{
	igt_pipe_crc_start(pipe_crc);
	read_one_crc(pipe_crc, out_crc);
	igt_pipe_crc_stop(pipe_crc);
}

/*
 * Drop caches
 */

void igt_drop_caches_set(uint64_t val)
{
	igt_debugfs_t debugfs;
	int fd;
	char data[19];
	size_t nbytes;

	sprintf(data, "0x%" PRIx64, val);

	igt_debugfs_init(&debugfs);
	fd = igt_debugfs_open(&debugfs, "i915_gem_drop_caches", O_WRONLY);

	igt_assert(fd >= 0);
	nbytes = write(fd, data, strlen(data) + 1);
	igt_assert(nbytes == strlen(data) + 1);
	close(fd);
}

/*
 * Prefault control
 */

#define PREFAULT_DEBUGFS "/sys/module/i915/parameters/prefault_disable"
static void igt_prefault_control(bool enable)
{
	const char *name = PREFAULT_DEBUGFS;
	int fd;
	char buf[2] = {'Y', 'N'};
	int index;

	fd = open(name, O_RDWR);
	igt_require(fd >= 0);

	if (enable)
		index = 1;
	else
		index = 0;

	igt_require(write(fd, &buf[index], 1) == 1);

	close(fd);
}

static void enable_prefault_at_exit(int sig)
{
	igt_enable_prefault();
}

void igt_disable_prefault(void)
{
	igt_prefault_control(false);

	igt_install_exit_handler(enable_prefault_at_exit);
}

void igt_enable_prefault(void)
{
	igt_prefault_control(true);
}
