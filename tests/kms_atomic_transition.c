/*
 * Copyright © 2016 Intel Corporation
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
 */

#include "igt.h"
#include "igt_rand.h"
#include "drmtest.h"
#include "sw_sync.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <poll.h>

#ifndef DRM_CAP_CURSOR_WIDTH
#define DRM_CAP_CURSOR_WIDTH 0x8
#endif
#ifndef DRM_CAP_CURSOR_HEIGHT
#define DRM_CAP_CURSOR_HEIGHT 0x9
#endif

struct plane_parms {
	struct igt_fb *fb;
	uint32_t width, height, mask;
};

typedef struct {
	int drm_fd;
	struct igt_fb fb, argb_fb, sprite_fb;
	igt_display_t display;
	bool extended;
} data_t;

/* globals for fence support */
int *timeline;
pthread_t *thread;
int *seqno;

static void
run_primary_test(data_t *data, enum pipe pipe, igt_output_t *output)
{
	drmModeModeInfo *mode;
	igt_plane_t *primary;
	igt_fb_t fb;
	int i, ret;
	unsigned flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;

	igt_output_set_pipe(output, pipe);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	mode = igt_output_get_mode(output);

	igt_plane_set_fb(primary, NULL);
	ret = igt_display_try_commit_atomic(&data->display, flags, NULL);
	igt_skip_on_f(ret == -EINVAL, "Primary plane cannot be disabled separately from output\n");

	igt_create_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
		      DRM_FORMAT_XRGB8888, LOCAL_DRM_FORMAT_MOD_NONE, &fb);

	igt_plane_set_fb(primary, &fb);

	for (i = 0; i < 4; i++) {
		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		if (!(i & 1))
			igt_wait_for_vblank(data->drm_fd,
					data->display.pipes[pipe].crtc_offset);

		igt_plane_set_fb(primary, (i & 1) ? &fb : NULL);
		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		if (i & 1)
			igt_wait_for_vblank(data->drm_fd,
					data->display.pipes[pipe].crtc_offset);

		igt_plane_set_fb(primary, (i & 1) ? NULL : &fb);
	}

	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_remove_fb(data->drm_fd, &fb);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);
}

static void *fence_inc_thread(void *arg)
{
	int t = *((int *) arg);

	pthread_detach(pthread_self());

	usleep(5000);
	sw_sync_timeline_inc(t, 1);
	return NULL;
}

static void configure_fencing(igt_plane_t *plane)
{
	int i, fd, ret;

	i = plane->index;

	seqno[i]++;
	fd = sw_sync_timeline_create_fence(timeline[i], seqno[i]);
	igt_plane_set_fence_fd(plane, fd);
	close(fd);
	ret = pthread_create(&thread[i], NULL, fence_inc_thread, &timeline[i]);
	igt_assert_eq(ret, 0);
}

static bool skip_plane(data_t *data, igt_plane_t *plane)
{
	int index = plane->index;

	if (data->extended)
		return false;

	if (!is_i915_device(data->drm_fd))
		return false;

	if (plane->type == DRM_PLANE_TYPE_CURSOR)
		return false;

	if (intel_display_ver(intel_get_drm_devid(data->drm_fd)) < 11)
		return false;

	/*
	 * Test 1 HDR plane, 1 SDR UV plane, 1 SDR Y plane.
	 *
	 * Kernel registers planes in the hardware Z order:
	 * 0,1,2 HDR planes
	 * 3,4 SDR UV planes
	 * 5,6 SDR Y planes
	 */
	return index != 0 && index != 3 && index != 5;
}

static int
wm_setup_plane(data_t *data, enum pipe pipe,
	       uint32_t mask, struct plane_parms *parms, bool fencing)
{
	igt_plane_t *plane;
	int planes_set_up = 0;

	/*
	* Make sure these buffers are suited for display use
	* because most of the modeset operations must be fast
	* later on.
	*/
	for_each_plane_on_pipe(&data->display, pipe, plane) {
		int i = plane->index;

		if (skip_plane(data, plane))
			continue;

		if (!mask || !(parms[i].mask & mask)) {
			if (plane->values[IGT_PLANE_FB_ID]) {
				igt_plane_set_fb(plane, NULL);
				planes_set_up++;
			}
			continue;
		}

		if (fencing)
			configure_fencing(plane);

		igt_plane_set_fb(plane, parms[i].fb);
		igt_fb_set_size(parms[i].fb, plane, parms[i].width, parms[i].height);
		igt_plane_set_size(plane, parms[i].width, parms[i].height);

		planes_set_up++;
	}
	return planes_set_up;
}

static void ev_page_flip(int fd, unsigned seq, unsigned tv_sec, unsigned tv_usec, void *user_data)
{
	igt_debug("Retrieved vblank seq: %u on unk\n", seq);
}

static drmEventContext drm_events = {
	.version = 2,
	.page_flip_handler = ev_page_flip
};

enum transition_type {
	TRANSITION_PLANES,
	TRANSITION_AFTER_FREE,
	TRANSITION_MODESET,
	TRANSITION_MODESET_FAST,
	TRANSITION_MODESET_DISABLE,
};

static void set_sprite_wh(data_t *data, enum pipe pipe,
			  struct plane_parms *parms, struct igt_fb *sprite_fb,
			  bool alpha, unsigned w, unsigned h)
{
	igt_plane_t *plane;

	for_each_plane_on_pipe(&data->display, pipe, plane) {
		int i = plane->index;

		if (plane->type == DRM_PLANE_TYPE_PRIMARY ||
		    plane->type == DRM_PLANE_TYPE_CURSOR)
			continue;

		if (!parms[i].mask)
			continue;

		parms[i].width = w;
		parms[i].height = h;
	}

	igt_remove_fb(data->drm_fd, sprite_fb);
	igt_create_fb(data->drm_fd, w, h,
		      alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888,
		      LOCAL_DRM_FORMAT_MOD_NONE, sprite_fb);
}

#define is_atomic_check_failure_errno(errno) \
		(errno != -EINVAL && errno != 0)

#define is_atomic_check_plane_size_errno(errno) \
		(errno == -EINVAL)

static void setup_parms(data_t *data, enum pipe pipe,
			const drmModeModeInfo *mode,
			struct igt_fb *primary_fb,
			struct igt_fb *argb_fb,
			struct igt_fb *sprite_fb,
			struct plane_parms *parms,
			unsigned *iter_max)
{
	uint64_t cursor_width, cursor_height;
	unsigned sprite_width, sprite_height, prev_w, prev_h;
	bool max_sprite_width, max_sprite_height, alpha = true;
	uint32_t n_planes = data->display.pipes[pipe].n_planes;
	uint32_t n_overlays = 0, overlays[n_planes];
	igt_plane_t *plane;
	uint32_t iter_mask = 3;

	do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_WIDTH, &cursor_width));
	if (cursor_width >= mode->hdisplay)
		cursor_width = mode->hdisplay;

	do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_HEIGHT, &cursor_height));
	if (cursor_height >= mode->vdisplay)
		cursor_height = mode->vdisplay;

	for_each_plane_on_pipe(&data->display, pipe, plane) {
		int i = plane->index;

		if (plane->type == DRM_PLANE_TYPE_PRIMARY) {
			parms[i].fb = primary_fb;
			parms[i].width = mode->hdisplay;
			parms[i].height = mode->vdisplay;
			parms[i].mask = 1 << 0;
		} else if (plane->type == DRM_PLANE_TYPE_CURSOR) {
			parms[i].fb = argb_fb;
			parms[i].width = cursor_width;
			parms[i].height = cursor_height;
			parms[i].mask = 1 << 1;
		} else {
			if (!n_overlays)
				alpha = igt_plane_has_format_mod(plane,
					DRM_FORMAT_ARGB8888, LOCAL_DRM_FORMAT_MOD_NONE);
			parms[i].fb = sprite_fb;
			parms[i].mask = 1 << 2;

			iter_mask |= 1 << 2;

			overlays[n_overlays++] = i;
		}
	}

	if (n_overlays >= 2) {
		uint32_t i;

		/*
		 * Create 2 groups for overlays, make sure 1 plane is put
		 * in each then spread the rest out.
		 */
		iter_mask |= 1 << 3;
		parms[overlays[n_overlays - 1]].mask = 1 << 3;

		for (i = 1; i < n_overlays - 1; i++) {
			int val = hars_petruska_f54_1_random_unsafe_max(2);

			parms[overlays[i]].mask = 1 << (2 + val);
		}
	}

	igt_create_fb(data->drm_fd, cursor_width, cursor_height,
		      DRM_FORMAT_ARGB8888, LOCAL_DRM_FORMAT_MOD_NONE, argb_fb);

	igt_create_fb(data->drm_fd, cursor_width, cursor_height,
		      DRM_FORMAT_ARGB8888, LOCAL_DRM_FORMAT_MOD_NONE, sprite_fb);

	*iter_max = iter_mask + 1;
	if (!n_overlays)
		return;

	/*
	 * Pre gen9 not all sizes are supported, find the biggest possible
	 * size that can be enabled on all sprite planes.
	 */
	prev_w = sprite_width = cursor_width;
	prev_h = sprite_height = cursor_height;

	max_sprite_width = (sprite_width == mode->hdisplay);
	max_sprite_height = (sprite_height == mode->vdisplay);

	while (!max_sprite_width && !max_sprite_height) {
		int ret;

		set_sprite_wh(data, pipe, parms, sprite_fb,
			      alpha, sprite_width, sprite_height);

		wm_setup_plane(data, pipe, (1 << n_planes) - 1, parms, false);
		ret = igt_display_try_commit_atomic(&data->display, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		igt_assert(!is_atomic_check_failure_errno(ret));

		if (!is_atomic_check_plane_size_errno(ret)) {
			prev_w = sprite_width;
			prev_h = sprite_height;
			sprite_width *= max_sprite_width ? 1 : 2;
			if (sprite_width >= mode->hdisplay) {
				max_sprite_width = true;

				sprite_width = mode->hdisplay;
			}

			sprite_height *= max_sprite_height ? 1 : 2;
			if (sprite_height >= mode->vdisplay) {
				max_sprite_height = true;

				sprite_height = mode->vdisplay;
			}
			continue;
		}

		if (cursor_width == sprite_width &&
		    cursor_height == sprite_height) {
			igt_plane_t *removed_plane = NULL;
			igt_assert_f(n_planes >= 3, "No planes left to proceed with!");
			if (n_overlays > 0) {
				uint32_t plane_to_remove = hars_petruska_f54_1_random_unsafe_max(n_overlays);
				removed_plane = &data->display.pipes[pipe].planes[overlays[plane_to_remove]];
				igt_plane_set_fb(removed_plane, NULL);
				while (plane_to_remove < (n_overlays - 1)) {
					overlays[plane_to_remove] = overlays[plane_to_remove + 1];
					plane_to_remove++;
				}
				n_overlays--;
			}
			if (removed_plane) {
				parms[removed_plane->index].mask = 0;
				igt_info("Removed plane %d\n", removed_plane->index);
			}
			n_planes--;
			igt_info("Reduced available planes to %d\n", n_planes);
			continue;
		}

		sprite_width = prev_w;
		sprite_height = prev_h;

		if (!max_sprite_width)
			max_sprite_width = true;
		else
			max_sprite_height = true;
	}

	set_sprite_wh(data, pipe, parms, sprite_fb,
			alpha, sprite_width, sprite_height);

	igt_info("Running test on pipe %s with resolution %dx%d and sprite size %dx%d alpha %i\n",
		 kmstest_pipe_name(pipe), mode->hdisplay, mode->vdisplay,
		 sprite_width, sprite_height, alpha);
}

static void prepare_fencing(data_t *data, enum pipe pipe)
{
	igt_plane_t *plane;
	int n_planes;

	igt_require_sw_sync();

	n_planes = data->display.pipes[pipe].n_planes;
	timeline = calloc(sizeof(*timeline), n_planes);
	igt_assert_f(timeline != NULL, "Failed to allocate memory for timelines\n");
	thread = calloc(sizeof(*thread), n_planes);
	igt_assert_f(thread != NULL, "Failed to allocate memory for thread\n");
	seqno = calloc(sizeof(*seqno), n_planes);
	igt_assert_f(seqno != NULL, "Failed to allocate memory for seqno\n");

	for_each_plane_on_pipe(&data->display, pipe, plane)
		timeline[plane->index] = sw_sync_timeline_create();
}

static void unprepare_fencing(data_t *data, enum pipe pipe)
{
	igt_plane_t *plane;

	/* Make sure these got allocated in the first place */
	if (!timeline)
		return;

	for_each_plane_on_pipe(&data->display, pipe, plane)
		close(timeline[plane->index]);

	free(timeline);
	free(thread);
	free(seqno);
}

static void atomic_commit(data_t *data_v, enum pipe pipe, unsigned int flags, void *data, bool fencing)
{
	if (fencing)
		igt_pipe_request_out_fence(&data_v->display.pipes[pipe]);

	igt_display_commit_atomic(&data_v->display, flags, data);
}

static int fd_completed(int fd)
{
	struct pollfd pfd = { fd, POLLIN };
	int ret;

	ret = poll(&pfd, 1, 0);
	igt_assert(ret >= 0);
	return ret;
}

static void wait_for_transition(data_t *data, enum pipe pipe, bool nonblocking, bool fencing)
{
	if (fencing) {
		int fence_fd = data->display.pipes[pipe].out_fence_fd;

		if (!nonblocking)
			igt_assert(fd_completed(fence_fd));

		igt_assert(sync_fence_wait(fence_fd, 30000) == 0);
	} else {
		if (!nonblocking)
			igt_assert(fd_completed(data->drm_fd));

		drmHandleEvent(data->drm_fd, &drm_events);
	}
}

/*
 * 1. Set primary plane to a known fb.
 * 2. Make sure getcrtc returns the correct fb id.
 * 3. Call rmfb on the fb.
 * 4. Make sure getcrtc returns 0 fb id.
 *
 * RMFB is supposed to free the framebuffers from any and all planes,
 * so test this and make sure it works.
 */
static void
run_transition_test(data_t *data, enum pipe pipe, igt_output_t *output,
		enum transition_type type, bool nonblocking, bool fencing)
{
	drmModeModeInfo *mode, override_mode;
	igt_plane_t *plane;
	igt_pipe_t *pipe_obj = &data->display.pipes[pipe];
	uint32_t iter_max, i;
	struct plane_parms parms[pipe_obj->n_planes];
	unsigned flags = 0;
	int ret;

	if (fencing)
		prepare_fencing(data, pipe);
	else
		flags |= DRM_MODE_PAGE_FLIP_EVENT;

	if (nonblocking)
		flags |= DRM_MODE_ATOMIC_NONBLOCK;

	if (type >= TRANSITION_MODESET)
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

	mode = igt_output_get_mode(output);
	override_mode = *mode;
	/* try to force a modeset */
	override_mode.flags ^= DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC;

	igt_create_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
		      DRM_FORMAT_XRGB8888, LOCAL_DRM_FORMAT_MOD_NONE, &data->fb);

	igt_output_set_pipe(output, pipe);

	wm_setup_plane(data, pipe, 0, NULL, false);

	if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET) {
		igt_output_set_pipe(output, PIPE_NONE);

		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		igt_output_set_pipe(output, pipe);
	}

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	setup_parms(data, pipe, mode, &data->fb, &data->argb_fb, &data->sprite_fb, parms, &iter_max);

	/*
	 * In some configurations the tests may not run to completion with all
	 * sprite planes lit up at 4k resolution, try decreasing width/size of secondary
	 * planes to fix this
	 */
	while (1) {
		wm_setup_plane(data, pipe, iter_max - 1, parms, false);

		if (fencing)
			igt_pipe_request_out_fence(pipe_obj);

		ret = igt_display_try_commit_atomic(&data->display, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		igt_assert(!is_atomic_check_failure_errno(ret));

		if (!is_atomic_check_plane_size_errno(ret) || pipe_obj->n_planes < 3)
			break;

		ret = 0;
		for_each_plane_on_pipe(&data->display, pipe, plane) {
			i = plane->index;

			if (plane->type == DRM_PLANE_TYPE_PRIMARY ||
			    plane->type == DRM_PLANE_TYPE_CURSOR)
				continue;

			parms[i].width /= 2;
			ret = 1;
			igt_info("Reducing sprite %i to %ux%u\n", i - 1, parms[i].width, parms[i].height);
			break;
		}

		if (!ret)
			igt_skip("Cannot run tests without proper size sprite planes\n");
	}

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	if (type == TRANSITION_AFTER_FREE) {
		int fence_fd = -1;

		wm_setup_plane(data, pipe, 0, parms, fencing);

		atomic_commit(data, pipe, flags, (void *)(unsigned long)0, fencing);
		if (fencing) {
			fence_fd = pipe_obj->out_fence_fd;
			pipe_obj->out_fence_fd = -1;
		}

		/* force planes to be part of commit */
		for_each_plane_on_pipe(&data->display, pipe, plane) {
			if (parms[plane->index].mask)
				igt_plane_set_position(plane, 0, 0);
		}

		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		if (fence_fd != -1) {
			igt_assert(fd_completed(fence_fd));
			close(fence_fd);
		} else {
			igt_assert(fd_completed(data->drm_fd));
			wait_for_transition(data, pipe, false, fencing);
		}
		return;
	}

	for (i = 0; i < iter_max; i++) {
		int n_enable_planes = igt_hweight(i);

		if (type == TRANSITION_MODESET_FAST &&
		    n_enable_planes > 1 &&
		    n_enable_planes < pipe_obj->n_planes)
			continue;

		igt_output_set_pipe(output, pipe);

		if (!wm_setup_plane(data, pipe, i, parms, fencing))
			continue;

		atomic_commit(data, pipe, flags, (void *)(unsigned long)i, fencing);
		wait_for_transition(data, pipe, nonblocking, fencing);

		if (type == TRANSITION_MODESET_DISABLE) {
			igt_output_set_pipe(output, PIPE_NONE);

			if (!wm_setup_plane(data, pipe, 0, parms, fencing))
				continue;

			atomic_commit(data, pipe, flags, (void *) 0UL, fencing);
			wait_for_transition(data, pipe, nonblocking, fencing);
		} else {
			uint32_t j;

			/* i -> i+1 will be done when i increases, can be skipped here */
			for (j = iter_max - 1; j > i + 1; j--) {
				n_enable_planes = igt_hweight(j);

				if (type == TRANSITION_MODESET_FAST &&
				    n_enable_planes > 1 &&
				    n_enable_planes < pipe_obj->n_planes)
					continue;

				if (!wm_setup_plane(data, pipe, j, parms, fencing))
					continue;

				if (type >= TRANSITION_MODESET)
					igt_output_override_mode(output, &override_mode);

				atomic_commit(data, pipe, flags, (void *)(unsigned long) j, fencing);
				wait_for_transition(data, pipe, nonblocking, fencing);

				if (!wm_setup_plane(data, pipe, i, parms, fencing))
					continue;

				if (type >= TRANSITION_MODESET)
					igt_output_override_mode(output, NULL);

				atomic_commit(data, pipe, flags, (void *)(unsigned long) i, fencing);
				wait_for_transition(data, pipe, nonblocking, fencing);
			}
		}
	}
}

static void test_cleanup(data_t *data, enum pipe pipe, igt_output_t *output, bool fencing)
{
	igt_plane_t *plane;

	if (fencing)
		unprepare_fencing(data, pipe);

	igt_output_set_pipe(output, PIPE_NONE);

	for_each_plane_on_pipe(&data->display, pipe, plane)
		igt_plane_set_fb(plane, NULL);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_remove_fb(data->drm_fd, &data->fb);
	igt_remove_fb(data->drm_fd, &data->argb_fb);
	igt_remove_fb(data->drm_fd, &data->sprite_fb);
}

static void commit_display(data_t *data, unsigned event_mask, bool nonblocking)
{
	unsigned flags;
	int num_events = igt_hweight(event_mask);
	ssize_t ret;

	flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
	if (nonblocking)
		flags |= DRM_MODE_ATOMIC_NONBLOCK;

	igt_display_commit_atomic(&data->display, flags, NULL);

	igt_debug("Event mask: %x, waiting for %i events\n", event_mask, num_events);

	igt_set_timeout(30, "Waiting for events timed out\n");

	while (num_events) {
		char buf[32];
		struct drm_event *e = (void *)buf;
		struct drm_event_vblank *vblank = (void *)buf;

		igt_set_timeout(3, "Timed out while reading drm_fd\n");
		ret = read(data->drm_fd, buf, sizeof(buf));
		igt_reset_timeout();
		if (ret < 0 && (errno == EINTR || errno == EAGAIN))
			continue;

		igt_assert(ret >= 0);
		igt_assert_eq(e->type, DRM_EVENT_FLIP_COMPLETE);

		igt_debug("Retrieved vblank seq: %u on unk/unk\n", vblank->sequence);

		num_events--;
	}

	igt_reset_timeout();
}

static unsigned set_combinations(data_t *data, unsigned mask, struct igt_fb *fb)
{
	igt_output_t *output;
	enum pipe pipe;
	unsigned event_mask = 0;
	int i;

	for (i = 0; i < data->display.n_outputs; i++)
		igt_output_set_pipe(&data->display.outputs[i], PIPE_NONE);

	for_each_pipe(&data->display, pipe) {
		igt_plane_t *plane = igt_pipe_get_plane_type(&data->display.pipes[pipe],
			DRM_PLANE_TYPE_PRIMARY);
		drmModeModeInfo *mode = NULL;

		if (!(mask & (1 << pipe))) {
			if (igt_pipe_is_prop_changed(&data->display, pipe, IGT_CRTC_ACTIVE)) {
				event_mask |= 1 << pipe;
				igt_plane_set_fb(plane, NULL);
			}

			continue;
		}

		event_mask |= 1 << pipe;

		for_each_valid_output_on_pipe(&data->display, pipe, output) {
			if (output->pending_pipe != PIPE_NONE)
				continue;

			mode = igt_output_get_mode(output);
			break;
		}

		if (!mode)
			return 0;

		igt_output_set_pipe(output, pipe);
		igt_plane_set_fb(plane, fb);
		igt_fb_set_size(fb, plane, mode->hdisplay, mode->vdisplay);
		igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);
	}

	return event_mask;
}

static void refresh_primaries(data_t  *data, int mask)
{
	enum pipe pipe;
	igt_plane_t *plane;

	for_each_pipe(&data->display, pipe) {
		if (!((1 << pipe) & mask))
			continue;

		for_each_plane_on_pipe(&data->display, pipe, plane)
			if (plane->type == DRM_PLANE_TYPE_PRIMARY)
				igt_plane_set_position(plane, 0, 0);
	}
}

static void collect_crcs_mask(igt_pipe_crc_t **pipe_crcs, unsigned mask, igt_crc_t *crcs)
{
	int i;

	for (i = 0; i < IGT_MAX_PIPES; i++) {
		if (!((1 << i) & mask))
			continue;

		if (!pipe_crcs[i])
			continue;

		igt_pipe_crc_collect_crc(pipe_crcs[i], &crcs[i]);
	}
}

static void run_modeset_tests(data_t *data, int howmany, bool nonblocking, bool fencing)
{
	struct igt_fb fbs[2];
	int i, j;
	unsigned iter_max;
	igt_pipe_crc_t *pipe_crcs[IGT_MAX_PIPES] = { 0 };
	igt_output_t *output;
	unsigned width = 0, height = 0;

	for (i = 0; i < data->display.n_outputs; i++)
		igt_output_set_pipe(&data->display.outputs[i], PIPE_NONE);

retry:
	j = 0;
	for_each_connected_output(&data->display, output) {
		drmModeModeInfo *mode = igt_output_get_mode(output);

		width = max(width, mode->hdisplay);
		height = max(height, mode->vdisplay);
	}

	igt_create_pattern_fb(data->drm_fd, width, height,
				   DRM_FORMAT_XRGB8888, 0, &fbs[0]);
	igt_create_color_pattern_fb(data->drm_fd, width, height,
				    DRM_FORMAT_XRGB8888, 0, .5, .5, .5, &fbs[1]);

	for_each_pipe(&data->display, i) {
		igt_pipe_t *pipe = &data->display.pipes[i];
		igt_plane_t *plane = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);
		drmModeModeInfo *mode = NULL;

		/* count enable pipes to set max iteration */
		j += 1;

		if (is_i915_device(data->drm_fd))
			pipe_crcs[i] = igt_pipe_crc_new(data->drm_fd, i, INTEL_PIPE_CRC_SOURCE_AUTO);

		for_each_valid_output_on_pipe(&data->display, i, output) {
			if (output->pending_pipe != PIPE_NONE)
				continue;

			igt_output_set_pipe(output, i);
			mode = igt_output_get_mode(output);
			break;
		}

		if (mode) {
			igt_plane_set_fb(plane, &fbs[1]);
			igt_fb_set_size(&fbs[1], plane, mode->hdisplay, mode->vdisplay);
			igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);

			if (fencing)
				igt_pipe_request_out_fence(&data->display.pipes[i]);
		} else {
			igt_plane_set_fb(plane, NULL);
		}
	}

	iter_max = 1 << j;

	if (igt_display_try_commit_atomic(&data->display,
				DRM_MODE_ATOMIC_TEST_ONLY |
				DRM_MODE_ATOMIC_ALLOW_MODESET,
				NULL) != 0) {
		igt_output_t *out;
		bool found = igt_override_all_active_output_modes_to_fit_bw(&data->display);
		igt_require_f(found, "No valid mode combo found.\n");

		for_each_connected_output(&data->display, out)
			igt_output_set_pipe(out, PIPE_NONE);

		goto retry;
	}
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	for (i = 0; i < iter_max; i++) {
		igt_crc_t crcs[5][IGT_MAX_PIPES];
		unsigned event_mask;

		if (igt_hweight(i) > howmany)
			continue;

		event_mask = set_combinations(data, i, &fbs[0]);
		if (!event_mask && i)
			continue;

		commit_display(data, event_mask, nonblocking);

		collect_crcs_mask(pipe_crcs, i, crcs[0]);

		for (j = iter_max - 1; j > i + 1; j--) {
			if (igt_hweight(j) > howmany)
				continue;

			if (igt_hweight(i) < howmany && igt_hweight(j) < howmany)
				continue;

			event_mask = set_combinations(data, j, &fbs[1]);
			if (!event_mask)
				continue;

			commit_display(data, event_mask, nonblocking);

			collect_crcs_mask(pipe_crcs, j, crcs[1]);

			refresh_primaries(data, j);
			commit_display(data, j, nonblocking);
			collect_crcs_mask(pipe_crcs, j, crcs[2]);

			event_mask = set_combinations(data, i, &fbs[0]);
			if (!event_mask)
				continue;

			commit_display(data, event_mask, nonblocking);
			collect_crcs_mask(pipe_crcs, i, crcs[3]);

			refresh_primaries(data, i);
			commit_display(data, i, nonblocking);
			collect_crcs_mask(pipe_crcs, i, crcs[4]);

			if (!is_i915_device(data->drm_fd))
				continue;

			for (int k = 0; k < IGT_MAX_PIPES; k++) {
				if (i & (1 << k)) {
					igt_assert_crc_equal(&crcs[0][k], &crcs[3][k]);
					igt_assert_crc_equal(&crcs[0][k], &crcs[4][k]);
				}

				if (j & (1 << k))
					igt_assert_crc_equal(&crcs[1][k], &crcs[2][k]);
			}
		}
	}

	set_combinations(data, 0, NULL);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	if (is_i915_device(data->drm_fd)) {
		for_each_pipe(&data->display, i)
			igt_pipe_crc_free(pipe_crcs[i]);
	}

	igt_remove_fb(data->drm_fd, &fbs[1]);
	igt_remove_fb(data->drm_fd, &fbs[0]);
}

static void run_modeset_transition(data_t *data, int requested_outputs, bool nonblocking, bool fencing)
{
	igt_output_t *outputs[IGT_MAX_PIPES] = {};
	int num_outputs = 0;
	enum pipe pipe;

	for_each_pipe(&data->display, pipe) {
		igt_output_t *output;

		for_each_valid_output_on_pipe(&data->display, pipe, output) {
			int i;

			for (i = pipe - 1; i >= 0; i--)
				if (outputs[i] == output)
					break;

			if (i < 0) {
				outputs[pipe] = output;
				num_outputs++;
				break;
			}
		}
	}

	igt_require_f(num_outputs >= requested_outputs,
		      "Should have at least %i outputs, found %i\n",
		      requested_outputs, num_outputs);

	run_modeset_tests(data, requested_outputs, nonblocking, fencing);
}

static bool output_is_internal_panel(igt_output_t *output)
{
	switch (output->config.connector->connector_type) {
	case DRM_MODE_CONNECTOR_LVDS:
	case DRM_MODE_CONNECTOR_eDP:
	case DRM_MODE_CONNECTOR_DSI:
	case DRM_MODE_CONNECTOR_DPI:
		return true;
	default:
		return false;
	}
}

static int opt_handler(int opt, int opt_index, void *_data)
{
	data_t *data = _data;

	switch (opt) {
	case 'e':
		data->extended = true;
		break;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static const struct option long_opts[] = {
	{ .name = "extended", .has_arg = false, .val = 'e', },
	{}
};

static const char help_str[] =
	"  --extended\t\tRun the extended tests\n";

static data_t data;

igt_main_args("", long_opts, help_str, opt_handler, &data)
{
	igt_output_t *output;
	enum pipe pipe;
	int i, count = 0;
	int pipe_count = 0;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);

		igt_display_require_output(&data.display);

		for_each_connected_output(&data.display, output)
			count++;
	}

	igt_describe("Check toggling of primary plane with vblank");
	igt_subtest("plane-primary-toggle-with-vblank-wait") {
		for_each_pipe_with_valid_output(&data.display, pipe, output) {
			if (pipe_count == 2 * count && !data.extended)
				break;
			pipe_count++;
			run_primary_test(&data, pipe, output);

		}
		pipe_count = 0;
	}

	igt_describe("Transition test for all plane combinations");
	igt_subtest_with_dynamic("plane-all-transition") {
		for_each_pipe_with_valid_output(&data.display, pipe, output) {
			if (pipe_count == 2 * count && !data.extended)
				break;
			pipe_count++;
			igt_dynamic_f("%s-pipe-%s", igt_output_name(output), kmstest_pipe_name(pipe))
				run_transition_test(&data, pipe, output, TRANSITION_PLANES, false, false);
			test_cleanup(&data, pipe, output, false);
		}
		pipe_count = 0;
	}

	igt_describe("Transition test for all plane combinations with fencing commit");
	igt_subtest_with_dynamic("plane-all-transition-fencing") {
		for_each_pipe_with_valid_output(&data.display, pipe, output) {
			if (pipe_count == 2 * count && !data.extended)
				break;
			pipe_count++;
			igt_dynamic_f("%s-pipe-%s", igt_output_name(output), kmstest_pipe_name(pipe))
				run_transition_test(&data, pipe, output, TRANSITION_PLANES, false, true);
			test_cleanup(&data, pipe, output, true);
		}
		pipe_count = 0;
	}

	igt_describe("Transition test for all plane combinations with nonblocking commit");
	igt_subtest_with_dynamic("plane-all-transition-nonblocking") {
		for_each_pipe_with_valid_output(&data.display, pipe, output) {
			if (pipe_count == 2 * count && !data.extended)
				break;
			pipe_count++;
			igt_dynamic_f("%s-pipe-%s", igt_output_name(output), kmstest_pipe_name(pipe))
				run_transition_test(&data, pipe, output, TRANSITION_PLANES, true, false);
			test_cleanup(&data, pipe, output, false);
		}
		pipe_count = 0;
	}

	igt_describe("Transition test for all plane combinations with nonblocking and fencing commit");
	igt_subtest_with_dynamic("plane-all-transition-nonblocking-fencing") {
		for_each_pipe_with_valid_output(&data.display, pipe, output) {
			if (pipe_count == 2 * count && !data.extended)
				break;
			pipe_count++;
			igt_dynamic_f("%s-pipe-%s", igt_output_name(output), kmstest_pipe_name(pipe))
				run_transition_test(&data, pipe, output, TRANSITION_PLANES, true, true);
			test_cleanup(&data, pipe, output, true);
		}
		pipe_count = 0;
	}

	igt_describe("Transition test with non blocking commit and make sure commit of disabled plane has "
		       "to complete before atomic commit on that plane");
	igt_subtest_with_dynamic("plane-use-after-nonblocking-unbind") {
		for_each_pipe_with_valid_output(&data.display, pipe, output) {
			if (pipe_count == 2 * count && !data.extended)
				break;
			pipe_count++;
			igt_dynamic_f("%s-pipe-%s", igt_output_name(output), kmstest_pipe_name(pipe))
				run_transition_test(&data, pipe, output, TRANSITION_AFTER_FREE, true, false);
			test_cleanup(&data, pipe, output, false);
		}
		pipe_count = 0;
	}

	igt_describe("Transition test with non blocking and fencing commit and make sure commit of "
		       "disabled plane has to complete before atomic commit on that plane");
	igt_subtest_with_dynamic("plane-use-after-nonblocking-unbind-fencing") {
		for_each_pipe_with_valid_output(&data.display, pipe, output) {
			if (pipe_count == 2 * count && !data.extended)
				break;
			pipe_count++;
			igt_dynamic_f("%s-pipe-%s", igt_output_name(output), kmstest_pipe_name(pipe))
				run_transition_test(&data, pipe, output, TRANSITION_AFTER_FREE, true, true);
			test_cleanup(&data, pipe, output, true);
		}
		pipe_count = 0;
	}

	/*
	 * Test modeset cases on internal panels separately with a reduced
	 * number of combinations, to avoid long runtimes due to modesets on
	 * panels with long power cycle delays.
	 */
	igt_describe("Modeset test for all plane combinations");
	igt_subtest_with_dynamic("plane-all-modeset-transition") {
		for_each_pipe_with_valid_output(&data.display, pipe, output) {
			if (pipe_count == 2 * count && !data.extended)
				break;
			pipe_count++;
			if (output_is_internal_panel(output))
				continue;

			igt_dynamic_f("%s-pipe-%s", igt_output_name(output), kmstest_pipe_name(pipe))
				run_transition_test(&data, pipe, output, TRANSITION_MODESET, false, false);
			test_cleanup(&data, pipe, output, false);
		}
		pipe_count = 0;
	}

	igt_describe("Modeset test for all plane combinations with fencing commit");
	igt_subtest_with_dynamic("plane-all-modeset-transition-fencing") {
		for_each_pipe_with_valid_output(&data.display, pipe, output) {
			if (pipe_count == 2 * count && !data.extended)
				break;
			pipe_count++;
			if (output_is_internal_panel(output))
				continue;

			igt_dynamic_f("%s-pipe-%s", igt_output_name(output), kmstest_pipe_name(pipe))
				run_transition_test(&data, pipe, output, TRANSITION_MODESET, false, true);
			test_cleanup(&data, pipe, output, true);
		}
		pipe_count = 0;
	}

	igt_describe("Modeset test for all plane combinations on internal panels");
	igt_subtest_with_dynamic("plane-all-modeset-transition-internal-panels") {
		for_each_pipe_with_valid_output(&data.display, pipe, output) {
			if (pipe_count == 2 * count && !data.extended)
				break;
			pipe_count++;
			if (!output_is_internal_panel(output))
				continue;

			igt_dynamic_f("%s-pipe-%s", igt_output_name(output), kmstest_pipe_name(pipe))
				run_transition_test(&data, pipe, output, TRANSITION_MODESET_FAST, false, false);
			test_cleanup(&data, pipe, output, false);
		}
		pipe_count = 0;
	}

	igt_describe("Modeset test for all plane combinations on internal panels with fencing commit");
	igt_subtest_with_dynamic("plane-all-modeset-transition-fencing-internal-panels") {
		for_each_pipe_with_valid_output(&data.display, pipe, output) {
			if (pipe_count == 2 * count && !data.extended)
				break;
			pipe_count++;
			if (!output_is_internal_panel(output))
				continue;

			igt_dynamic_f("%s-pipe-%s", igt_output_name(output), kmstest_pipe_name(pipe))
				run_transition_test(&data, pipe, output, TRANSITION_MODESET_FAST, false, true);
			test_cleanup(&data, pipe, output, true);
		}
		pipe_count = 0;
	}

	igt_describe("Check toggling and modeset transition on plane");
	igt_subtest("plane-toggle-modeset-transition") {
		for_each_pipe_with_valid_output(&data.display, pipe, output) {
			if (pipe_count == 2 * count && !data.extended)
				break;
			pipe_count++;
			run_transition_test(&data, pipe, output, TRANSITION_MODESET_DISABLE, false, false);
			test_cleanup(&data, pipe, output, false);
		}
		pipe_count = 0;
	}

	igt_describe("Modeset transition tests for combinations of crtc enabled");
	igt_subtest_with_dynamic("modeset-transition") {
		for (i = 1; i <= count; i++) {
			igt_dynamic_f("%ix-outputs", i)
				run_modeset_transition(&data, i, false, false);
		}
	}

	igt_describe("Modeset transition tests for combinations of crtc enabled with nonblocking commit");
	igt_subtest_with_dynamic("modeset-transition-nonblocking") {
		for (i = 1; i <= count; i++) {
			igt_dynamic_f("%ix-outputs", i)
				run_modeset_transition(&data, i, true, false);
		}
	}

	igt_describe("Modeset transition tests for combinations of crtc enabled with fencing commit");
	igt_subtest_with_dynamic("modeset-transition-fencing") {
		for (i = 1; i <= count; i++) {
			igt_dynamic_f("%ix-outputs", i)
				run_modeset_transition(&data, i, false, true);
		}
	}

	igt_describe("Modeset transition tests for combinations of crtc enabled with nonblocking &"
		       " fencing commit");
	igt_subtest_with_dynamic("modeset-transition-nonblocking-fencing") {
		for (i = 1; i <= count; i++) {
			igt_dynamic_f("%ix-outputs", i)
				run_modeset_transition(&data, i, true, true);
		}
	}

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
