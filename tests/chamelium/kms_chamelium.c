/*
 * Copyright © 2016 Red Hat Inc.
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
 * Authors:
 *    Lyude Paul <lyude@redhat.com>
 */

#include "config.h"
#include "igt.h"
#include "igt_vc4.h"
#include "igt_edid.h"
#include "igt_eld.h"
#include "igt_infoframe.h"
#include "monitor_edids/dp_edids.h"
#include "monitor_edids/hdmi_edids.h"
#include "monitor_edids/monitor_edids_helper.h"

#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <stdatomic.h>
// #include <stdio.h>

// struct chamelium_edid;

enum test_modeset_mode {
	TEST_MODESET_ON,
	TEST_MODESET_ON_OFF,
	TEST_MODESET_OFF,
};

typedef struct {
	struct chamelium *chamelium;
	struct chamelium_port **ports;
	igt_display_t display;
	int port_count;

	int drm_fd;

	struct chamelium_edid *edids[IGT_CUSTOM_EDID_COUNT];
} data_t;

#define ONLINE_TIMEOUT 20 /* seconds */

#define HPD_STORM_PULSE_INTERVAL_DP 100 /* ms */
#define HPD_STORM_PULSE_INTERVAL_HDMI 200 /* ms */

#define HPD_TOGGLE_COUNT_VGA 5
#define HPD_TOGGLE_COUNT_DP_HDMI 15
#define HPD_TOGGLE_COUNT_FAST 3

static void
get_connectors_link_status_failed(data_t *data, bool *link_status_failed)
{
	drmModeConnector *connector;
	uint64_t link_status;
	drmModePropertyPtr prop;
	int p;

	for (p = 0; p < data->port_count; p++) {
		connector = chamelium_port_get_connector(data->chamelium,
							 data->ports[p], false);

		igt_assert(kmstest_get_property(data->drm_fd,
						connector->connector_id,
						DRM_MODE_OBJECT_CONNECTOR,
						"link-status", NULL,
						&link_status, &prop));

		link_status_failed[p] = link_status == DRM_MODE_LINK_STATUS_BAD;

		drmModeFreeProperty(prop);
		drmModeFreeConnector(connector);
	}
}

/* Wait for hotplug and return the remaining time left from timeout */
static bool wait_for_hotplug(struct udev_monitor *mon, int *timeout)
{
	struct timespec start, end;
	int elapsed;
	bool detected;

	igt_assert_eq(igt_gettime(&start), 0);
	detected = igt_hotplug_detected(mon, *timeout);
	igt_assert_eq(igt_gettime(&end), 0);

	elapsed = igt_time_elapsed(&start, &end);
	igt_assert_lte(0, elapsed);
	*timeout = max(0, *timeout - elapsed);

	return detected;
}

static void
wait_for_connector_after_hotplug(data_t *data, struct udev_monitor *mon,
				 struct chamelium_port *port,
				 drmModeConnection status)
{
	int timeout = CHAMELIUM_HOTPLUG_TIMEOUT;
	int hotplug_count = 0;

	igt_debug("Waiting for %s to get %s after a hotplug event...\n",
			  chamelium_port_get_name(port),
			  kmstest_connector_status_str(status));

	while (timeout > 0) {
		if (!wait_for_hotplug(mon, &timeout))
			break;

		hotplug_count++;

		if (chamelium_reprobe_connector(&data->display, data->chamelium,
						port) == status)
			return;
	}

	igt_assert_f(false, "Timed out waiting for %s to get %s after a hotplug. Current state %s hotplug_count %d\n",
			    chamelium_port_get_name(port),
			    kmstest_connector_status_str(status),
			    kmstest_connector_status_str(chamelium_reprobe_connector(&data->display, data->chamelium, port)), hotplug_count);
}


static int chamelium_vga_modes[][2] = {
	{ 1600, 1200 },
	{ 1920, 1200 },
	{ 1920, 1080 },
	{ 1680, 1050 },
	{ 1280, 1024 },
	{ 1280, 960 },
	{ 1440, 900 },
	{ 1280, 800 },
	{ 1024, 768 },
	{ 1360, 768 },
	{ 1280, 720 },
	{ 800, 600 },
	{ 640, 480 },
	{ -1, -1 },
};

static bool
prune_vga_mode(data_t *data, drmModeModeInfo *mode)
{
	int i = 0;

	while (chamelium_vga_modes[i][0] != -1) {
		if (mode->hdisplay == chamelium_vga_modes[i][0] &&
		    mode->vdisplay == chamelium_vga_modes[i][1])
			return false;

		i++;
	}

	return true;
}

static bool
check_analog_bridge(data_t *data, struct chamelium_port *port)
{
	drmModePropertyBlobPtr edid_blob = NULL;
	drmModeConnector *connector = chamelium_port_get_connector(
	    data->chamelium, port, false);
	uint64_t edid_blob_id;
	const struct edid *edid;
	char edid_vendor[3];

	if (chamelium_port_get_type(port) != DRM_MODE_CONNECTOR_VGA) {
		drmModeFreeConnector(connector);
		return false;
	}

	igt_assert(kmstest_get_property(data->drm_fd, connector->connector_id,
					DRM_MODE_OBJECT_CONNECTOR, "EDID", NULL,
					&edid_blob_id, NULL));
	igt_assert(edid_blob = drmModeGetPropertyBlob(data->drm_fd,
						      edid_blob_id));

	edid = (const struct edid *) edid_blob->data;
	edid_get_mfg(edid, edid_vendor);

	drmModeFreePropertyBlob(edid_blob);
	drmModeFreeConnector(connector);

	/* Analog bridges provide their own EDID */
	if (edid_vendor[0] != 'I' || edid_vendor[1] != 'G' ||
	    edid_vendor[2] != 'T')
		return true;

	return false;
}

static void chamelium_paint_xr24_pattern(uint32_t *data,
					 size_t width, size_t height,
					 size_t stride, size_t block_size)
{
	uint32_t colors[] = { 0xff000000,
			      0xffff0000,
			      0xff00ff00,
			      0xff0000ff,
			      0xffffffff };
	unsigned i, j;

	for (i = 0; i < height; i++)
		for (j = 0; j < width; j++)
			*(data + i * stride / 4 + j) = colors[((j / block_size) + (i / block_size)) % 5];
}

static int chamelium_get_pattern_fb(data_t *data, size_t width, size_t height,
				    uint32_t fourcc, size_t block_size,
				    struct igt_fb *fb)
{
	int fb_id;
	void *ptr;

	igt_assert(fourcc == DRM_FORMAT_XRGB8888);

	fb_id = igt_create_fb(data->drm_fd, width, height, fourcc,
			      DRM_FORMAT_MOD_LINEAR, fb);
	igt_assert(fb_id > 0);

	ptr = igt_fb_map_buffer(fb->fd, fb);
	igt_assert(ptr);

	chamelium_paint_xr24_pattern(ptr, width, height, fb->strides[0],
				     block_size);
	igt_fb_unmap_buffer(fb, ptr);

	return fb_id;
}

static void
enable_output(data_t *data,
	      struct chamelium_port *port,
	      igt_output_t *output,
	      drmModeModeInfo *mode,
	      struct igt_fb *fb)
{
	igt_display_t *display = output->display;
	igt_plane_t *primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	drmModeConnector *connector = chamelium_port_get_connector(
	    data->chamelium, port, false);

	igt_assert(primary);

	igt_plane_set_size(primary, mode->hdisplay, mode->vdisplay);
	igt_plane_set_fb(primary, fb);
	igt_output_override_mode(output, mode);

	/* Clear any color correction values that might be enabled */
	if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_DEGAMMA_LUT))
		igt_pipe_obj_replace_prop_blob(primary->pipe, IGT_CRTC_DEGAMMA_LUT, NULL, 0);
	if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_GAMMA_LUT))
		igt_pipe_obj_replace_prop_blob(primary->pipe, IGT_CRTC_GAMMA_LUT, NULL, 0);
	if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_CTM))
		igt_pipe_obj_replace_prop_blob(primary->pipe, IGT_CRTC_CTM, NULL, 0);

	igt_display_commit2(display, COMMIT_ATOMIC);

	if (chamelium_port_get_type(port) == DRM_MODE_CONNECTOR_VGA)
		usleep(250000);

	drmModeFreeConnector(connector);
}

static enum pipe get_pipe_for_output(igt_display_t *display, igt_output_t *output)
{
	enum pipe pipe;

	for_each_pipe(display, pipe) {
		if (igt_pipe_connector_valid(pipe, output)) {
			return pipe;
		}
	}

	igt_assert_f(false, "No pipe found for output %s\n",
		     igt_output_name(output));
}

static void create_fb_for_mode(data_t *data, struct igt_fb *fb, drmModeModeInfo *mode)
{
	int fb_id;

	fb_id = chamelium_get_pattern_fb(data, mode->hdisplay, mode->vdisplay,
					 DRM_FORMAT_XRGB8888, 64, fb);

	igt_assert(fb_id > 0);
}

static drmModeModeInfo get_mode_for_port(struct chamelium *chamelium,
					 struct chamelium_port *port)
{
	drmModeConnector *connector = chamelium_port_get_connector(chamelium,
								   port, false);
	drmModeModeInfo mode;
	igt_assert(&connector->modes[0] != NULL);
	memcpy(&mode, &connector->modes[0], sizeof(mode));
	drmModeFreeConnector(connector);
	return mode;
}

static igt_output_t *get_output_for_port(data_t *data,
					 struct chamelium_port *port)
{
	drmModeConnector *connector =
		chamelium_port_get_connector(data->chamelium, port, true);
	igt_output_t *output = igt_output_from_connector(&data->display,
							 connector);
	drmModeFreeConnector(connector);
	igt_assert(output != NULL);
	return output;
}

static const char test_hotplug_for_each_pipe_desc[] =
	"Check that we get uevents and updated connector status on "
	"hotplug and unplug for each pipe with valid output";
static void
test_hotplug_for_each_pipe(data_t *data, struct chamelium_port *port)
{
	igt_output_t *output;
	enum pipe pipe;
	struct udev_monitor *mon = igt_watch_uevents();

	chamelium_reset_state(&data->display,
			      data->chamelium,
			      port,
			      data->ports,
			      data->port_count);

	igt_hpd_storm_set_threshold(data->drm_fd, 0);
	/* Disconnect if any port got connected */
	chamelium_unplug(data->chamelium, port);
	wait_for_connector_after_hotplug(data, mon, port,
			DRM_MODE_DISCONNECTED);

	for_each_pipe(&data->display, pipe) {
		igt_flush_uevents(mon);
		/* Check if we get a sysfs hotplug event */
		chamelium_plug(data->chamelium, port);
		wait_for_connector_after_hotplug(data, mon, port,
				DRM_MODE_CONNECTED);
		igt_flush_uevents(mon);
		output = get_output_for_port(data, port);

		/* If pipe is valid for output then set it */
		if (igt_pipe_connector_valid(pipe, output)) {
			igt_output_set_pipe(output, pipe);
			igt_display_commit2(&data->display, COMMIT_ATOMIC);
		}

		chamelium_unplug(data->chamelium, port);
		wait_for_connector_after_hotplug(data, mon, port,
				DRM_MODE_DISCONNECTED);
		igt_flush_uevents(mon);
	}

	igt_cleanup_uevents(mon);
	igt_hpd_storm_reset(data->drm_fd);
}

static const char test_basic_hotplug_desc[] =
	"Check that we get uevents and updated connector status on "
	"hotplug and unplug";
static void
test_hotplug(data_t *data, struct chamelium_port *port, int toggle_count,
	     enum test_modeset_mode modeset_mode)
{
	int i;
	enum pipe pipe;
	struct igt_fb fb = {0};
	drmModeModeInfo mode;
	struct udev_monitor *mon = igt_watch_uevents();
	igt_output_t *output = get_output_for_port(data, port);

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium, NULL,
			      data->ports, data->port_count);


	igt_hpd_storm_set_threshold(data->drm_fd, 0);

	for (i = 0; i < toggle_count; i++) {
		igt_flush_uevents(mon);

		/* Check if we get a sysfs hotplug event */
		chamelium_plug(data->chamelium, port);

		wait_for_connector_after_hotplug(data, mon, port,
						 DRM_MODE_CONNECTED);
		igt_flush_uevents(mon);

		if (modeset_mode == TEST_MODESET_ON_OFF ||
		    (modeset_mode == TEST_MODESET_ON && i == 0 )) {
			if (i == 0) {
				/* We can only get mode and pipe once we are connected */
				output = get_output_for_port(data, port);
				pipe = get_pipe_for_output(&data->display, output);
				mode = get_mode_for_port(data->chamelium, port);
				create_fb_for_mode(data, &fb, &mode);
			}

			igt_output_set_pipe(output, pipe);
			enable_output(data, port, output, &mode, &fb);
		}

		/* Now check if we get a hotplug from disconnection */
		chamelium_unplug(data->chamelium, port);

		wait_for_connector_after_hotplug(data, mon, port,
						 DRM_MODE_DISCONNECTED);

		igt_flush_uevents(mon);

		if (modeset_mode == TEST_MODESET_ON_OFF) {
			igt_output_set_pipe(output, PIPE_NONE);
			igt_display_commit2(&data->display, COMMIT_ATOMIC);
		}
	}

	igt_cleanup_uevents(mon);
	igt_hpd_storm_reset(data->drm_fd);
	igt_remove_fb(data->drm_fd, &fb);
}

static void set_edid(data_t *data, struct chamelium_port *port,
		     enum igt_custom_edid_type edid)
{
	chamelium_port_set_edid(data->chamelium, port, data->edids[edid]);
}

static const char igt_custom_edid_type_read_desc[] =
	"Make sure the EDID exposed by KMS is the same as the screen's";
static void
igt_custom_edid_type_read(data_t *data, struct chamelium_port *port, enum igt_custom_edid_type edid)
{
	drmModePropertyBlobPtr edid_blob = NULL;
	drmModeConnector *connector;
	size_t raw_edid_size;
	const struct edid *raw_edid;
	uint64_t edid_blob_id;

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium,
			      port, data->ports, data->port_count);

	set_edid(data, port, edid);
	chamelium_plug(data->chamelium, port);
	chamelium_wait_for_conn_status_change(&data->display, data->chamelium,
					      port, DRM_MODE_CONNECTED);

	igt_skip_on(check_analog_bridge(data, port));

	connector = chamelium_port_get_connector(data->chamelium, port, true);
	igt_assert(kmstest_get_property(data->drm_fd, connector->connector_id,
					DRM_MODE_OBJECT_CONNECTOR, "EDID", NULL,
					&edid_blob_id, NULL));
	igt_assert(edid_blob_id != 0);
	igt_assert(edid_blob = drmModeGetPropertyBlob(data->drm_fd,
						      edid_blob_id));

	raw_edid = chamelium_edid_get_raw(data->edids[edid], port);
	raw_edid_size = edid_get_size(raw_edid);
	igt_assert(memcmp(raw_edid, edid_blob->data, raw_edid_size) == 0);

	drmModeFreePropertyBlob(edid_blob);
	drmModeFreeConnector(connector);
}

static void
try_suspend_resume_hpd(data_t *data, struct chamelium_port *port,
		       enum igt_suspend_state state, enum igt_suspend_test test,
		       struct udev_monitor *mon, bool connected)
{
	drmModeConnection target_state = connected ? DRM_MODE_DISCONNECTED :
						     DRM_MODE_CONNECTED;
	int timeout = CHAMELIUM_HOTPLUG_TIMEOUT;
	int delay;
	int p;

	igt_flush_uevents(mon);

	delay = igt_get_autoresume_delay(state) * 1000 / 2;

	if (port) {
		chamelium_schedule_hpd_toggle(data->chamelium, port, delay,
					      !connected);
	} else {
		for (p = 0; p < data->port_count; p++) {
			port = data->ports[p];
			chamelium_schedule_hpd_toggle(data->chamelium, port,
						      delay, !connected);
		}

		port = NULL;
	}

	igt_system_suspend_autoresume(state, test);
	igt_assert(wait_for_hotplug(mon, &timeout));
	chamelium_assert_reachable(data->chamelium, ONLINE_TIMEOUT);

	if (port) {
		igt_assert_eq(chamelium_reprobe_connector(&data->display,
							  data->chamelium,
							  port),
							  target_state);
	} else {
		for (p = 0; p < data->port_count; p++) {
			drmModeConnection current_state;

			port = data->ports[p];
			/*
			 * There could be as many hotplug events sent by
			 * driver as connectors we scheduled an HPD toggle on
			 * above, depending on timing. So if we're not seeing
			 * the expected connector state try to wait for an HPD
			 * event for each connector/port.
			 */
			current_state = chamelium_reprobe_connector(&data->display, data->chamelium, port);
			if (p > 0 && current_state != target_state) {
				igt_assert(wait_for_hotplug(mon, &timeout));
				current_state = chamelium_reprobe_connector(&data->display, data->chamelium, port);
			}

			igt_assert_eq(current_state, target_state);
		}

		port = NULL;
	}
}

static const char test_suspend_resume_hpd_desc[] =
	"Toggle HPD during suspend, check that uevents are sent and connector "
	"status is updated";
static void
test_suspend_resume_hpd(data_t *data, struct chamelium_port *port,
			enum igt_suspend_state state,
			enum igt_suspend_test test)
{
	struct udev_monitor *mon = igt_watch_uevents();

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium,
			      port, data->ports, data->port_count);

	/* Make sure we notice new connectors after resuming */
	try_suspend_resume_hpd(data, port, state, test, mon, false);

	/* Now make sure we notice disconnected connectors after resuming */
	try_suspend_resume_hpd(data, port, state, test, mon, true);

	igt_cleanup_uevents(mon);
}

static const char test_suspend_resume_hpd_common_desc[] =
	"Toggle HPD during suspend on all connectors, check that uevents are "
	"sent and connector status is updated";
static void
test_suspend_resume_hpd_common(data_t *data, enum igt_suspend_state state,
			       enum igt_suspend_test test)
{
	struct udev_monitor *mon = igt_watch_uevents();
	struct chamelium_port *port;
	int p;

	for (p = 0; p < data->port_count; p++) {
		port = data->ports[p];
		igt_debug("Testing port %s\n", chamelium_port_get_name(port));
	}

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium, NULL,
			      data->ports, data->port_count);

	/* Make sure we notice new connectors after resuming */
	try_suspend_resume_hpd(data, NULL, state, test, mon, false);

	/* Now make sure we notice disconnected connectors after resuming */
	try_suspend_resume_hpd(data, NULL, state, test, mon, true);

	igt_cleanup_uevents(mon);
}

static const char test_suspend_resume_edid_change_desc[] =
	"Simulate a screen being unplugged and another screen being plugged "
	"during suspend, check that a uevent is sent and connector status is "
	"updated";
static void
test_suspend_resume_edid_change(data_t *data, struct chamelium_port *port,
				enum igt_suspend_state state,
				enum igt_suspend_test test,
				enum igt_custom_edid_type edid,
				enum igt_custom_edid_type alt_edid)
{
	struct udev_monitor *mon = igt_watch_uevents();
	bool link_status_failed[2][data->port_count];
	int p;

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium,
			      port, data->ports, data->port_count);

	/* Catch the event and flush all remaining ones. */
	igt_assert(igt_hotplug_detected(mon, CHAMELIUM_HOTPLUG_TIMEOUT));
	igt_flush_uevents(mon);

	/* First plug in the port */
	set_edid(data, port, edid);
	chamelium_plug(data->chamelium, port);
	igt_assert(igt_hotplug_detected(mon, CHAMELIUM_HOTPLUG_TIMEOUT));

	chamelium_wait_for_conn_status_change(&data->display, data->chamelium,
					      port, DRM_MODE_CONNECTED);

	/*
	 * Change the edid before we suspend. On resume, the machine should
	 * notice the EDID change and fire a hotplug event.
	 */
	set_edid(data, port, alt_edid);

	get_connectors_link_status_failed(data, link_status_failed[0]);

	igt_flush_uevents(mon);

	igt_system_suspend_autoresume(state, test);
	igt_assert(igt_hotplug_detected(mon, CHAMELIUM_HOTPLUG_TIMEOUT));
	chamelium_assert_reachable(data->chamelium, ONLINE_TIMEOUT);

	get_connectors_link_status_failed(data, link_status_failed[1]);

	for (p = 0; p < data->port_count; p++)
		igt_skip_on(!link_status_failed[0][p] && link_status_failed[1][p]);
}

static igt_output_t *
prepare_output(data_t *data, struct chamelium_port *port, enum igt_custom_edid_type edid)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe pipe;

	/* The chamelium's default EDID has a lot of resolutions, way more then
	 * we need to test. Additionally the default EDID doesn't support HDMI
	 * audio.
	 */
	set_edid(data, port, edid);

	chamelium_plug(data->chamelium, port);
	chamelium_wait_for_conn_status_change(&data->display, data->chamelium,
					      port, DRM_MODE_CONNECTED);

	igt_display_reset(display);

	output = get_output_for_port(data, port);

	/* Refresh pipe to update connected status */
	igt_output_set_pipe(output, PIPE_NONE);

	pipe = get_pipe_for_output(display, output);
	igt_output_set_pipe(output, pipe);

	return output;
}

static void do_test_display(data_t *data, struct chamelium_port *port,
			    igt_output_t *output, drmModeModeInfo *mode,
			    uint32_t fourcc, enum chamelium_check check,
			    int count)
{
	struct chamelium_fb_crc_async_data *fb_crc;
	struct igt_fb frame_fb, fb;
	int i, fb_id, captured_frame_count;
	int frame_id;

	fb_id = chamelium_get_pattern_fb(data, mode->hdisplay, mode->vdisplay,
					 DRM_FORMAT_XRGB8888, 64, &fb);
	igt_assert(fb_id > 0);

	frame_id = igt_fb_convert(&frame_fb, &fb, fourcc,
				  DRM_FORMAT_MOD_LINEAR);
	igt_assert(frame_id > 0);

	if (check == CHAMELIUM_CHECK_CRC)
		fb_crc = chamelium_calculate_fb_crc_async_start(data->drm_fd,
								&fb);

	enable_output(data, port, output, mode, &frame_fb);

	if (check == CHAMELIUM_CHECK_CRC) {
		igt_crc_t *expected_crc;
		igt_crc_t *crc;

		/* We want to keep the display running for a little bit, since
		 * there's always the potential the driver isn't able to keep
		 * the display running properly for very long
		 */
		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, count);
		crc = chamelium_read_captured_crcs(data->chamelium,
						   &captured_frame_count);

		igt_assert(captured_frame_count == count);

		igt_debug("Captured %d frames\n", captured_frame_count);

		expected_crc = chamelium_calculate_fb_crc_async_finish(fb_crc);

		for (i = 0; i < captured_frame_count; i++)
			chamelium_assert_crc_eq_or_dump(data->chamelium,
							expected_crc, &crc[i],
							&fb, i);

		free(expected_crc);
		free(crc);
	} else if (check == CHAMELIUM_CHECK_ANALOG ||
		   check == CHAMELIUM_CHECK_CHECKERBOARD) {
		struct chamelium_frame_dump *dump;

		igt_assert(count == 1);

		dump = chamelium_port_dump_pixels(data->chamelium, port, 0, 0,
						  0, 0);

		if (check == CHAMELIUM_CHECK_ANALOG)
			chamelium_crop_analog_frame(dump, mode->hdisplay,
						    mode->vdisplay);

		chamelium_assert_frame_match_or_dump(data->chamelium, port,
						     dump, &fb, check);
		chamelium_destroy_frame_dump(dump);
	}

	igt_remove_fb(data->drm_fd, &frame_fb);
	igt_remove_fb(data->drm_fd, &fb);
}

static const char test_display_one_mode_desc[] =
	"Pick the first mode of the IGT base EDID, display and capture a few "
	"frames, then check captured frames are correct";
static void test_display_one_mode(data_t *data, struct chamelium_port *port,
				  uint32_t fourcc, enum chamelium_check check,
				  int count)
{
	drmModeConnector *connector;
	drmModeModeInfo *mode;
	igt_output_t *output;
	igt_plane_t *primary;

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium,
			      port, data->ports, data->port_count);

	output = prepare_output(data, port, IGT_CUSTOM_EDID_BASE);
	connector = chamelium_port_get_connector(data->chamelium, port, false);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	igt_require(igt_plane_has_format_mod(primary, fourcc, DRM_FORMAT_MOD_LINEAR));

	mode = &connector->modes[0];
	if (check == CHAMELIUM_CHECK_ANALOG) {
		bool bridge = check_analog_bridge(data, port);

		igt_assert(!(bridge && prune_vga_mode(data, mode)));
	}

	do_test_display(data, port, output, mode, fourcc, check, count);

	drmModeFreeConnector(connector);
}

static const char test_display_all_modes_desc[] =
	"For each mode of the IGT base EDID, display and capture a few "
	"frames, then check captured frames are correct";
static void test_display_all_modes(data_t *data, struct chamelium_port *port,
				   uint32_t fourcc, enum chamelium_check check,
				   int count)
{
	bool bridge;
	int i, count_modes;

	if (check == CHAMELIUM_CHECK_ANALOG)
		bridge = check_analog_bridge(data, port);

	i = 0;
	do {
		igt_output_t *output;
		igt_plane_t *primary;
		drmModeConnector *connector;
		drmModeModeInfo *mode;

		/*
		 * let's reset state each mode so we will get the
		 * HPD pulses realibably
		 */
		igt_modeset_disable_all_outputs(&data->display);
		chamelium_reset_state(&data->display, data->chamelium,
				      port, data->ports, data->port_count);

		/*
		 * modes may change due to mode pruining and link issues, so we
		 * need to refresh the connector
		 */
		output = prepare_output(data, port, IGT_CUSTOM_EDID_BASE);
		connector = chamelium_port_get_connector(data->chamelium, port,
							 false);
		primary = igt_output_get_plane_type(output,
						    DRM_PLANE_TYPE_PRIMARY);
		igt_assert(primary);
		igt_require(igt_plane_has_format_mod(primary, fourcc,
			    DRM_FORMAT_MOD_LINEAR));

		/* we may skip some modes due to above but that's ok */
		count_modes = connector->count_modes;
		if (i >= count_modes)
			break;

		mode = &connector->modes[i];

		if (check == CHAMELIUM_CHECK_ANALOG && bridge &&
		    prune_vga_mode(data, mode))
			continue;

		do_test_display(data, port, output, mode, fourcc, check,
				count);
		drmModeFreeConnector(connector);
	} while (++i < count_modes);
}

static const char test_display_frame_dump_desc[] =
	"For each mode of the IGT base EDID, display and capture a few "
	"frames, then download the captured frames and compare them "
	"bit-by-bit to the sent ones";
static void
test_display_frame_dump(data_t *data, struct chamelium_port *port)
{

	int i, count_modes;

	i = 0;
	do {
		igt_output_t *output;
		igt_plane_t *primary;
		struct igt_fb fb;
		struct chamelium_frame_dump *frame;
		drmModeModeInfo *mode;
		drmModeConnector *connector;
		int fb_id, j;

		/*
		 * let's reset state each mode so we will get the
		 * HPD pulses realibably
		 */
		igt_modeset_disable_all_outputs(&data->display);
		chamelium_reset_state(&data->display, data->chamelium,
				      port, data->ports, data->port_count);

		/*
		 * modes may change due to mode pruining and link issues, so we
		 * need to refresh the connector
		 */
		output = prepare_output(data, port, IGT_CUSTOM_EDID_BASE);
		connector = chamelium_port_get_connector(data->chamelium, port,
							 false);
		primary = igt_output_get_plane_type(output,
						    DRM_PLANE_TYPE_PRIMARY);
		igt_assert(primary);

		/* we may skip some modes due to above but that's ok */
		count_modes = connector->count_modes;
		if (i >= count_modes)
			break;

		mode = &connector->modes[i];

		fb_id = igt_create_color_pattern_fb(data->drm_fd,
						    mode->hdisplay, mode->vdisplay,
						    DRM_FORMAT_XRGB8888,
						    DRM_FORMAT_MOD_LINEAR,
						    0, 0, 0, &fb);
		igt_assert(fb_id > 0);

		enable_output(data, port, output, mode, &fb);

		igt_debug("Reading frame dumps from Chamelium...\n");
		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 5);
		for (j = 0; j < 5; j++) {
			frame = chamelium_read_captured_frame(data->chamelium,
							      j);
			chamelium_assert_frame_eq(data->chamelium, frame, &fb);
			chamelium_destroy_frame_dump(frame);
		}

		igt_remove_fb(data->drm_fd, &fb);
		drmModeFreeConnector(connector);
	} while (++i < count_modes);
}

#define MODE_CLOCK_ACCURACY 0.05 /* 5% */

static void check_mode(struct chamelium *chamelium, struct chamelium_port *port,
		       drmModeModeInfo *mode)
{
	struct chamelium_video_params video_params = {0};
	double mode_clock;
	int mode_hsync_offset, mode_vsync_offset;
	int mode_hsync_width, mode_vsync_width;
	int mode_hsync_polarity, mode_vsync_polarity;

	chamelium_port_get_video_params(chamelium, port, &video_params);

	mode_clock = (double) mode->clock / 1000;

	if (chamelium_port_get_type(port) == DRM_MODE_CONNECTOR_DisplayPort) {
		/* this is what chamelium understands as offsets for DP */
		mode_hsync_offset = mode->htotal - mode->hsync_start;
		mode_vsync_offset = mode->vtotal - mode->vsync_start;
	} else {
		/* and this is what they are for other connectors */
		mode_hsync_offset = mode->hsync_start - mode->hdisplay;
		mode_vsync_offset = mode->vsync_start - mode->vdisplay;
	}

	mode_hsync_width = mode->hsync_end - mode->hsync_start;
	mode_vsync_width = mode->vsync_end - mode->vsync_start;

	mode_hsync_polarity = !!(mode->flags & DRM_MODE_FLAG_PHSYNC);
	mode_vsync_polarity = !!(mode->flags & DRM_MODE_FLAG_PVSYNC);

	igt_debug("Checking video mode:\n");
	igt_debug("clock: got %f, expected %f ± %f%%\n",
		  video_params.clock, mode_clock, MODE_CLOCK_ACCURACY * 100);
	igt_debug("hactive: got %d, expected %d\n",
		  video_params.hactive, mode->hdisplay);
	igt_debug("vactive: got %d, expected %d\n",
		  video_params.vactive, mode->vdisplay);
	igt_debug("hsync_offset: got %d, expected %d\n",
		  video_params.hsync_offset, mode_hsync_offset);
	igt_debug("vsync_offset: got %d, expected %d\n",
		  video_params.vsync_offset, mode_vsync_offset);
	igt_debug("htotal: got %d, expected %d\n",
		  video_params.htotal, mode->htotal);
	igt_debug("vtotal: got %d, expected %d\n",
		  video_params.vtotal, mode->vtotal);
	igt_debug("hsync_width: got %d, expected %d\n",
		  video_params.hsync_width, mode_hsync_width);
	igt_debug("vsync_width: got %d, expected %d\n",
		  video_params.vsync_width, mode_vsync_width);
	igt_debug("hsync_polarity: got %d, expected %d\n",
		  video_params.hsync_polarity, mode_hsync_polarity);
	igt_debug("vsync_polarity: got %d, expected %d\n",
		  video_params.vsync_polarity, mode_vsync_polarity);

	if (!isnan(video_params.clock)) {
		igt_assert(video_params.clock >
			   mode_clock * (1 - MODE_CLOCK_ACCURACY));
		igt_assert(video_params.clock <
			   mode_clock * (1 + MODE_CLOCK_ACCURACY));
	}
	igt_assert(video_params.hactive == mode->hdisplay);
	igt_assert(video_params.vactive == mode->vdisplay);
	igt_assert(video_params.hsync_offset == mode_hsync_offset);
	igt_assert(video_params.vsync_offset == mode_vsync_offset);
	igt_assert(video_params.htotal == mode->htotal);
	igt_assert(video_params.vtotal == mode->vtotal);
	igt_assert(video_params.hsync_width == mode_hsync_width);
	igt_assert(video_params.vsync_width == mode_vsync_width);
	igt_assert(video_params.hsync_polarity == mode_hsync_polarity);
	igt_assert(video_params.vsync_polarity == mode_vsync_polarity);
}

static const char test_mode_timings_desc[] =
	"For each mode of the IGT base EDID, perform a modeset and check the "
	"mode detected by the Chamelium receiver matches the mode we set";
static void test_mode_timings(data_t *data, struct chamelium_port *port)
{
	int i, count_modes;

	i = 0;
	igt_require(chamelium_supports_get_video_params(data->chamelium));
	do {
		igt_output_t *output;
		igt_plane_t *primary;
		drmModeConnector *connector;
		drmModeModeInfo *mode;
		int fb_id;
		struct igt_fb fb;

		/*
		 * let's reset state each mode so we will get the
		 * HPD pulses realibably
		 */
		igt_modeset_disable_all_outputs(&data->display);
		chamelium_reset_state(&data->display, data->chamelium,
				      port, data->ports, data->port_count);

		/*
		 * modes may change due to mode pruining and link issues, so we
		 * need to refresh the connector
		 */
		output = prepare_output(data, port, IGT_CUSTOM_EDID_BASE);
		connector = chamelium_port_get_connector(data->chamelium, port, false);
		primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
		igt_assert(primary);

		/* we may skip some modes due to above but that's ok */
		count_modes = connector->count_modes;
		if (i >= count_modes)
			break;

		mode = &connector->modes[i];

		fb_id = igt_create_color_pattern_fb(data->drm_fd,
						    mode->hdisplay, mode->vdisplay,
						    DRM_FORMAT_XRGB8888,
						    DRM_FORMAT_MOD_LINEAR,
						    0, 0, 0, &fb);
		igt_assert(fb_id > 0);

		enable_output(data, port, output, mode, &fb);

		/* Trigger the FSM */
		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 0);

		check_mode(data->chamelium, port, mode);

		igt_remove_fb(data->drm_fd, &fb);
		drmModeFreeConnector(connector);
	} while (++i < count_modes);
}

struct vic_mode {
	int hactive, vactive;
	int vrefresh; /* Hz */
	uint32_t picture_ar;
};

/* Maps Video Identification Codes to a mode */
static const struct vic_mode vic_modes[] = {
	[16] = {
		.hactive = 1920,
		.vactive = 1080,
		.vrefresh = 60,
		.picture_ar = DRM_MODE_PICTURE_ASPECT_16_9,
	},
};

/* Maps aspect ratios to their mode flag */
static const uint32_t mode_ar_flags[] = {
	[DRM_MODE_PICTURE_ASPECT_16_9] = DRM_MODE_FLAG_PIC_AR_16_9,
};

static enum infoframe_avi_picture_aspect_ratio
get_infoframe_avi_picture_ar(uint32_t aspect_ratio)
{
	/* The AVI picture aspect ratio field only supports 4:3 and 16:9 */
	switch (aspect_ratio) {
	case DRM_MODE_PICTURE_ASPECT_4_3:
		return INFOFRAME_AVI_PIC_AR_4_3;
	case DRM_MODE_PICTURE_ASPECT_16_9:
		return INFOFRAME_AVI_PIC_AR_16_9;
	default:
		return INFOFRAME_AVI_PIC_AR_UNSPECIFIED;
	}
}

static bool vic_mode_matches_drm(const struct vic_mode *vic_mode,
				 drmModeModeInfo *drm_mode)
{
	uint32_t ar_flag = mode_ar_flags[vic_mode->picture_ar];

	return vic_mode->hactive == drm_mode->hdisplay &&
	       vic_mode->vactive == drm_mode->vdisplay &&
	       vic_mode->vrefresh == drm_mode->vrefresh &&
	       ar_flag == (drm_mode->flags & DRM_MODE_FLAG_PIC_AR_MASK);
}

static const char test_display_aspect_ratio_desc[] =
	"Pick a mode with a picture aspect-ratio, capture AVI InfoFrames and "
	"check they include the relevant fields";
static void test_display_aspect_ratio(data_t *data, struct chamelium_port *port)
{
	igt_output_t *output;
	igt_plane_t *primary;
	drmModeConnector *connector;
	drmModeModeInfo *mode;
	int fb_id, i;
	struct igt_fb fb;
	bool found, ok;
	struct chamelium_infoframe *infoframe;
	struct infoframe_avi infoframe_avi;
	uint8_t vic = 16; /* TODO: test more VICs */
	const struct vic_mode *vic_mode;
	uint32_t aspect_ratio;
	enum infoframe_avi_picture_aspect_ratio frame_ar;

	igt_require(chamelium_supports_get_last_infoframe(data->chamelium));

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium,
			      port, data->ports, data->port_count);

	output = prepare_output(data, port, IGT_CUSTOM_EDID_ASPECT_RATIO);
	connector = chamelium_port_get_connector(data->chamelium, port, false);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	vic_mode = &vic_modes[vic];
	aspect_ratio = vic_mode->picture_ar;

	found = false;
	igt_assert(connector->count_modes > 0);
	for (i = 0; i < connector->count_modes; i++) {
		mode = &connector->modes[i];

		if (vic_mode_matches_drm(vic_mode, mode)) {
			found = true;
			break;
		}
	}
	igt_assert_f(found,
		     "Failed to find mode with the correct aspect ratio\n");

	fb_id = igt_create_color_pattern_fb(data->drm_fd,
					    mode->hdisplay, mode->vdisplay,
					    DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_MOD_LINEAR,
					    0, 0, 0, &fb);
	igt_assert(fb_id > 0);

	enable_output(data, port, output, mode, &fb);

	infoframe = chamelium_get_last_infoframe(data->chamelium, port,
						 CHAMELIUM_INFOFRAME_AVI);
	igt_assert_f(infoframe, "AVI InfoFrame not received\n");

	ok = infoframe_avi_parse(&infoframe_avi, infoframe->version,
				 infoframe->payload, infoframe->payload_size);
	igt_assert_f(ok, "Failed to parse AVI InfoFrame\n");

	frame_ar = get_infoframe_avi_picture_ar(aspect_ratio);

	igt_debug("Checking AVI InfoFrame\n");
	igt_debug("Picture aspect ratio: got %d, expected %d\n",
		  infoframe_avi.picture_aspect_ratio, frame_ar);
	igt_debug("Video Identification Code (VIC): got %d, expected %d\n",
		  infoframe_avi.vic, vic);

	igt_assert(infoframe_avi.picture_aspect_ratio == frame_ar);
	igt_assert(infoframe_avi.vic == vic);

	chamelium_infoframe_destroy(infoframe);
	igt_remove_fb(data->drm_fd, &fb);
	drmModeFreeConnector(connector);
}


/* Playback parameters control the audio signal we synthesize and send */
#define PLAYBACK_CHANNELS 2
#define PLAYBACK_SAMPLES 1024

/* Capture paremeters control the audio signal we receive */
#define CAPTURE_SAMPLES 2048

#define AUDIO_TIMEOUT 2000 /* ms */
/* A streak of 3 gives confidence that the signal is good. */
#define MIN_STREAK 3

#define FLATLINE_AMPLITUDE 0.1 /* normalized, ie. in [0, 1] */
#define FLATLINE_AMPLITUDE_ACCURACY 0.001 /* ± 0.1 % of the full amplitude */
#define FLATLINE_ALIGN_ACCURACY 0 /* number of samples */

/* TODO: enable >48KHz rates, these are not reliable */
static int test_sampling_rates[] = {
	32000,
	44100,
	48000,
	/* 88200, */
	/* 96000, */
	/* 176400, */
	/* 192000, */
};

static int test_sampling_rates_count = sizeof(test_sampling_rates) / sizeof(int);

/* Test frequencies (Hz): a sine signal will be generated for each.
 *
 * Depending on the sampling rate chosen, it might not be possible to properly
 * detect the generated sine (see Nyquist–Shannon sampling theorem).
 * Frequencies that can't be reliably detected will be automatically pruned in
 * #audio_signal_add_frequency. For instance, the 80KHz frequency can only be
 * tested with a 192KHz sampling rate.
 */
static int test_frequencies[] = {
	300,
	600,
	1200,
	10000,
	80000,
};

static int test_frequencies_count = sizeof(test_frequencies) / sizeof(int);

static const snd_pcm_format_t test_formats[] = {
	SND_PCM_FORMAT_S16_LE,
	SND_PCM_FORMAT_S24_LE,
	SND_PCM_FORMAT_S32_LE,
};

static const size_t test_formats_count = sizeof(test_formats) / sizeof(test_formats[0]);

struct audio_state {
	struct alsa *alsa;
	struct chamelium *chamelium;
	struct chamelium_port *port;
	struct chamelium_stream *stream;

	/* The capture format is only available after capture has started. */
	struct {
		snd_pcm_format_t format;
		int channels;
		int rate;
	} playback, capture;

	char *name;
	struct audio_signal *signal; /* for frequencies test only */
	int channel_mapping[CHAMELIUM_MAX_AUDIO_CHANNELS];

	size_t recv_pages;
	int msec;

	int dump_fd;
	char *dump_path;

	pthread_t thread;
	atomic_bool run;
	atomic_bool positive; /* for pulse test only */
};

static void audio_state_init(struct audio_state *state, data_t *data,
			     struct alsa *alsa, struct chamelium_port *port,
			     snd_pcm_format_t format, int channels, int rate)
{
	memset(state, 0, sizeof(*state));
	state->dump_fd = -1;

	state->alsa = alsa;
	state->chamelium = data->chamelium;
	state->port = port;

	state->playback.format = format;
	state->playback.channels = channels;
	state->playback.rate = rate;

	alsa_configure_output(alsa, format, channels, rate);

	state->stream = chamelium_stream_init();
	igt_assert_f(state->stream,
		     "Failed to initialize Chamelium stream client\n");
}

static void audio_state_fini(struct audio_state *state)
{
	chamelium_stream_deinit(state->stream);
	free(state->name);
}

static void *run_audio_thread(void *data)
{
	struct alsa *alsa = data;

	alsa_run(alsa, -1);
	return NULL;
}

static void audio_state_start(struct audio_state *state, const char *name)
{
	int ret;
	bool ok;
	size_t i, j;
	enum chamelium_stream_realtime_mode stream_mode;
	char dump_suffix[64];

	free(state->name);
	state->name = strdup(name);
	state->recv_pages = 0;
	state->msec = 0;

	igt_debug("Starting %s test with playback format %s, "
		  "sampling rate %d Hz and %d channels\n",
		  name, snd_pcm_format_name(state->playback.format),
		  state->playback.rate, state->playback.channels);

	chamelium_start_capturing_audio(state->chamelium, state->port, false);

	stream_mode = CHAMELIUM_STREAM_REALTIME_STOP_WHEN_OVERFLOW;
	ok = chamelium_stream_dump_realtime_audio(state->stream, stream_mode);
	igt_assert_f(ok, "Failed to start streaming audio capture\n");

	/* Start playing audio */
	state->run = true;
	ret = pthread_create(&state->thread, NULL,
			     run_audio_thread, state->alsa);
	igt_assert_f(ret == 0, "Failed to start audio playback thread\n");

	/* The Chamelium device only supports this PCM format. */
	state->capture.format = SND_PCM_FORMAT_S32_LE;

	/* Only after we've started playing audio, we can retrieve the capture
	 * format used by the Chamelium device. */
	chamelium_get_audio_format(state->chamelium, state->port,
				   &state->capture.rate,
				   &state->capture.channels);
	if (state->capture.rate == 0) {
		igt_debug("Audio receiver doesn't indicate the capture "
			 "sampling rate, assuming it's %d Hz\n",
			 state->playback.rate);
		state->capture.rate = state->playback.rate;
	}

	chamelium_get_audio_channel_mapping(state->chamelium, state->port,
					    state->channel_mapping);
	/* Make sure we can capture all channels we send. */
	for (i = 0; i < state->playback.channels; i++) {
		ok = false;
		for (j = 0; j < state->capture.channels; j++) {
			if (state->channel_mapping[j] == i) {
				ok = true;
				break;
			}
		}
		igt_assert_f(ok, "Cannot capture all channels\n");
	}

	if (igt_frame_dump_is_enabled()) {
		snprintf(dump_suffix, sizeof(dump_suffix),
			 "capture-%s-%s-%dch-%dHz",
			 name, snd_pcm_format_name(state->playback.format),
			 state->playback.channels, state->playback.rate);

		state->dump_fd = audio_create_wav_file_s32_le(dump_suffix,
							      state->capture.rate,
							      state->capture.channels,
							      &state->dump_path);
		igt_assert_f(state->dump_fd >= 0,
			     "Failed to create audio dump file\n");
	}
}

static void audio_state_receive(struct audio_state *state,
				int32_t **recv, size_t *recv_len)
{
	bool ok;
	size_t page_count;
	size_t recv_size;

	ok = chamelium_stream_receive_realtime_audio(state->stream,
						     &page_count,
						     recv, recv_len);
	igt_assert_f(ok, "Failed to receive audio from stream server\n");

	state->msec = state->recv_pages * *recv_len
		      / (double) state->capture.channels
		      / (double) state->capture.rate * 1000;
	state->recv_pages++;

	if (state->dump_fd >= 0) {
		recv_size = *recv_len * sizeof(int32_t);
		igt_assert_f(write(state->dump_fd, *recv, recv_size) == recv_size,
			     "Failed to write to audio dump file\n");
	}
}

static void audio_state_stop(struct audio_state *state, bool success)
{
	bool ok;
	int ret;
	struct chamelium_audio_file *audio_file;
	enum igt_log_level log_level;

	igt_debug("Stopping audio playback\n");
	state->run = false;
	ret = pthread_join(state->thread, NULL);
	igt_assert_f(ret == 0, "Failed to join audio playback thread\n");

	ok = chamelium_stream_stop_realtime_audio(state->stream);
	igt_assert_f(ok, "Failed to stop streaming audio capture\n");

	audio_file = chamelium_stop_capturing_audio(state->chamelium,
						    state->port);
	if (audio_file) {
		igt_debug("Audio file saved on the Chamelium in %s\n",
			  audio_file->path);
		chamelium_destroy_audio_file(audio_file);
	}

	if (state->dump_fd >= 0) {
		close(state->dump_fd);
		state->dump_fd = -1;

		if (success) {
			/* Test succeeded, no need to keep the captured data */
			unlink(state->dump_path);
		} else
			igt_debug("Saved captured audio data to %s\n",
				  state->dump_path);
		free(state->dump_path);
		state->dump_path = NULL;
	}

	if (success)
		log_level = IGT_LOG_DEBUG;
	else
		log_level = IGT_LOG_CRITICAL;

	igt_log(IGT_LOG_DOMAIN, log_level, "Audio %s test result for format %s, "
		"sampling rate %d Hz and %d channels: %s\n",
		state->name, snd_pcm_format_name(state->playback.format),
		state->playback.rate, state->playback.channels,
		success ? "ALL GREEN" : "FAILED");

}

static void check_audio_infoframe(struct audio_state *state)
{
	struct chamelium_infoframe *infoframe;
	struct infoframe_audio infoframe_audio;
	struct infoframe_audio expected = {0};
	bool ok;

	if (!chamelium_supports_get_last_infoframe(state->chamelium)) {
		igt_debug("Skipping audio InfoFrame check: "
			  "Chamelium board doesn't support GetLastInfoFrame\n");
		return;
	}

	expected.coding_type = INFOFRAME_AUDIO_CT_PCM;
	expected.channel_count = state->playback.channels;
	expected.sampling_freq = state->playback.rate;
	expected.sample_size = snd_pcm_format_width(state->playback.format);

	infoframe = chamelium_get_last_infoframe(state->chamelium, state->port,
						 CHAMELIUM_INFOFRAME_AUDIO);
	if (infoframe == NULL && state->playback.channels <= 2) {
		/* Audio InfoFrames are optional for mono and stereo audio */
		igt_debug("Skipping audio InfoFrame check: "
			  "no InfoFrame received\n");
		return;
	}
	igt_assert_f(infoframe != NULL, "no audio InfoFrame received\n");

	ok = infoframe_audio_parse(&infoframe_audio, infoframe->version,
				   infoframe->payload, infoframe->payload_size);
	chamelium_infoframe_destroy(infoframe);
	igt_assert_f(ok, "failed to parse audio InfoFrame\n");

	igt_debug("Checking audio InfoFrame:\n");
	igt_debug("coding_type: got %d, expected %d\n",
		  infoframe_audio.coding_type, expected.coding_type);
	igt_debug("channel_count: got %d, expected %d\n",
		  infoframe_audio.channel_count, expected.channel_count);
	igt_debug("sampling_freq: got %d, expected %d\n",
		  infoframe_audio.sampling_freq, expected.sampling_freq);
	igt_debug("sample_size: got %d, expected %d\n",
		  infoframe_audio.sample_size, expected.sample_size);

	if (infoframe_audio.coding_type != INFOFRAME_AUDIO_CT_UNSPECIFIED)
		igt_assert(infoframe_audio.coding_type == expected.coding_type);
	if (infoframe_audio.channel_count >= 0)
		igt_assert(infoframe_audio.channel_count == expected.channel_count);
	if (infoframe_audio.sampling_freq >= 0)
		igt_assert(infoframe_audio.sampling_freq == expected.sampling_freq);
	if (infoframe_audio.sample_size >= 0)
		igt_assert(infoframe_audio.sample_size == expected.sample_size);
}

static int
audio_output_frequencies_callback(void *data, void *buffer, int samples)
{
	struct audio_state *state = data;
	double *tmp;
	size_t len;

	len = samples * state->playback.channels;
	tmp = malloc(len * sizeof(double));
	audio_signal_fill(state->signal, tmp, samples);
	audio_convert_to(buffer, tmp, len, state->playback.format);
	free(tmp);

	return state->run ? 0 : -1;
}

static bool test_audio_frequencies(struct audio_state *state)
{
	int freq, step;
	int32_t *recv, *buf;
	double *channel;
	size_t i, j, streak;
	size_t recv_len, buf_len, buf_cap, channel_len;
	bool success;
	int capture_chan;

	state->signal = audio_signal_init(state->playback.channels,
					  state->playback.rate);
	igt_assert_f(state->signal, "Failed to initialize audio signal\n");

	/* We'll choose different frequencies per channel to make sure they are
	 * independent from each other. To do so, we'll add a different offset
	 * to the base frequencies for each channel. We need to choose a big
	 * enough offset so that we're sure to detect mixed up channels. We
	 * choose an offset of two 2 bins in the final FFT to enforce a clear
	 * difference.
	 *
	 * Note that we assume capture_rate == playback_rate. We'll assert this
	 * later on. We cannot retrieve the capture rate before starting
	 * playing audio, so we don't really have the choice.
	 */
	step = 2 * state->playback.rate / CAPTURE_SAMPLES;
	for (i = 0; i < test_frequencies_count; i++) {
		for (j = 0; j < state->playback.channels; j++) {
			freq = test_frequencies[i] + j * step;
			audio_signal_add_frequency(state->signal, freq, j);
		}
	}
	audio_signal_synthesize(state->signal);

	alsa_register_output_callback(state->alsa,
				      audio_output_frequencies_callback, state,
				      PLAYBACK_SAMPLES);

	audio_state_start(state, "frequencies");

	igt_assert_f(state->capture.rate == state->playback.rate,
		     "Capture rate (%dHz) doesn't match playback rate (%dHz)\n",
		     state->capture.rate, state->playback.rate);

	/* Needs to be a multiple of 128, because that's the number of samples
	 * we get per channel each time we receive an audio page from the
	 * Chamelium device.
	 *
	 * Additionally, this value needs to be high enough to guarantee we
	 * capture a full period of each sine we generate. If we capture 2048
	 * samples at a 192KHz sampling rate, we get a full period for a >94Hz
	 * sines. For lower sampling rates, the capture duration will be
	 * longer.
	 */
	channel_len = CAPTURE_SAMPLES;
	channel = malloc(sizeof(double) * channel_len);

	buf_cap = state->capture.channels * channel_len;
	buf = malloc(sizeof(int32_t) * buf_cap);
	buf_len = 0;

	recv = NULL;
	recv_len = 0;

	success = false;
	streak = 0;
	while (!success && state->msec < AUDIO_TIMEOUT) {
		audio_state_receive(state, &recv, &recv_len);

		memcpy(&buf[buf_len], recv, recv_len * sizeof(int32_t));
		buf_len += recv_len;

		if (buf_len < buf_cap)
			continue;
		igt_assert(buf_len == buf_cap);

		igt_debug("Detecting audio signal, t=%d msec\n", state->msec);

		for (j = 0; j < state->playback.channels; j++) {
			capture_chan = state->channel_mapping[j];
			igt_assert(capture_chan >= 0);
			igt_debug("Processing channel %zu (captured as "
				  "channel %d)\n", j, capture_chan);

			audio_extract_channel_s32_le(channel, channel_len,
						     buf, buf_len,
						     state->capture.channels,
						     capture_chan);

			if (audio_signal_detect(state->signal,
						state->capture.rate, j,
						channel, channel_len))
				streak++;
			else
				streak = 0;
		}

		buf_len = 0;

		success = streak == MIN_STREAK * state->playback.channels;
	}

	audio_state_stop(state, success);

	free(recv);
	free(buf);
	free(channel);
	audio_signal_fini(state->signal);

	check_audio_infoframe(state);

	return success;
}

static int audio_output_flatline_callback(void *data, void *buffer,
					     int samples)
{
	struct audio_state *state = data;
	double *tmp;
	size_t len, i;

	len = samples * state->playback.channels;
	tmp = malloc(len * sizeof(double));
	for (i = 0; i < len; i++)
		tmp[i] = (state->positive ? 1 : -1) * FLATLINE_AMPLITUDE;
	audio_convert_to(buffer, tmp, len, state->playback.format);
	free(tmp);

	return state->run ? 0 : -1;
}

static bool detect_flatline_amplitude(double *buf, size_t buf_len, bool pos)
{
	double expected, min, max;
	size_t i;
	bool ok;

	min = max = NAN;
	for (i = 0; i < buf_len; i++) {
		if (isnan(min) || buf[i] < min)
			min = buf[i];
		if (isnan(max) || buf[i] > max)
			max = buf[i];
	}

	expected = (pos ? 1 : -1) * FLATLINE_AMPLITUDE;
	ok = (min >= expected - FLATLINE_AMPLITUDE_ACCURACY &&
	      max <= expected + FLATLINE_AMPLITUDE_ACCURACY);
	if (ok)
		igt_debug("Flatline wave amplitude detected\n");
	else
		igt_debug("Flatline amplitude not detected (min=%f, max=%f)\n",
			  min, max);
	return ok;
}

static ssize_t detect_falling_edge(double *buf, size_t buf_len)
{
	size_t i;

	for (i = 0; i < buf_len; i++) {
		if (buf[i] < 0)
			return i;
	}

	return -1;
}

/** test_audio_flatline:
 *
 * Send a constant value (one positive, then a negative one) and check that:
 *
 * - The amplitude of the flatline is correct
 * - All channels switch from a positive signal to a negative one at the same
 *   time (ie. all channels are aligned)
 */
static bool test_audio_flatline(struct audio_state *state)
{
	bool success, amp_success, align_success;
	int32_t *recv;
	size_t recv_len, i, channel_len;
	ssize_t j;
	int streak, capture_chan;
	double *channel;
	int falling_edges[CHAMELIUM_MAX_AUDIO_CHANNELS];

	alsa_register_output_callback(state->alsa,
				      audio_output_flatline_callback, state,
				      PLAYBACK_SAMPLES);

	/* Start by sending a positive signal */
	state->positive = true;

	audio_state_start(state, "flatline");

	for (i = 0; i < state->playback.channels; i++)
		falling_edges[i] = -1;

	recv = NULL;
	recv_len = 0;
	amp_success = false;
	streak = 0;
	while (!amp_success && state->msec < AUDIO_TIMEOUT) {
		audio_state_receive(state, &recv, &recv_len);

		igt_debug("Detecting audio signal, t=%d msec\n", state->msec);

		for (i = 0; i < state->playback.channels; i++) {
			capture_chan = state->channel_mapping[i];
			igt_assert(capture_chan >= 0);
			igt_debug("Processing channel %zu (captured as "
				  "channel %d)\n", i, capture_chan);

			channel_len = audio_extract_channel_s32_le(NULL, 0,
								   recv, recv_len,
								   state->capture.channels,
								   capture_chan);
			channel = malloc(channel_len * sizeof(double));
			audio_extract_channel_s32_le(channel, channel_len,
						     recv, recv_len,
						     state->capture.channels,
						     capture_chan);

			/* Check whether the amplitude is fine */
			if (detect_flatline_amplitude(channel, channel_len,
						      state->positive))
				streak++;
			else
				streak = 0;

			/* If we're now sending a negative signal, detect the
			 * falling edge */
			j = detect_falling_edge(channel, channel_len);
			if (!state->positive && j >= 0) {
				falling_edges[i] = recv_len * state->recv_pages
						   + j;
			}

			free(channel);
		}

		amp_success = streak == MIN_STREAK * state->playback.channels;

		if (amp_success && state->positive) {
			/* Switch to a negative signal after we've detected the
			 * positive one. */
			state->positive = false;
			amp_success = false;
			streak = 0;
			igt_debug("Switching to negative square wave\n");
		}
	}

	/* Check alignment between all channels by comparing the index of the
	 * falling edge. */
	align_success = true;
	for (i = 0; i < state->playback.channels; i++) {
		if (falling_edges[i] < 0) {
			igt_critical("Falling edge not detected for channel %zu\n",
				     i);
			align_success = false;
			continue;
		}

		if (abs(falling_edges[0] - falling_edges[i]) >
		    FLATLINE_ALIGN_ACCURACY) {
			igt_critical("Channel alignment mismatch: "
				     "channel 0 has a falling edge at index %d "
				     "while channel %zu has index %d\n",
				     falling_edges[0], i, falling_edges[i]);
			align_success = false;
		}
	}

	success = amp_success && align_success;
	audio_state_stop(state, success);

	free(recv);

	return success;
}

static bool check_audio_configuration(struct alsa *alsa, snd_pcm_format_t format,
				      int channels, int sampling_rate)
{
	if (!alsa_test_output_configuration(alsa, format, channels,
					    sampling_rate)) {
		igt_debug("Skipping test with format %s, sampling rate %d Hz "
			  "and %d channels because at least one of the "
			  "selected output devices doesn't support this "
			  "configuration\n",
			  snd_pcm_format_name(format),
			  sampling_rate, channels);
		return false;
	}
	/* TODO: the Chamelium device sends a malformed signal for some audio
	 * configurations. See crbug.com/950917 */
	if ((format != SND_PCM_FORMAT_S16_LE && sampling_rate >= 44100) ||
			channels > 2) {
		igt_debug("Skipping test with format %s, sampling rate %d Hz "
			  "and %d channels because the Chamelium device "
			  "doesn't support this configuration\n",
			  snd_pcm_format_name(format),
			  sampling_rate, channels);
		return false;
	}
	return true;
}

static const char test_display_audio_desc[] =
	"Playback various audio signals with various audio formats/rates, "
	"capture them and check they are correct";
static void
test_display_audio(data_t *data, struct chamelium_port *port,
		   const char *audio_device, enum igt_custom_edid_type edid)
{
	bool run, success;
	struct alsa *alsa;
	int ret;
	igt_output_t *output;
	igt_plane_t *primary;
	struct igt_fb fb;
	drmModeModeInfo *mode;
	drmModeConnector *connector;
	int fb_id, i, j;
	int channels, sampling_rate;
	snd_pcm_format_t format;
	struct audio_state state;

	igt_require(alsa_has_exclusive_access());

	/* Old Chamelium devices need an update for DisplayPort audio and
	 * chamelium_get_audio_format support. */
	igt_require(chamelium_has_audio_support(data->chamelium, port));

	alsa = alsa_init();
	igt_assert(alsa);

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium,
			      port, data->ports, data->port_count);

	output = prepare_output(data, port, edid);
	connector = chamelium_port_get_connector(data->chamelium, port, false);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	/* Enable the output because the receiver won't try to receive audio if
	 * it doesn't receive video. */
	igt_assert(connector->count_modes > 0);
	mode = &connector->modes[0];

	fb_id = igt_create_color_pattern_fb(data->drm_fd,
					    mode->hdisplay, mode->vdisplay,
					    DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_MOD_LINEAR,
					    0, 0, 0, &fb);
	igt_assert(fb_id > 0);

	enable_output(data, port, output, mode, &fb);

	run = false;
	success = true;
	for (i = 0; i < test_sampling_rates_count; i++) {
		for (j = 0; j < test_formats_count; j++) {
			ret = alsa_open_output(alsa, audio_device);
			igt_assert_f(ret >= 0, "Failed to open ALSA output\n");

			/* TODO: playback on all 8 available channels (this
			 * isn't supported by Chamelium devices yet, see
			 * https://crbug.com/950917) */
			format = test_formats[j];
			channels = PLAYBACK_CHANNELS;
			sampling_rate = test_sampling_rates[i];

			if (!check_audio_configuration(alsa, format, channels,
						       sampling_rate))
				continue;

			run = true;

			audio_state_init(&state, data, alsa, port,
					 format, channels, sampling_rate);
			success &= test_audio_frequencies(&state);
			success &= test_audio_flatline(&state);
			audio_state_fini(&state);

			alsa_close_output(alsa);
		}
	}

	/* Make sure we tested at least one frequency and format. */
	igt_assert(run);
	/* Make sure all runs were successful. */
	igt_assert(success);

	igt_remove_fb(data->drm_fd, &fb);

	drmModeFreeConnector(connector);

	free(alsa);
}

static const char test_display_audio_edid_desc[] =
	"Plug a connector with an EDID suitable for audio, check ALSA's "
	"EDID-Like Data reports the correct audio parameters";
static void
test_display_audio_edid(data_t *data, struct chamelium_port *port,
			enum igt_custom_edid_type edid)
{
	igt_output_t *output;
	igt_plane_t *primary;
	struct igt_fb fb;
	drmModeModeInfo *mode;
	drmModeConnector *connector;
	int fb_id;
	struct eld_entry eld;
	struct eld_sad *sad;

	igt_require(eld_is_supported());

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium,
			      port, data->ports, data->port_count);

	output = prepare_output(data, port, edid);
	connector = chamelium_port_get_connector(data->chamelium, port, false);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	/* Enable the output because audio cannot be played on inactive
	 * connectors. */
	igt_assert(connector->count_modes > 0);
	mode = &connector->modes[0];

	fb_id = igt_create_color_pattern_fb(data->drm_fd,
					    mode->hdisplay, mode->vdisplay,
					    DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_MOD_LINEAR,
					    0, 0, 0, &fb);
	igt_assert(fb_id > 0);

	enable_output(data, port, output, mode, &fb);

	igt_assert(eld_get_igt(&eld));
	igt_assert(eld.sads_len == 1);

	sad = &eld.sads[0];
	igt_assert(sad->coding_type == CEA_SAD_FORMAT_PCM);
	igt_assert(sad->channels == 2);
	igt_assert(sad->rates == (CEA_SAD_SAMPLING_RATE_32KHZ |
		   CEA_SAD_SAMPLING_RATE_44KHZ | CEA_SAD_SAMPLING_RATE_48KHZ));
	igt_assert(sad->bits == (CEA_SAD_SAMPLE_SIZE_16 |
		   CEA_SAD_SAMPLE_SIZE_20 | CEA_SAD_SAMPLE_SIZE_24));

	igt_remove_fb(data->drm_fd, &fb);

	drmModeFreeConnector(connector);
}

static void randomize_plane_stride(data_t *data,
				   uint32_t width, uint32_t height,
				   uint32_t format, uint64_t modifier,
				   size_t *stride)
{
	size_t stride_min;
	uint32_t max_tile_w = 4, tile_w, tile_h;
	int i;
	struct igt_fb dummy;

	stride_min = width * igt_format_plane_bpp(format, 0) / 8;

	/* Randomize the stride to less than twice the minimum. */
	*stride = (rand() % stride_min) + stride_min;

	/*
	 * Create a dummy FB to determine bpp for each plane, and calculate
	 * the maximum tile width from that.
	 */
	igt_create_fb(data->drm_fd, 64, 64, format, modifier, &dummy);
	for (i = 0; i < dummy.num_planes; i++) {
		igt_get_fb_tile_size(data->drm_fd, modifier, dummy.plane_bpp[i], &tile_w, &tile_h);

		if (tile_w > max_tile_w)
			max_tile_w = tile_w;
	}
	igt_remove_fb(data->drm_fd, &dummy);

	/*
	 * Pixman requires the stride to be aligned to 32-bits, which is
	 * reflected in the initial value of max_tile_w and the hw
	 * may require a multiple of tile width, choose biggest of the 2.
	 */
	*stride = ALIGN(*stride, max_tile_w);
}

static void update_tiled_modifier(igt_plane_t *plane, uint32_t width,
				  uint32_t height, uint32_t format,
				  uint64_t *modifier)
{
	if (*modifier == DRM_FORMAT_MOD_BROADCOM_SAND256) {
		/* Randomize the column height to less than twice the minimum. */
		size_t column_height = (rand() % height) + height;

		igt_debug("Selecting VC4 SAND256 tiling with column height %ld\n",
			  column_height);

		*modifier = DRM_FORMAT_MOD_BROADCOM_SAND256_COL_HEIGHT(column_height);
	}
}

static void randomize_plane_setup(data_t *data, igt_plane_t *plane,
				  drmModeModeInfo *mode,
				  uint32_t *width, uint32_t *height,
				  uint32_t *format, uint64_t *modifier,
				  bool allow_yuv)
{
	int min_dim;
	uint32_t idx[plane->format_mod_count];
	unsigned int count = 0;
	unsigned int i;

	/* First pass to count the supported formats. */
	for (i = 0; i < plane->format_mod_count; i++)
		if (igt_fb_supported_format(plane->formats[i]) &&
		    (allow_yuv || !igt_format_is_yuv(plane->formats[i])))
			idx[count++] = i;

	igt_assert(count > 0);

	i = idx[rand() % count];
	*format = plane->formats[i];
	*modifier = plane->modifiers[i];

	update_tiled_modifier(plane, *width, *height, *format, modifier);

	/*
	 * Randomize width and height in the mode dimensions range.
	 *
	 * Restrict to a min of 2 * min_dim, this way src_w/h are always at
	 * least min_dim, because src_w = width - (rand % w / 2).
	 *
	 * Use a minimum dimension of 16 for YUV, because planar YUV
	 * subsamples the UV plane.
	 */
	min_dim = igt_format_is_yuv(*format) ? 16 : 8;

	*width = max((rand() % mode->hdisplay) + 1, 2 * min_dim);
	*height = max((rand() % mode->vdisplay) + 1, 2 * min_dim);
}

static void configure_plane(igt_plane_t *plane, uint32_t src_w, uint32_t src_h,
			    uint32_t src_x, uint32_t src_y, uint32_t crtc_w,
			    uint32_t crtc_h, int32_t crtc_x, int32_t crtc_y,
			    struct igt_fb *fb)
{
	igt_plane_set_fb(plane, fb);

	igt_plane_set_position(plane, crtc_x, crtc_y);
	igt_plane_set_size(plane, crtc_w, crtc_h);

	igt_fb_set_position(fb, plane, src_x, src_y);
	igt_fb_set_size(fb, plane, src_w, src_h);
}

static void randomize_plane_coordinates(data_t *data, igt_plane_t *plane,
					drmModeModeInfo *mode,
					struct igt_fb *fb,
					uint32_t *src_w, uint32_t *src_h,
					uint32_t *src_x, uint32_t *src_y,
					uint32_t *crtc_w, uint32_t *crtc_h,
					int32_t *crtc_x, int32_t *crtc_y,
					bool allow_scaling)
{
	bool is_yuv = igt_format_is_yuv(fb->drm_format);
	uint32_t width = fb->width, height = fb->height;
	double ratio;
	int ret;

	/* Randomize source offset in the first half of the original size. */
	*src_x = rand() % (width / 2);
	*src_y = rand() % (height / 2);

	/* The source size only includes the active source area. */
	*src_w = width - *src_x;
	*src_h = height - *src_y;

	if (allow_scaling) {
		*crtc_w = (rand() % mode->hdisplay) + 1;
		*crtc_h = (rand() % mode->vdisplay) + 1;

		/*
		 * Don't bother with scaling if dimensions are quite close in
		 * order to get non-scaling cases more frequently. Also limit
		 * scaling to 3x to avoid agressive filtering that makes
		 * comparison less reliable, and don't go above 2x downsampling
		 * to avoid possible hw limitations.
		 */

		ratio = ((double) *crtc_w / *src_w);
		if (ratio < 0.5)
			*src_w = *crtc_w * 2;
		else if (ratio > 0.8 && ratio < 1.2)
			*crtc_w = *src_w;
		else if (ratio > 3.0)
			*crtc_w = *src_w * 3;

		ratio = ((double) *crtc_h / *src_h);
		if (ratio < 0.5)
			*src_h = *crtc_h * 2;
		else if (ratio > 0.8 && ratio < 1.2)
			*crtc_h = *src_h;
		else if (ratio > 3.0)
			*crtc_h = *src_h * 3;
	} else {
		*crtc_w = *src_w;
		*crtc_h = *src_h;
	}

	if (*crtc_w != *src_w || *crtc_h != *src_h) {
		/*
		 * When scaling is involved, make sure to not go off-bounds or
		 * scaled clipping may result in decimal dimensions, that most
		 * drivers don't support.
		 */
		if (*crtc_w < mode->hdisplay)
			*crtc_x = rand() % (mode->hdisplay - *crtc_w);
		else
			*crtc_x = 0;

		if (*crtc_h < mode->vdisplay)
			*crtc_y = rand() % (mode->vdisplay - *crtc_h);
		else
			*crtc_y = 0;
	} else {
		/*
		 * Randomize the on-crtc position and allow the plane to go
		 * off-display by less than half of its on-crtc dimensions.
		 */
		*crtc_x = (rand() % mode->hdisplay) - *crtc_w / 2;
		*crtc_y = (rand() % mode->vdisplay) - *crtc_h / 2;
	}

	configure_plane(plane, *src_w, *src_h, *src_x, *src_y,
			*crtc_w, *crtc_h, *crtc_x, *crtc_y, fb);
	ret = igt_display_try_commit_atomic(&data->display,
					    DRM_MODE_ATOMIC_TEST_ONLY |
					    DRM_MODE_ATOMIC_ALLOW_MODESET,
					    NULL);
	if (!ret)
		return;

	/* Coordinates are logged in the dumped debug log, so only report w/h on failure here. */
	igt_assert_f(ret != -ENOSPC,"Failure in testcase, invalid coordinates on a %ux%u fb\n", width, height);

	/* Make YUV coordinates a multiple of 2 and retry the math. */
	if (is_yuv) {
		*src_x &= ~1;
		*src_y &= ~1;
		*src_w &= ~1;
		*src_h &= ~1;
		/* To handle 1:1 scaling, clear crtc_w/h too. */
		*crtc_w &= ~1;
		*crtc_h &= ~1;

		if (*crtc_x < 0 && (*crtc_x & 1))
			(*crtc_x)++;
		else
			*crtc_x &= ~1;

		/* If negative, round up to 0 instead of down */
		if (*crtc_y < 0 && (*crtc_y & 1))
			(*crtc_y)++;
		else
			*crtc_y &= ~1;

		configure_plane(plane, *src_w, *src_h, *src_x, *src_y, *crtc_w,
				*crtc_h, *crtc_x, *crtc_y, fb);
		ret = igt_display_try_commit_atomic(&data->display,
						DRM_MODE_ATOMIC_TEST_ONLY |
						DRM_MODE_ATOMIC_ALLOW_MODESET,
						NULL);
		if (!ret)
			return;
	}

	igt_assert(!ret || allow_scaling);
	igt_info("Scaling ratio %g / %g failed, trying without scaling.\n",
		  ((double) *crtc_w / *src_w), ((double) *crtc_h / *src_h));

	*crtc_w = *src_w;
	*crtc_h = *src_h;

	configure_plane(plane, *src_w, *src_h, *src_x, *src_y, *crtc_w,
			*crtc_h, *crtc_x, *crtc_y, fb);
	igt_display_commit_atomic(&data->display,
				  DRM_MODE_ATOMIC_TEST_ONLY |
				  DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
}

static void blit_plane_cairo(data_t *data, cairo_surface_t *result,
			     uint32_t src_w, uint32_t src_h,
			     uint32_t src_x, uint32_t src_y,
			     uint32_t crtc_w, uint32_t crtc_h,
			     int32_t crtc_x, int32_t crtc_y,
			     struct igt_fb *fb)
{
	cairo_surface_t *surface;
	cairo_surface_t *clipped_surface;
	cairo_t *cr;

	surface = igt_get_cairo_surface(data->drm_fd, fb);

	if (src_x || src_y) {
		clipped_surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
							     src_w, src_h);

		cr = cairo_create(clipped_surface);

		cairo_translate(cr, -1. * src_x, -1. * src_y);

		cairo_set_source_surface(cr, surface, 0, 0);

		cairo_paint(cr);
		cairo_surface_flush(clipped_surface);

		cairo_destroy(cr);
	} else {
		clipped_surface = surface;
	}

	cr = cairo_create(result);

	cairo_translate(cr, crtc_x, crtc_y);

	if (src_w != crtc_w || src_h != crtc_h) {
		cairo_scale(cr, (double) crtc_w / src_w,
			    (double) crtc_h / src_h);
	}

	cairo_set_source_surface(cr, clipped_surface, 0, 0);
	cairo_surface_destroy(clipped_surface);

	if (src_w != crtc_w || src_h != crtc_h) {
		cairo_pattern_set_filter(cairo_get_source(cr),
					 CAIRO_FILTER_BILINEAR);
		cairo_pattern_set_extend(cairo_get_source(cr),
					 CAIRO_EXTEND_NONE);
	}

	cairo_paint(cr);
	cairo_surface_flush(result);

	cairo_destroy(cr);
}

static void prepare_randomized_plane(data_t *data,
				     drmModeModeInfo *mode,
				     igt_plane_t *plane,
				     struct igt_fb *overlay_fb,
				     unsigned int index,
				     cairo_surface_t *result_surface,
				     bool allow_scaling, bool allow_yuv)
{
	struct igt_fb pattern_fb;
	uint32_t overlay_fb_w, overlay_fb_h;
	uint32_t overlay_src_w, overlay_src_h;
	uint32_t overlay_src_x, overlay_src_y;
	int32_t overlay_crtc_x, overlay_crtc_y;
	uint32_t overlay_crtc_w, overlay_crtc_h;
	uint32_t format;
	uint64_t modifier;
	size_t stride;
	bool tiled;
	int fb_id;

	randomize_plane_setup(data, plane, mode, &overlay_fb_w, &overlay_fb_h,
			      &format, &modifier, allow_yuv);

	tiled = (modifier != DRM_FORMAT_MOD_LINEAR);
	igt_debug("Plane %d: framebuffer size %dx%d %s format (%s)\n",
		  index, overlay_fb_w, overlay_fb_h,
		  igt_format_str(format), tiled ? "tiled" : "linear");

	/* Get a pattern framebuffer for the overlay plane. */
	fb_id = chamelium_get_pattern_fb(data, overlay_fb_w, overlay_fb_h,
					 DRM_FORMAT_XRGB8888, 32, &pattern_fb);
	igt_assert(fb_id > 0);

	randomize_plane_stride(data, overlay_fb_w, overlay_fb_h,
			       format, modifier, &stride);

	igt_debug("Plane %d: stride %ld\n", index, stride);

	fb_id = igt_fb_convert_with_stride(overlay_fb, &pattern_fb, format,
					   modifier, stride);
	igt_assert(fb_id > 0);

	randomize_plane_coordinates(data, plane, mode, overlay_fb,
				    &overlay_src_w, &overlay_src_h,
				    &overlay_src_x, &overlay_src_y,
				    &overlay_crtc_w, &overlay_crtc_h,
				    &overlay_crtc_x, &overlay_crtc_y,
				    allow_scaling);

	igt_debug("Plane %d: in-framebuffer size %dx%d\n", index,
		  overlay_src_w, overlay_src_h);
	igt_debug("Plane %d: in-framebuffer position %dx%d\n", index,
		  overlay_src_x, overlay_src_y);
	igt_debug("Plane %d: on-crtc size %dx%d\n", index,
		  overlay_crtc_w, overlay_crtc_h);
	igt_debug("Plane %d: on-crtc position %dx%d\n", index,
		  overlay_crtc_x, overlay_crtc_y);

	blit_plane_cairo(data, result_surface, overlay_src_w, overlay_src_h,
			 overlay_src_x, overlay_src_y,
			 overlay_crtc_w, overlay_crtc_h,
			 overlay_crtc_x, overlay_crtc_y, &pattern_fb);

	/* Remove the original pattern framebuffer. */
	igt_remove_fb(data->drm_fd, &pattern_fb);
}

static const char test_display_planes_random_desc[] =
	"Setup a few overlay planes with random parameters, capture the frame "
	"and check it matches the expected output";
static void test_display_planes_random(data_t *data,
				       struct chamelium_port *port,
				       enum chamelium_check check)
{
	igt_output_t *output;
	drmModeModeInfo *mode;
	igt_plane_t *primary_plane;
	struct igt_fb primary_fb;
	struct igt_fb result_fb;
	struct igt_fb *overlay_fbs;
	igt_crc_t *crc;
	igt_crc_t *expected_crc;
	struct chamelium_fb_crc_async_data *fb_crc;
	unsigned int overlay_planes_max = 0;
	unsigned int overlay_planes_count;
	cairo_surface_t *result_surface;
	int captured_frame_count;
	bool allow_scaling;
	bool allow_yuv;
	unsigned int i;
	unsigned int fb_id;

	switch (check) {
	case CHAMELIUM_CHECK_CRC:
		allow_scaling = false;
		allow_yuv = false;
		break;
	case CHAMELIUM_CHECK_CHECKERBOARD:
		allow_scaling = true;
		allow_yuv = true;
		break;
	default:
		igt_assert(false);
	}

	srand(time(NULL));

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium,
			      port, data->ports, data->port_count);

	/* Find the connector and pipe. */
	output = prepare_output(data, port, IGT_CUSTOM_EDID_BASE);

	mode = igt_output_get_mode(output);

	/* Get a framebuffer for the primary plane. */
	primary_plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary_plane);

	fb_id = chamelium_get_pattern_fb(data, mode->hdisplay, mode->vdisplay,
					 DRM_FORMAT_XRGB8888, 64, &primary_fb);
	igt_assert(fb_id > 0);

	/* Get a framebuffer for the cairo composition result. */
	fb_id = igt_create_fb(data->drm_fd, mode->hdisplay,
			      mode->vdisplay, DRM_FORMAT_XRGB8888,
			      DRM_FORMAT_MOD_LINEAR, &result_fb);
	igt_assert(fb_id > 0);

	result_surface = igt_get_cairo_surface(data->drm_fd, &result_fb);

	/* Paint the primary framebuffer on the result surface. */
	blit_plane_cairo(data, result_surface, 0, 0, 0, 0, 0, 0, 0, 0,
			 &primary_fb);

	/* Configure the primary plane. */
	igt_plane_set_fb(primary_plane, &primary_fb);

	overlay_planes_max =
		igt_output_count_plane_type(output, DRM_PLANE_TYPE_OVERLAY);

	/* Limit the number of planes to a reasonable scene. */
	overlay_planes_max = min(overlay_planes_max, 4u);

	overlay_planes_count = (rand() % overlay_planes_max) + 1;
	igt_debug("Using %d overlay planes\n", overlay_planes_count);

	overlay_fbs = calloc(sizeof(struct igt_fb), overlay_planes_count);

	for (i = 0; i < overlay_planes_count; i++) {
		struct igt_fb *overlay_fb = &overlay_fbs[i];
		igt_plane_t *plane =
			igt_output_get_plane_type_index(output,
							DRM_PLANE_TYPE_OVERLAY,
							i);
		igt_assert(plane);

		prepare_randomized_plane(data, mode, plane, overlay_fb, i,
					 result_surface, allow_scaling,
					 allow_yuv);
	}

	cairo_surface_destroy(result_surface);

	if (check == CHAMELIUM_CHECK_CRC)
		fb_crc = chamelium_calculate_fb_crc_async_start(data->drm_fd,
								&result_fb);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	if (check == CHAMELIUM_CHECK_CRC) {
		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 1);
		crc = chamelium_read_captured_crcs(data->chamelium,
						   &captured_frame_count);

		igt_assert(captured_frame_count == 1);

		expected_crc = chamelium_calculate_fb_crc_async_finish(fb_crc);

		chamelium_assert_crc_eq_or_dump(data->chamelium,
						expected_crc, crc,
						&result_fb, 0);

		free(expected_crc);
		free(crc);
	} else if (check == CHAMELIUM_CHECK_CHECKERBOARD) {
		struct chamelium_frame_dump *dump;

		dump = chamelium_port_dump_pixels(data->chamelium, port, 0, 0,
						  0, 0);
		chamelium_assert_frame_match_or_dump(data->chamelium, port,
						     dump, &result_fb, check);
		chamelium_destroy_frame_dump(dump);
	}

	for (i = 0; i < overlay_planes_count; i++)
		igt_remove_fb(data->drm_fd, &overlay_fbs[i]);

	free(overlay_fbs);

	igt_remove_fb(data->drm_fd, &primary_fb);
	igt_remove_fb(data->drm_fd, &result_fb);
}

static const char test_hpd_without_ddc_desc[] =
	"Disable DDC on a VGA connector, check we still get a uevent on hotplug";
static void
test_hpd_without_ddc(data_t *data, struct chamelium_port *port)
{
	struct udev_monitor *mon = igt_watch_uevents();

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium,
			      port, data->ports, data->port_count);
	igt_flush_uevents(mon);

	/* Disable the DDC on the connector and make sure we still get a
	 * hotplug
	 */
	chamelium_port_set_ddc_state(data->chamelium, port, false);
	chamelium_plug(data->chamelium, port);

	igt_assert(igt_hotplug_detected(mon, CHAMELIUM_HOTPLUG_TIMEOUT));
	igt_assert_eq(chamelium_reprobe_connector(&data->display,
						  data->chamelium, port),
						  DRM_MODE_CONNECTED);

	igt_cleanup_uevents(mon);
}

static const char test_hpd_storm_detect_desc[] =
	"Trigger a series of hotplugs in a very small timeframe to simulate a"
	"bad cable, check the kernel falls back to polling to avoid a hotplug "
	"storm";
static void
test_hpd_storm_detect(data_t *data, struct chamelium_port *port, int width)
{
	struct udev_monitor *mon;
	int count = 0;

	igt_require_hpd_storm_ctl(data->drm_fd);
	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium,
			      port, data->ports, data->port_count);

	igt_hpd_storm_set_threshold(data->drm_fd, 1);
	chamelium_fire_hpd_pulses(data->chamelium, port, width, 10);
	igt_assert(igt_hpd_storm_detected(data->drm_fd));

	mon = igt_watch_uevents();
	chamelium_fire_hpd_pulses(data->chamelium, port, width, 10);

	/*
	 * Polling should have been enabled by the HPD storm at this point,
	 * so we should only get at most 1 hotplug event
	 */
	igt_until_timeout(5)
		count += igt_hotplug_detected(mon, 1);
	igt_assert_lt(count, 2);

	igt_cleanup_uevents(mon);
	igt_hpd_storm_reset(data->drm_fd);
}

static const char test_hpd_storm_disable_desc[] =
	"Disable HPD storm detection, trigger a storm and check the kernel "
	"doesn't detect one";
static void
test_hpd_storm_disable(data_t *data, struct chamelium_port *port, int width)
{
	igt_require_hpd_storm_ctl(data->drm_fd);
	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium,
			      port, data->ports, data->port_count);

	igt_hpd_storm_set_threshold(data->drm_fd, 0);
	chamelium_fire_hpd_pulses(data->chamelium, port,
				  width, 10);
	igt_assert(!igt_hpd_storm_detected(data->drm_fd));

	igt_hpd_storm_reset(data->drm_fd);
}

static const char igt_edid_stress_resolution_desc[] =
	"Stress test the DUT by testing multiple EDIDs, one right after the other,"
	"and ensure their validity by check the real screen resolution vs the"
	"advertised mode resultion.";
static void edid_stress_resolution(data_t *data, struct chamelium_port *port,
				   monitor_edid edids_list[],
				   size_t edids_list_len)
{
	int i;
	struct chamelium *chamelium = data->chamelium;
	struct udev_monitor *mon = igt_watch_uevents();

	for (i = 0; i < edids_list_len; ++i) {
		struct chamelium_edid *chamelium_edid;
		drmModeModeInfo mode;
		struct igt_fb fb = { 0 };
		igt_output_t *output;
		enum pipe pipe;
		bool is_video_stable;
		int screen_res_w, screen_res_h;

		monitor_edid *edid = &edids_list[i];
		igt_info("Testing out the EDID for %s\n",
			 monitor_edid_get_name(edid));

		/* Getting and Setting the EDID on Chamelium. */
		chamelium_edid =
			get_chameleon_edid_from_monitor_edid(chamelium, edid);
		chamelium_port_set_edid(data->chamelium, port, chamelium_edid);
		free_chamelium_edid_from_monitor_edid(chamelium_edid);

		igt_flush_uevents(mon);
		chamelium_plug(chamelium, port);
		wait_for_connector_after_hotplug(data, mon, port,
						 DRM_MODE_CONNECTED);
		igt_flush_uevents(mon);

		/* Setting an output on the screen to turn it on. */
		mode = get_mode_for_port(chamelium, port);
		create_fb_for_mode(data, &fb, &mode);
		output = get_output_for_port(data, port);
		pipe = get_pipe_for_output(&data->display, output);
		igt_output_set_pipe(output, pipe);
		enable_output(data, port, output, &mode, &fb);

		/* Capture the screen resolution and verify. */
		is_video_stable = chamelium_port_wait_video_input_stable(
			chamelium, port, 5);
		igt_assert(is_video_stable);

		chamelium_port_get_resolution(chamelium, port, &screen_res_w,
					      &screen_res_h);
		igt_assert(screen_res_w == fb.width);
		igt_assert(screen_res_h == fb.height);

		// Clean up
		igt_remove_fb(data->drm_fd, &fb);
		igt_modeset_disable_all_outputs(&data->display);
		chamelium_unplug(chamelium, port);
	}

	chamelium_reset_state(&data->display, data->chamelium, port,
			      data->ports, data->port_count);
}

static const char igt_edid_resolution_list_desc[] =
	"Get an EDID with many modes of different configurations, set them on the screen and check the"
	" screen resolution matches the mode resolution.";

static void edid_resolution_list(data_t *data, struct chamelium_port *port)
{
	struct chamelium *chamelium = data->chamelium;
	struct udev_monitor *mon = igt_watch_uevents();
	drmModeConnector *connector;
	drmModeModeInfoPtr modes;
	int count_modes;
	int i;
	igt_output_t *output;
	enum pipe pipe;

	chamelium_unplug(chamelium, port);
	set_edid(data, port, IGT_CUSTOM_EDID_FULL);

	igt_flush_uevents(mon);
	chamelium_plug(chamelium, port);
	wait_for_connector_after_hotplug(data, mon, port, DRM_MODE_CONNECTED);
	igt_flush_uevents(mon);

	connector = chamelium_port_get_connector(chamelium, port, true);
	modes = connector->modes;
	count_modes = connector->count_modes;

	output = get_output_for_port(data, port);
	pipe = get_pipe_for_output(&data->display, output);
	igt_output_set_pipe(output, pipe);

	for (i = 0; i < count_modes; ++i)
		igt_debug("#%d %s %uHz\n", i, modes[i].name, modes[i].vrefresh);

	for (i = 0; i < count_modes; ++i) {
		struct igt_fb fb = { 0 };
		bool is_video_stable;
		int screen_res_w, screen_res_h;

		igt_info("Testing #%d %s %uHz\n", i, modes[i].name,
			 modes[i].vrefresh);

		/* Set the screen mode with the one we chose. */
		create_fb_for_mode(data, &fb, &modes[i]);
		enable_output(data, port, output, &modes[i], &fb);
		is_video_stable = chamelium_port_wait_video_input_stable(
			chamelium, port, 10);
		igt_assert(is_video_stable);

		chamelium_port_get_resolution(chamelium, port, &screen_res_w,
					      &screen_res_h);
		igt_assert_eq(screen_res_w, modes[i].hdisplay);
		igt_assert_eq(screen_res_h, modes[i].vdisplay);

		igt_remove_fb(data->drm_fd, &fb);
	}

	igt_modeset_disable_all_outputs(&data->display);
	drmModeFreeConnector(connector);
}

#define for_each_port(p, port)            \
	for (p = 0, port = data.ports[p]; \
	     p < data.port_count;         \
	     p++, port = data.ports[p])

#define connector_subtest(name__, type__)                    \
	igt_subtest(name__)                                  \
		for_each_port(p, port)                       \
			if (chamelium_port_get_type(port) == \
			    DRM_MODE_CONNECTOR_ ## type__)

#define connector_dynamic_subtest(name__, type__)            \
	igt_subtest_with_dynamic(name__)                     \
		for_each_port(p, port)                       \
			if (chamelium_port_get_type(port) == \
			    DRM_MODE_CONNECTOR_ ## type__)


static data_t data;

IGT_TEST_DESCRIPTION("Tests requiring a Chamelium board");
igt_main
{
	struct chamelium_port *port;
	int p;
	size_t i;

	igt_fixture {
		/* So fbcon doesn't try to reprobe things itself */
		kmstest_set_vt_graphics_mode();

		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);

		/*
		 * XXX: disabling modeset, can be removed when
		 * igt_display_require will start doing this for us
		 */
		igt_display_commit2(&data.display, COMMIT_ATOMIC);

		/* we need to initalize chamelium after igt_display_require */
		data.chamelium = chamelium_init(data.drm_fd, &data.display);
		igt_require(data.chamelium);

		data.ports = chamelium_get_ports(data.chamelium,
						 &data.port_count);

		for (i = 0; i < IGT_CUSTOM_EDID_COUNT; ++i) {
			data.edids[i] = chamelium_new_edid(data.chamelium,
							   igt_kms_get_custom_edid(i));
		}
	}

	igt_describe("DisplayPort tests");
	igt_subtest_group {
		igt_fixture {
			chamelium_require_connector_present(data.ports, DRM_MODE_CONNECTOR_DisplayPort,
							    data.port_count, 1);
		}

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("dp-hpd", DisplayPort)
			test_hotplug(&data, port,
				     HPD_TOGGLE_COUNT_DP_HDMI,
				     TEST_MODESET_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("dp-hpd-fast", DisplayPort)
			test_hotplug(&data, port,
				     HPD_TOGGLE_COUNT_FAST,
				     TEST_MODESET_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("dp-hpd-enable-disable-mode", DisplayPort)
			test_hotplug(&data, port,
				     HPD_TOGGLE_COUNT_FAST,
				     TEST_MODESET_ON_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("dp-hpd-with-enabled-mode", DisplayPort)
			test_hotplug(&data, port,
				     HPD_TOGGLE_COUNT_FAST,
				     TEST_MODESET_ON);

		igt_describe(igt_custom_edid_type_read_desc);
		connector_subtest("dp-edid-read", DisplayPort) {
			igt_custom_edid_type_read(&data, port, IGT_CUSTOM_EDID_BASE);
			igt_custom_edid_type_read(&data, port, IGT_CUSTOM_EDID_ALT);
		}

		igt_describe(igt_edid_stress_resolution_desc);
		connector_subtest("dp-edid-stress-resolution-4k", DisplayPort)
			edid_stress_resolution(&data, port, DP_EDIDS_4K,
					       ARRAY_SIZE(DP_EDIDS_4K));

		igt_describe(igt_edid_stress_resolution_desc);
		connector_subtest("dp-edid-stress-resolution-non-4k",
				  DisplayPort)
			edid_stress_resolution(&data, port, DP_EDIDS_NON_4K,
					       ARRAY_SIZE(DP_EDIDS_NON_4K));

		igt_describe(igt_edid_resolution_list_desc);
		connector_subtest("dp-edid-resolution-list", DisplayPort)
			edid_resolution_list(&data, port);

		igt_describe(test_suspend_resume_hpd_desc);
		connector_subtest("dp-hpd-after-suspend", DisplayPort)
			test_suspend_resume_hpd(&data, port,
						SUSPEND_STATE_MEM,
						SUSPEND_TEST_NONE);

		igt_describe(test_suspend_resume_hpd_desc);
		connector_subtest("dp-hpd-after-hibernate", DisplayPort)
			test_suspend_resume_hpd(&data, port,
						SUSPEND_STATE_DISK,
						SUSPEND_TEST_DEVICES);

		igt_describe(test_hpd_storm_detect_desc);
		connector_subtest("dp-hpd-storm", DisplayPort)
			test_hpd_storm_detect(&data, port,
					      HPD_STORM_PULSE_INTERVAL_DP);

		igt_describe(test_hpd_storm_disable_desc);
		connector_subtest("dp-hpd-storm-disable", DisplayPort)
			test_hpd_storm_disable(&data, port,
					       HPD_STORM_PULSE_INTERVAL_DP);

		igt_describe(test_suspend_resume_edid_change_desc);
		connector_subtest("dp-edid-change-during-suspend", DisplayPort)
			test_suspend_resume_edid_change(&data, port,
							SUSPEND_STATE_MEM,
							SUSPEND_TEST_NONE,
							IGT_CUSTOM_EDID_BASE,
							IGT_CUSTOM_EDID_ALT);

		igt_describe(test_suspend_resume_edid_change_desc);
		connector_subtest("dp-edid-change-during-hibernate", DisplayPort)
			test_suspend_resume_edid_change(&data, port,
							SUSPEND_STATE_DISK,
							SUSPEND_TEST_DEVICES,
							IGT_CUSTOM_EDID_BASE,
							IGT_CUSTOM_EDID_ALT);

		igt_describe(test_display_all_modes_desc);
		connector_subtest("dp-crc-single", DisplayPort)
			test_display_all_modes(&data, port, DRM_FORMAT_XRGB8888,
					       CHAMELIUM_CHECK_CRC, 1);

		igt_describe(test_display_one_mode_desc);
		connector_subtest("dp-crc-fast", DisplayPort)
			test_display_one_mode(&data, port, DRM_FORMAT_XRGB8888,
					      CHAMELIUM_CHECK_CRC, 1);

		igt_describe(test_display_all_modes_desc);
		connector_subtest("dp-crc-multiple", DisplayPort)
			test_display_all_modes(&data, port, DRM_FORMAT_XRGB8888,
					       CHAMELIUM_CHECK_CRC, 3);

		igt_describe(test_display_frame_dump_desc);
		connector_subtest("dp-frame-dump", DisplayPort)
			test_display_frame_dump(&data, port);

		igt_describe(test_mode_timings_desc);
		connector_subtest("dp-mode-timings", DisplayPort)
			test_mode_timings(&data, port);

		igt_describe(test_display_audio_desc);
		connector_subtest("dp-audio", DisplayPort)
			test_display_audio(&data, port, "HDMI",
					   IGT_CUSTOM_EDID_DP_AUDIO);

		igt_describe(test_display_audio_edid_desc);
		connector_subtest("dp-audio-edid", DisplayPort)
			test_display_audio_edid(&data, port,
						IGT_CUSTOM_EDID_DP_AUDIO);

		igt_describe(test_hotplug_for_each_pipe_desc);
		connector_subtest("dp-hpd-for-each-pipe", DisplayPort)
			test_hotplug_for_each_pipe(&data, port);
	}

	igt_describe("HDMI tests");
	igt_subtest_group {
		igt_fixture {
			chamelium_require_connector_present(data.ports, DRM_MODE_CONNECTOR_HDMIA,
							    data.port_count, 1);
		}

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("hdmi-hpd", HDMIA)
			test_hotplug(&data, port,
				     HPD_TOGGLE_COUNT_DP_HDMI,
				     TEST_MODESET_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("hdmi-hpd-fast", HDMIA)
			test_hotplug(&data, port,
				     HPD_TOGGLE_COUNT_FAST,
				     TEST_MODESET_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("hdmi-hpd-enable-disable-mode", HDMIA)
			test_hotplug(&data, port,
				     HPD_TOGGLE_COUNT_FAST,
				     TEST_MODESET_ON_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("hdmi-hpd-with-enabled-mode", HDMIA)
			test_hotplug(&data, port,
				     HPD_TOGGLE_COUNT_FAST,
				     TEST_MODESET_ON);

		igt_describe(igt_custom_edid_type_read_desc);
		connector_subtest("hdmi-edid-read", HDMIA) {
			igt_custom_edid_type_read(&data, port, IGT_CUSTOM_EDID_BASE);
			igt_custom_edid_type_read(&data, port, IGT_CUSTOM_EDID_ALT);
		}

		igt_describe(igt_edid_stress_resolution_desc);
		connector_subtest("hdmi-edid-stress-resolution-4k", HDMIA)
			edid_stress_resolution(&data, port, HDMI_EDIDS_4K,
					       ARRAY_SIZE(HDMI_EDIDS_4K));

		igt_describe(igt_edid_stress_resolution_desc);
		connector_subtest("hdmi-edid-stress-resolution-non-4k", HDMIA)
			edid_stress_resolution(&data, port, HDMI_EDIDS_NON_4K,
					       ARRAY_SIZE(HDMI_EDIDS_NON_4K));

		igt_describe(test_suspend_resume_hpd_desc);
		connector_subtest("hdmi-hpd-after-suspend", HDMIA)
			test_suspend_resume_hpd(&data, port,
						SUSPEND_STATE_MEM,
						SUSPEND_TEST_NONE);

		igt_describe(test_suspend_resume_hpd_desc);
		connector_subtest("hdmi-hpd-after-hibernate", HDMIA)
			test_suspend_resume_hpd(&data, port,
						SUSPEND_STATE_DISK,
						SUSPEND_TEST_DEVICES);

		igt_describe(test_hpd_storm_detect_desc);
		connector_subtest("hdmi-hpd-storm", HDMIA)
			test_hpd_storm_detect(&data, port,
					      HPD_STORM_PULSE_INTERVAL_HDMI);

		igt_describe(test_hpd_storm_disable_desc);
		connector_subtest("hdmi-hpd-storm-disable", HDMIA)
			test_hpd_storm_disable(&data, port,
					       HPD_STORM_PULSE_INTERVAL_HDMI);

		igt_describe(test_suspend_resume_edid_change_desc);
		connector_subtest("hdmi-edid-change-during-suspend", HDMIA)
			test_suspend_resume_edid_change(&data, port,
							SUSPEND_STATE_MEM,
							SUSPEND_TEST_NONE,
							IGT_CUSTOM_EDID_BASE,
							IGT_CUSTOM_EDID_ALT);

		igt_describe(test_suspend_resume_edid_change_desc);
		connector_subtest("hdmi-edid-change-during-hibernate", HDMIA)
			test_suspend_resume_edid_change(&data, port,
							SUSPEND_STATE_DISK,
							SUSPEND_TEST_DEVICES,
							IGT_CUSTOM_EDID_BASE,
							IGT_CUSTOM_EDID_ALT);

		igt_describe(test_display_all_modes_desc);
		connector_subtest("hdmi-crc-single", HDMIA)
			test_display_all_modes(&data, port, DRM_FORMAT_XRGB8888,
					       CHAMELIUM_CHECK_CRC, 1);

		igt_describe(test_display_one_mode_desc);
		connector_subtest("hdmi-crc-fast", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_XRGB8888,
					      CHAMELIUM_CHECK_CRC, 1);

		igt_describe(test_display_all_modes_desc);
		connector_subtest("hdmi-crc-multiple", HDMIA)
			test_display_all_modes(&data, port, DRM_FORMAT_XRGB8888,
					       CHAMELIUM_CHECK_CRC, 3);

		igt_describe(test_display_one_mode_desc);
		connector_dynamic_subtest("hdmi-crc-nonplanar-formats", HDMIA) {
			int k;
			igt_output_t *output;
			igt_plane_t *primary;

			output = prepare_output(&data, port, IGT_CUSTOM_EDID_BASE);
			primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
			igt_assert(primary);

			for (k = 0; k < primary->format_mod_count; k++) {
				if (!igt_fb_supported_format(primary->formats[k]))
					continue;

				if (igt_format_is_yuv(primary->formats[k]))
					continue;

				if (primary->modifiers[k] != DRM_FORMAT_MOD_LINEAR)
					continue;

				igt_dynamic_f("%s", igt_format_str(primary->formats[k]))
					test_display_one_mode(&data, port, primary->formats[k],
							      CHAMELIUM_CHECK_CRC, 1);
			}
		}

		igt_describe(test_display_planes_random_desc);
		connector_subtest("hdmi-crc-planes-random", HDMIA)
			test_display_planes_random(&data, port,
						   CHAMELIUM_CHECK_CRC);

		igt_describe(test_display_one_mode_desc);
		connector_dynamic_subtest("hdmi-cmp-planar-formats", HDMIA) {
			int k;
			igt_output_t *output;
			igt_plane_t *primary;

			output = prepare_output(&data, port, IGT_CUSTOM_EDID_BASE);
			primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
			igt_assert(primary);

			for (k = 0; k < primary->format_mod_count; k++) {
				if (!igt_fb_supported_format(primary->formats[k]))
					continue;

				if (!igt_format_is_yuv(primary->formats[k]))
					continue;

				if (primary->modifiers[k] != DRM_FORMAT_MOD_LINEAR)
					continue;

				igt_dynamic_f("%s", igt_format_str(primary->formats[k]))
					test_display_one_mode(&data, port, primary->formats[k],
							      CHAMELIUM_CHECK_CHECKERBOARD, 1);
			}
		}

		igt_describe(test_display_planes_random_desc);
		connector_subtest("hdmi-cmp-planes-random", HDMIA)
			test_display_planes_random(&data, port,
						   CHAMELIUM_CHECK_CHECKERBOARD);

		igt_describe(test_display_frame_dump_desc);
		connector_subtest("hdmi-frame-dump", HDMIA)
			test_display_frame_dump(&data, port);

		igt_describe(test_mode_timings_desc);
		connector_subtest("hdmi-mode-timings", HDMIA)
			test_mode_timings(&data, port);

		igt_describe(test_display_audio_desc);
		connector_subtest("hdmi-audio", HDMIA)
			test_display_audio(&data, port, "HDMI",
					   IGT_CUSTOM_EDID_HDMI_AUDIO);

		igt_describe(test_display_audio_edid_desc);
		connector_subtest("hdmi-audio-edid", HDMIA)
			test_display_audio_edid(&data, port,
						IGT_CUSTOM_EDID_HDMI_AUDIO);

		igt_describe(test_display_aspect_ratio_desc);
		connector_subtest("hdmi-aspect-ratio", HDMIA)
			test_display_aspect_ratio(&data, port);

		igt_describe(test_hotplug_for_each_pipe_desc);
		connector_subtest("hdmi-hpd-for-each-pipe", HDMIA)
			test_hotplug_for_each_pipe(&data, port);
	}

	igt_describe("VGA tests");
	igt_subtest_group {
		igt_fixture {
			chamelium_require_connector_present(data.ports, DRM_MODE_CONNECTOR_VGA,
							    data.port_count, 1);
		}

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("vga-hpd", VGA)
			test_hotplug(&data, port, HPD_TOGGLE_COUNT_VGA,
				     TEST_MODESET_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("vga-hpd-fast", VGA)
			test_hotplug(&data, port, HPD_TOGGLE_COUNT_FAST,
				     TEST_MODESET_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("vga-hpd-enable-disable-mode", VGA)
			test_hotplug(&data, port,
				     HPD_TOGGLE_COUNT_FAST,
				     TEST_MODESET_ON_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("vga-hpd-with-enabled-mode", VGA)
			test_hotplug(&data, port,
				     HPD_TOGGLE_COUNT_FAST,
				     TEST_MODESET_ON);

		igt_describe(igt_custom_edid_type_read_desc);
		connector_subtest("vga-edid-read", VGA) {
			igt_custom_edid_type_read(&data, port, IGT_CUSTOM_EDID_BASE);
			igt_custom_edid_type_read(&data, port, IGT_CUSTOM_EDID_ALT);
		}

		igt_describe(test_suspend_resume_hpd_desc);
		connector_subtest("vga-hpd-after-suspend", VGA)
			test_suspend_resume_hpd(&data, port,
						SUSPEND_STATE_MEM,
						SUSPEND_TEST_NONE);

		igt_describe(test_suspend_resume_hpd_desc);
		connector_subtest("vga-hpd-after-hibernate", VGA)
			test_suspend_resume_hpd(&data, port,
						SUSPEND_STATE_DISK,
						SUSPEND_TEST_DEVICES);

		igt_describe(test_hpd_without_ddc_desc);
		connector_subtest("vga-hpd-without-ddc", VGA)
			test_hpd_without_ddc(&data, port);

		igt_describe(test_display_all_modes_desc);
		connector_subtest("vga-frame-dump", VGA)
			test_display_all_modes(&data, port, DRM_FORMAT_XRGB8888,
					       CHAMELIUM_CHECK_ANALOG, 1);
	}

	igt_describe("Tests that operate on all connectors");
	igt_subtest_group {

		igt_fixture {
			igt_require(data.port_count);
		}

		igt_describe(test_suspend_resume_hpd_common_desc);
		igt_subtest("common-hpd-after-suspend")
			test_suspend_resume_hpd_common(&data,
						       SUSPEND_STATE_MEM,
						       SUSPEND_TEST_NONE);

		igt_describe(test_suspend_resume_hpd_common_desc);
		igt_subtest("common-hpd-after-hibernate")
			test_suspend_resume_hpd_common(&data,
						       SUSPEND_STATE_DISK,
						       SUSPEND_TEST_DEVICES);
	}

	igt_describe(test_hotplug_for_each_pipe_desc);
	connector_subtest("vga-hpd-for-each-pipe", VGA)
		test_hotplug_for_each_pipe(&data, port);

	igt_fixture {
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
