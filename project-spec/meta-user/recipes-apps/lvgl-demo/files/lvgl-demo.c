#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "lvgl.h"

#define DISPLAY_WIDTH 1280
#define DISPLAY_HEIGHT 720
#define DISPLAY_REFRESH 60

struct framebuffer {
	int fd;
	uint8_t *memory;
	size_t memory_size;
	struct fb_fix_screeninfo fixed;
	struct fb_var_screeninfo variable;
	unsigned int bytes_per_pixel;
};

static struct framebuffer fb = { .fd = -1 };
static int drm_fd = -1;
static uint32_t display_width = DISPLAY_WIDTH;
static uint32_t display_height = DISPLAY_HEIGHT;
static volatile sig_atomic_t keep_running = 1;
static lv_obj_t *progress_bar;
static lv_obj_t *progress_label;
static int progress_value;
static int progress_direction = 1;

static void stop_handler(int signal_number)
{
	(void)signal_number;
	keep_running = 0;
}

static int framebuffer_open(struct framebuffer *framebuffer, const char *device)
{
	framebuffer->fd = open(device, O_RDWR);
	if (framebuffer->fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", device, strerror(errno));
		return -1;
	}

	if (ioctl(framebuffer->fd, FBIOGET_VSCREENINFO, &framebuffer->variable) < 0 ||
	    ioctl(framebuffer->fd, FBIOGET_FSCREENINFO, &framebuffer->fixed) < 0) {
		fprintf(stderr, "read framebuffer info failed: %s\n", strerror(errno));
		close(framebuffer->fd);
		framebuffer->fd = -1;
		return -1;
	}

	framebuffer->bytes_per_pixel = (framebuffer->variable.bits_per_pixel + 7U) / 8U;
	if (framebuffer->variable.xres == 0 || framebuffer->variable.yres == 0 ||
	    (framebuffer->variable.bits_per_pixel != 16 &&
	     framebuffer->variable.bits_per_pixel != 24 &&
	     framebuffer->variable.bits_per_pixel != 32) ||
	    framebuffer->fixed.visual != FB_VISUAL_TRUECOLOR ||
	    framebuffer->fixed.line_length <
		framebuffer->variable.xres_virtual * framebuffer->bytes_per_pixel ||
	    framebuffer->variable.xoffset + framebuffer->variable.xres >
		framebuffer->variable.xres_virtual ||
	    framebuffer->variable.yoffset + framebuffer->variable.yres >
		framebuffer->variable.yres_virtual ||
	    framebuffer->fixed.smem_len == 0) {
		fprintf(stderr, "unsupported framebuffer format or geometry\n");
		close(framebuffer->fd);
		framebuffer->fd = -1;
		return -1;
	}

	framebuffer->memory_size = framebuffer->fixed.smem_len;
	framebuffer->memory = mmap(NULL, framebuffer->memory_size,
				   PROT_READ | PROT_WRITE, MAP_SHARED,
				   framebuffer->fd, 0);
	if (framebuffer->memory == MAP_FAILED) {
		fprintf(stderr, "mmap %s failed: %s\n", device, strerror(errno));
		framebuffer->memory = NULL;
		close(framebuffer->fd);
		framebuffer->fd = -1;
		return -1;
	}

	return 0;
}

static int framebuffer_set_720p(void)
{
	drmModeResPtr resources;
	drmModeConnectorPtr connector = NULL;
	drmModeEncoderPtr encoder = NULL;
	drmModeCrtcPtr crtc = NULL;
	drmModeFBPtr drm_fb = NULL;
	drmModeModeInfo mode;
	uint32_t connector_id = 0;
	uint32_t crtc_id = 0;
	int fd;
	int i;
	int j;
	int result = -1;
	int mode_found = 0;

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open /dev/dri/card0 failed: %s\n", strerror(errno));
		return -1;
	}

	resources = drmModeGetResources(fd);
	if (resources == NULL) {
		fprintf(stderr, "read DRM resources failed: %s\n", strerror(errno));
		goto out;
	}

	for (i = 0; i < resources->count_connectors; ++i) {
		drmModeConnectorPtr candidate = drmModeGetConnector(fd, resources->connectors[i]);
		drmModeModeInfoPtr candidate_mode = NULL;
		drmModeModeInfoPtr fallback_mode = NULL;

		if (candidate == NULL)
			continue;
		if (candidate->connection != DRM_MODE_CONNECTED ||
		    candidate->connector_type != DRM_MODE_CONNECTOR_HDMIA) {
			drmModeFreeConnector(candidate);
			continue;
		}

		for (j = 0; j < candidate->count_modes; ++j) {
			if (candidate->modes[j].hdisplay != DISPLAY_WIDTH ||
			    candidate->modes[j].vdisplay != DISPLAY_HEIGHT)
				continue;
			if (candidate->modes[j].vrefresh == DISPLAY_REFRESH) {
				candidate_mode = &candidate->modes[j];
				break;
			}
			if (fallback_mode == NULL)
				fallback_mode = &candidate->modes[j];
		}
		if (candidate_mode == NULL)
			candidate_mode = fallback_mode;
		if (candidate_mode == NULL) {
			drmModeFreeConnector(candidate);
			continue;
		}

		for (j = -1; j < candidate->count_encoders; ++j) {
			uint32_t encoder_id;
			int k;

			if (j == -1) {
				if (candidate->encoder_id == 0)
					continue;
				encoder_id = candidate->encoder_id;
			} else {
				encoder_id = candidate->encoders[j];
				if (encoder_id == candidate->encoder_id)
					continue;
			}

			encoder = drmModeGetEncoder(fd, encoder_id);
			if (encoder == NULL)
				continue;
			if (encoder->crtc_id != 0) {
				crtc_id = encoder->crtc_id;
				break;
			}
			for (k = 0; k < resources->count_crtcs; ++k) {
				if (encoder->possible_crtcs & (1U << k)) {
					crtc_id = resources->crtcs[k];
					break;
				}
			}
			if (crtc_id != 0)
				break;
			drmModeFreeEncoder(encoder);
			encoder = NULL;
		}
		if (encoder == NULL || crtc_id == 0) {
			if (encoder != NULL) {
				drmModeFreeEncoder(encoder);
				encoder = NULL;
			}
			drmModeFreeConnector(candidate);
			continue;
		}

		connector = candidate;
		connector_id = connector->connector_id;
		mode = *candidate_mode;
		mode_found = 1;
		break;
	}

	if (connector == NULL || !mode_found) {
		fprintf(stderr, "no connected DRM output supports 1280x720\n");
		goto out_resources;
	}

	crtc = drmModeGetCrtc(fd, crtc_id);
	if (crtc == NULL || crtc->buffer_id == 0) {
		fprintf(stderr, "DRM CRTC has no active framebuffer\n");
		goto out_resources;
	}
	drm_fb = drmModeGetFB(fd, crtc->buffer_id);
	if (drm_fb == NULL || drm_fb->width < DISPLAY_WIDTH ||
	    drm_fb->height < DISPLAY_HEIGHT) {
		fprintf(stderr, "active DRM framebuffer is smaller than 1280x720\n");
		goto out_resources;
	}

	if (drmModeSetCrtc(fd, crtc_id, crtc->buffer_id, 0, 0,
			   &connector_id, 1, &mode) != 0) {
		fprintf(stderr, "set DRM mode 1280x720 failed: %s\n", strerror(errno));
		goto out_resources;
	}

	printf("DRM connector %u mode: %s (%ux%u@%u)\n", connector_id,
	       mode.name, DISPLAY_WIDTH, DISPLAY_HEIGHT, mode.vrefresh);
	drm_fd = fd;
	result = 0;

out_resources:
	if (drm_fb != NULL)
		drmModeFreeFB(drm_fb);
	if (crtc != NULL)
		drmModeFreeCrtc(crtc);
	if (encoder != NULL)
		drmModeFreeEncoder(encoder);
	if (connector != NULL)
		drmModeFreeConnector(connector);
	drmModeFreeResources(resources);
	if (result != 0)
		close(fd);
	return result;

out:
	close(fd);
	return -1;
}

static void framebuffer_close(struct framebuffer *framebuffer)
{
	if (framebuffer->memory != NULL)
		munmap(framebuffer->memory, framebuffer->memory_size);
	if (framebuffer->fd >= 0)
		close(framebuffer->fd);
	framebuffer->memory = NULL;
	framebuffer->fd = -1;
}

static uint32_t component_to_field(uint8_t value, struct fb_bitfield field)
{
	uint64_t maximum;
	uint64_t scaled;

	if (field.length == 0 || field.length > 8 || field.offset + field.length > 32)
		return 0;

	maximum = (1ULL << field.length) - 1ULL;
	scaled = ((uint64_t)value * maximum + 127ULL) / 255ULL;
	return (uint32_t)(scaled << field.offset);
}

static uint32_t framebuffer_pixel(lv_color_t color)
{
	lv_color32_t rgba;
	uint32_t pixel;

	rgba.full = lv_color_to32(color);
	pixel = component_to_field(rgba.ch.red, fb.variable.red) |
		component_to_field(rgba.ch.green, fb.variable.green) |
		component_to_field(rgba.ch.blue, fb.variable.blue);
	if (fb.variable.transp.length != 0)
		pixel |= component_to_field(255, fb.variable.transp);
	return pixel;
}

static void framebuffer_flush(lv_disp_drv_t *display, const lv_area_t *area,
			      lv_color_t *colors)
{
	int32_t x;
	int32_t y;
	int32_t area_width = area->x2 - area->x1 + 1;

	for (y = area->y1; y <= area->y2; ++y) {
		if (y < 0 || y >= (int32_t)display_height)
			continue;
		for (x = area->x1; x <= area->x2; ++x) {
			uint32_t pixel;
			int32_t source_index;
			size_t destination;
			unsigned int byte;

			if (x < 0 || x >= (int32_t)display_width)
				continue;

			source_index = (y - area->y1) * area_width + (x - area->x1);
			pixel = framebuffer_pixel(colors[source_index]);
			destination = (size_t)(y + fb.variable.yoffset) * fb.fixed.line_length +
				      (size_t)(x + fb.variable.xoffset) * fb.bytes_per_pixel;
			if (destination + fb.bytes_per_pixel > fb.memory_size)
				continue;

			for (byte = 0; byte < fb.bytes_per_pixel; ++byte)
				fb.memory[destination + byte] = (uint8_t)(pixel >> (byte * 8U));
		}
	}

	lv_disp_flush_ready(display);
}

static uint32_t monotonic_milliseconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((uint64_t)now.tv_sec * 1000ULL +
			  (uint64_t)now.tv_nsec / 1000000ULL);
}

static void progress_timer(lv_timer_t *timer)
{
	(void)timer;
	progress_value += progress_direction;
	if (progress_value >= 100) {
		progress_value = 100;
		progress_direction = -1;
	} else if (progress_value <= 0) {
		progress_value = 0;
		progress_direction = 1;
	}
	lv_bar_set_value(progress_bar, progress_value, LV_ANIM_OFF);
	lv_label_set_text_fmt(progress_label, "LVGL 8.3.11  |  %d%%", progress_value);
}

static void create_demo_ui(void)
{
	lv_obj_t *screen = lv_scr_act();
	lv_obj_t *title;
	lv_obj_t *button;
	lv_obj_t *button_label;
	lv_obj_t *details;
	char details_text[96];

	lv_obj_set_style_bg_color(screen, lv_color_hex(0x10243A), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

	title = lv_label_create(screen);
	lv_label_set_text(title, "PetaLinux + LVGL");
	lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 58);

	button = lv_btn_create(screen);
	lv_obj_set_size(button, 210, 66);
	lv_obj_align(button, LV_ALIGN_CENTER, 0, -32);
	lv_obj_set_style_bg_color(button, lv_color_hex(0x087E8B), LV_PART_MAIN);
	button_label = lv_label_create(button);
	lv_label_set_text(button_label, "Framebuffer OK");
	lv_obj_center(button_label);

	progress_bar = lv_bar_create(screen);
	lv_obj_set_size(progress_bar, 300, 22);
	lv_obj_align(progress_bar, LV_ALIGN_CENTER, 0, 48);
	lv_bar_set_range(progress_bar, 0, 100);
	lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);

	progress_label = lv_label_create(screen);
	lv_label_set_text(progress_label, "LVGL 8.3.11  |  0%");
	lv_obj_set_style_text_color(progress_label, lv_color_hex(0xD7E8F2), LV_PART_MAIN);
	lv_obj_align(progress_label, LV_ALIGN_CENTER, 0, 92);

	snprintf(details_text, sizeof(details_text), "%ux%u  /  %u bpp  /  /dev/fb0",
		 display_width, display_height, fb.variable.bits_per_pixel);
	details = lv_label_create(screen);
	lv_label_set_text(details, details_text);
	lv_obj_set_style_text_color(details, lv_color_hex(0x91AFC2), LV_PART_MAIN);
	lv_obj_align(details, LV_ALIGN_BOTTOM_MID, 0, -48);

	lv_timer_create(progress_timer, 30, NULL);
}

int main(int argc, char **argv)
{
	const char *device = "/dev/fb0";
	lv_color_t *draw_buffer;
	lv_disp_draw_buf_t draw_buffer_descriptor;
	lv_disp_drv_t display_driver;
	uint32_t buffer_lines;
	uint32_t previous_tick;
	struct timespec sleep_time = { .tv_sec = 0, .tv_nsec = 5000000L };

	if (argc != 1) {
		fprintf(stderr, "Usage: %s\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (framebuffer_open(&fb, device) != 0)
		return EXIT_FAILURE;

	if (fb.variable.xres < DISPLAY_WIDTH || fb.variable.yres < DISPLAY_HEIGHT) {
		fprintf(stderr, "fb0 backing buffer is too small for 1280x720\n");
		framebuffer_close(&fb);
		return EXIT_FAILURE;
	}
	if (framebuffer_set_720p() != 0) {
		framebuffer_close(&fb);
		return EXIT_FAILURE;
	}
	printf("Framebuffer %s: backing %ux%u, active %ux%u, %u bpp, line length %u\n",
	       device, fb.variable.xres, fb.variable.yres, display_width, display_height,
	       fb.variable.bits_per_pixel, fb.fixed.line_length);

	buffer_lines = display_height < 40 ? display_height : 40;
	draw_buffer = calloc((size_t)display_width * buffer_lines, sizeof(*draw_buffer));
	if (draw_buffer == NULL) {
		perror("allocate LVGL draw buffer");
		framebuffer_close(&fb);
		close(drm_fd);
		return EXIT_FAILURE;
	}

	lv_init();
	lv_disp_draw_buf_init(&draw_buffer_descriptor, draw_buffer, NULL,
			      display_width * buffer_lines);
	lv_disp_drv_init(&display_driver);
	display_driver.hor_res = display_width;
	display_driver.ver_res = display_height;
	display_driver.draw_buf = &draw_buffer_descriptor;
	display_driver.flush_cb = framebuffer_flush;
	if (lv_disp_drv_register(&display_driver) == NULL) {
		fprintf(stderr, "LVGL display registration failed\n");
		free(draw_buffer);
		framebuffer_close(&fb);
		close(drm_fd);
		return EXIT_FAILURE;
	}

	create_demo_ui();
	signal(SIGINT, stop_handler);
	signal(SIGTERM, stop_handler);
	previous_tick = monotonic_milliseconds();
	while (keep_running) {
		uint32_t current_tick = monotonic_milliseconds();
		lv_tick_inc(current_tick - previous_tick);
		previous_tick = current_tick;
		lv_timer_handler();
		nanosleep(&sleep_time, NULL);
	}

	free(draw_buffer);
	framebuffer_close(&fb);
	close(drm_fd);
	return EXIT_SUCCESS;
}
