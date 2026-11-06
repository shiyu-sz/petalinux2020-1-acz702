#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "lvgl.h"

#if LV_COLOR_DEPTH != 16 || LV_COLOR_16_SWAP != 0
#error "This example requires LVGL RGB565 without byte swapping"
#endif

#define DISPLAY_WIDTH 1280
#define DISPLAY_HEIGHT 720
#define DISPLAY_REFRESH 60
#define VIDEO_WIDTH 720
#define VIDEO_HEIGHT 480
#define VIDEO_BYTES (VIDEO_WIDTH * VIDEO_HEIGHT * 2U)

struct framebuffer {
	int fd;
	uint8_t *memory;
	size_t size;
	struct fb_fix_screeninfo fixed;
	struct fb_var_screeninfo variable;
	unsigned int bytes_per_pixel;
	int native_rgb565;
};

struct video {
	AVFormatContext *format;
	AVCodecContext *codec;
	AVPacket *packet;
	AVFrame *frame;
	struct SwsContext *scaler;
	int stream_index;
	int input_eof;
	int decoder_flushed;
	int output_width;
	int output_height;
	int output_x;
	int output_y;
};

static struct framebuffer fb = { .fd = -1 };
static int drm_fd = -1;
static uint32_t display_width;
static uint32_t display_height;
static volatile sig_atomic_t keep_running = 1;
static uint8_t *video_pixels;
static lv_img_dsc_t video_image_descriptor;
static lv_obj_t *video_image;

static void stop_handler(int signal_number)
{
	(void)signal_number;
	keep_running = 0;
}

static void print_av_error(const char *operation, int error)
{
	char message[AV_ERROR_MAX_STRING_SIZE];

	av_strerror(error, message, sizeof(message));
	fprintf(stderr, "%s: %s\n", operation, message);
}

static int framebuffer_open(const char *device)
{
	uint64_t last_row_end;

	fb.fd = open(device, O_RDWR | O_CLOEXEC);
	if (fb.fd < 0) {
		fprintf(stderr, "open %s: %s\n", device, strerror(errno));
		return -1;
	}
	if (ioctl(fb.fd, FBIOGET_VSCREENINFO, &fb.variable) < 0 ||
	    ioctl(fb.fd, FBIOGET_FSCREENINFO, &fb.fixed) < 0) {
		fprintf(stderr, "read framebuffer info: %s\n", strerror(errno));
		return -1;
	}
	fb.bytes_per_pixel = (fb.variable.bits_per_pixel + 7U) / 8U;
	last_row_end = (uint64_t)(fb.variable.yoffset + fb.variable.yres - 1U) *
		fb.fixed.line_length +
		(uint64_t)(fb.variable.xoffset + fb.variable.xres) * fb.bytes_per_pixel;
	if (fb.variable.xres < VIDEO_WIDTH || fb.variable.yres < VIDEO_HEIGHT ||
	    fb.variable.xres > INT16_MAX || fb.variable.yres > INT16_MAX ||
	    (fb.variable.bits_per_pixel != 16 &&
	     fb.variable.bits_per_pixel != 24 &&
	     fb.variable.bits_per_pixel != 32) ||
	    fb.fixed.visual != FB_VISUAL_TRUECOLOR ||
	    fb.variable.xoffset + fb.variable.xres > fb.variable.xres_virtual ||
	    fb.variable.yoffset + fb.variable.yres > fb.variable.yres_virtual ||
	    fb.fixed.line_length <
		fb.variable.xres_virtual * fb.bytes_per_pixel ||
	    last_row_end > fb.fixed.smem_len) {
		fprintf(stderr, "unsupported framebuffer geometry or pixel format\n");
		return -1;
	}
	fb.size = fb.fixed.smem_len;
	fb.memory = mmap(NULL, fb.size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fb.fd, 0);
	if (fb.memory == MAP_FAILED) {
		fb.memory = NULL;
		fprintf(stderr, "mmap %s: %s\n", device, strerror(errno));
		return -1;
	}
	fb.native_rgb565 = fb.variable.bits_per_pixel == 16 &&
		fb.variable.red.offset == 11 && fb.variable.red.length == 5 &&
		fb.variable.green.offset == 5 && fb.variable.green.length == 6 &&
		fb.variable.blue.offset == 0 && fb.variable.blue.length == 5;
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

	fprintf(stderr, "DRM connector %u mode: %s (%ux%u@%u)\n", connector_id,
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

static void framebuffer_close(void)
{
	if (fb.memory != NULL)
		munmap(fb.memory, fb.size);
	if (fb.fd >= 0)
		close(fb.fd);
	fb.memory = NULL;
	fb.fd = -1;
}

static uint32_t color_field(uint8_t component, struct fb_bitfield field)
{
	uint64_t maximum;

	if (field.length == 0 || field.length > 8 ||
	    field.offset + field.length > fb.variable.bits_per_pixel)
		return 0;
	maximum = (1ULL << field.length) - 1ULL;
	return (uint32_t)((((uint64_t)component * maximum + 127U) / 255U)
			  << field.offset);
}

static void framebuffer_flush(lv_disp_drv_t *display, const lv_area_t *area,
			      lv_color_t *colors)
{
	int32_t y;
	int32_t width = area->x2 - area->x1 + 1;

	for (y = area->y1; y <= area->y2; ++y) {
		int32_t x;
		uint8_t *destination;
		lv_color_t *source;

		if (y < 0 || y >= (int32_t)display_height)
			continue;
		if (area->x1 < 0 || area->x2 >= (int32_t)display_width)
			continue;
		destination = fb.memory +
			(size_t)(y + fb.variable.yoffset) * fb.fixed.line_length +
			(size_t)(area->x1 + fb.variable.xoffset) * fb.bytes_per_pixel;
		source = colors + (y - area->y1) * width;
		if (fb.native_rgb565) {
			memcpy(destination, source, (size_t)width * 2U);
			continue;
		}
		for (x = 0; x < width; ++x) {
			lv_color32_t rgb;
			uint32_t pixel;
			unsigned int byte;

			rgb.full = lv_color_to32(source[x]);
			pixel = color_field(rgb.ch.red, fb.variable.red) |
				color_field(rgb.ch.green, fb.variable.green) |
				color_field(rgb.ch.blue, fb.variable.blue) |
				color_field(255, fb.variable.transp);
			for (byte = 0; byte < fb.bytes_per_pixel; ++byte)
				destination[x * fb.bytes_per_pixel + byte] =
					(uint8_t)(pixel >> (byte * 8U));
		}
	}
	lv_disp_flush_ready(display);
}

static int64_t monotonic_milliseconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void service_lvgl(int64_t *previous_ms)
{
	int64_t now_ms = monotonic_milliseconds();

	lv_tick_inc((uint32_t)(now_ms - *previous_ms));
	*previous_ms = now_ms;
	lv_timer_handler();
}

static int video_open(struct video *video, const char *path)
{
	const AVCodec *decoder;
	int result;

	result = avformat_open_input(&video->format, path, NULL, NULL);
	if (result < 0)
		goto error;
	result = avformat_find_stream_info(video->format, NULL);
	if (result < 0)
		goto error;
	video->stream_index = av_find_best_stream(video->format, AVMEDIA_TYPE_VIDEO,
						-1, -1, NULL, 0);
	if (video->stream_index < 0) {
		result = video->stream_index;
		goto error;
	}
	decoder = avcodec_find_decoder(video->format->streams[video->stream_index]
					     ->codecpar->codec_id);
	if (decoder == NULL) {
		fprintf(stderr, "no decoder for the MP4 video stream\n");
		return -1;
	}
	video->codec = avcodec_alloc_context3(decoder);
	video->packet = av_packet_alloc();
	video->frame = av_frame_alloc();
	if (video->codec == NULL || video->packet == NULL || video->frame == NULL) {
		fprintf(stderr, "allocate FFmpeg decoder state failed\n");
		return -1;
	}
	result = avcodec_parameters_to_context(video->codec,
		video->format->streams[video->stream_index]->codecpar);
	if (result < 0)
		goto error;
	/* FFmpeg 4.2 defaults to one thread; H.264 can decode frames in parallel. */
	video->codec->thread_count = 2;
	result = avcodec_open2(video->codec, decoder, NULL);
	if (result < 0)
		goto error;
	fprintf(stderr, "decoder threads: %d (%s)\n", video->codec->thread_count,
		video->codec->active_thread_type & FF_THREAD_FRAME ? "frame" :
		video->codec->active_thread_type & FF_THREAD_SLICE ? "slice" : "single");
	return 0;

error:
	print_av_error("open video", result);
	return -1;
}

static void video_close(struct video *video)
{
	sws_freeContext(video->scaler);
	av_frame_free(&video->frame);
	av_packet_free(&video->packet);
	avcodec_free_context(&video->codec);
	avformat_close_input(&video->format);
}

/* Returns one decoded frame, 0 at EOF, or -1 on error. */
static int video_next_frame(struct video *video)
{
	int result;

	for (;;) {
		result = avcodec_receive_frame(video->codec, video->frame);
		if (result == 0)
			return 1;
		if (result == AVERROR_EOF)
			return 0;
		if (result != AVERROR(EAGAIN)) {
			print_av_error("decode frame", result);
			return -1;
		}
		if (video->input_eof) {
			if (video->decoder_flushed) {
				fprintf(stderr, "decoder requested input after flush\n");
				return -1;
			}
			video->decoder_flushed = 1;
			result = avcodec_send_packet(video->codec, NULL);
		} else {
			result = av_read_frame(video->format, video->packet);
			if (result == AVERROR_EOF) {
				video->input_eof = 1;
				continue;
			}
			if (result < 0) {
				print_av_error("read video", result);
				return -1;
			}
			if (video->packet->stream_index != video->stream_index) {
				av_packet_unref(video->packet);
				continue;
			}
			result = avcodec_send_packet(video->codec, video->packet);
			av_packet_unref(video->packet);
		}
		if (result < 0) {
			print_av_error("send video packet", result);
			return -1;
		}
	}
}

static int video_render_frame(struct video *video)
{
	AVRational aspect = video->frame->sample_aspect_ratio;
	double ratio;
	uint8_t *destination[4] = { NULL };
	int linesize[4] = { 0 };
	int height;

	if (video->frame->width <= 0 || video->frame->height <= 0)
		return -1;
	if (aspect.num <= 0 || aspect.den <= 0)
		aspect = video->format->streams[video->stream_index]
			->sample_aspect_ratio;
	if (aspect.num <= 0 || aspect.den <= 0)
		aspect = (AVRational){ 1, 1 };
	ratio = (double)video->frame->width * aspect.num /
		((double)video->frame->height * aspect.den);
	if (!isfinite(ratio) || ratio <= 0.0)
		return -1;
	if (ratio > (double)VIDEO_WIDTH / VIDEO_HEIGHT) {
		video->output_width = VIDEO_WIDTH;
		video->output_height = (int)lround(VIDEO_WIDTH / ratio);
	} else {
		video->output_height = VIDEO_HEIGHT;
		video->output_width = (int)lround(VIDEO_HEIGHT * ratio);
	}
	if (video->output_width < 1)
		video->output_width = 1;
	if (video->output_height < 1)
		video->output_height = 1;
	video->output_x = (VIDEO_WIDTH - video->output_width) / 2;
	video->output_y = (VIDEO_HEIGHT - video->output_height) / 2;
	video->scaler = sws_getCachedContext(video->scaler,
		video->frame->width, video->frame->height, video->frame->format,
		video->output_width, video->output_height, AV_PIX_FMT_RGB565LE,
		SWS_BILINEAR, NULL, NULL, NULL);
	if (video->scaler == NULL) {
		fprintf(stderr, "create RGB565 scaler failed\n");
		return -1;
	}
	memset(video_pixels, 0, VIDEO_BYTES);
	destination[0] = video_pixels +
		((size_t)video->output_y * VIDEO_WIDTH + video->output_x) * 2U;
	linesize[0] = VIDEO_WIDTH * 2;
	height = sws_scale(video->scaler, (const uint8_t *const *)video->frame->data,
		video->frame->linesize, 0, video->frame->height,
		destination, linesize);
	if (height != video->output_height) {
		fprintf(stderr, "scale video frame failed\n");
		return -1;
	}
	lv_img_cache_invalidate_src(&video_image_descriptor);
	lv_obj_invalidate(video_image);
	lv_refr_now(NULL);
	return 0;
}

static void create_ui(void)
{
	lv_obj_t *window;

	lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x1b1b1b), 0);
	window = lv_obj_create(lv_scr_act());
	lv_obj_set_size(window, VIDEO_WIDTH, VIDEO_HEIGHT);
	lv_obj_center(window);
	lv_obj_set_style_pad_all(window, 0, 0);
	lv_obj_set_style_border_width(window, 0, 0);
	lv_obj_set_style_radius(window, 0, 0);
	lv_obj_set_style_bg_color(window, lv_color_black(), 0);
	lv_obj_clear_flag(window, LV_OBJ_FLAG_SCROLLABLE);
	video_image = lv_img_create(window);
	lv_img_set_src(video_image, &video_image_descriptor);
	lv_obj_center(video_image);
}

int main(int argc, char **argv)
{
	struct video video = { .stream_index = -1 };
	lv_color_t *draw_buffer = NULL;
	lv_disp_draw_buf_t draw_buffer_descriptor;
	lv_disp_drv_t display_driver;
	int64_t previous_ms;
	int64_t playback_start_ms = 0;
	int64_t first_pts_ms = 0;
	int64_t fallback_frame_ms = 40;
	int64_t frame_number = 0;
	int64_t displayed_frames = 0;
	int64_t run_start_ms = 0;
	int status = EXIT_FAILURE;
	int result;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s /path/to/video.mp4\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (access(argv[1], R_OK) != 0) {
		fprintf(stderr, "cannot read video %s: %s\n", argv[1],
			strerror(errno));
		return EXIT_FAILURE;
	}
	if (framebuffer_open("/dev/fb0") != 0)
		goto cleanup;
	fprintf(stderr, "framebuffer: %ux%u, %u bpp\n", fb.variable.xres,
		fb.variable.yres, fb.variable.bits_per_pixel);
	display_width = fb.variable.xres;
	display_height = fb.variable.yres;
	if (display_width >= DISPLAY_WIDTH && display_height >= DISPLAY_HEIGHT) {
		if (framebuffer_set_720p() != 0)
			goto cleanup;
		display_width = DISPLAY_WIDTH;
		display_height = DISPLAY_HEIGHT;
	}
	if (video_open(&video, argv[1]) != 0)
		goto cleanup;
	fprintf(stderr, "video: %s, decoder %s, %dx%d\n", argv[1],
		avcodec_get_name(video.codec->codec_id),
		video.codec->width, video.codec->height);
	if (video.format->streams[video.stream_index]->avg_frame_rate.num > 0) {
		fallback_frame_ms = av_rescale_q(1,
			av_inv_q(video.format->streams[video.stream_index]->avg_frame_rate),
			(AVRational){ 1, 1000 });
		if (fallback_frame_ms < 1)
			fallback_frame_ms = 1;
	}
	video_pixels = calloc(1, VIDEO_BYTES);
	draw_buffer = calloc((size_t)display_width * 40U, sizeof(*draw_buffer));
	if (video_pixels == NULL || draw_buffer == NULL) {
		fprintf(stderr, "allocate LVGL buffers failed\n");
		goto cleanup;
	}
	video_image_descriptor.header.cf = LV_IMG_CF_TRUE_COLOR;
	video_image_descriptor.header.w = VIDEO_WIDTH;
	video_image_descriptor.header.h = VIDEO_HEIGHT;
	video_image_descriptor.data_size = VIDEO_BYTES;
	video_image_descriptor.data = video_pixels;

	lv_init();
	lv_disp_draw_buf_init(&draw_buffer_descriptor, draw_buffer, NULL,
		display_width * 40U);
	lv_disp_drv_init(&display_driver);
	display_driver.hor_res = display_width;
	display_driver.ver_res = display_height;
	display_driver.draw_buf = &draw_buffer_descriptor;
	display_driver.flush_cb = framebuffer_flush;
	if (lv_disp_drv_register(&display_driver) == NULL) {
		fprintf(stderr, "register LVGL display failed\n");
		goto cleanup;
	}
	create_ui();
	lv_refr_now(NULL);
	signal(SIGINT, stop_handler);
	signal(SIGTERM, stop_handler);
	previous_ms = monotonic_milliseconds();
	run_start_ms = previous_ms;
	while (keep_running) {
		int64_t pts_ms;
		int64_t target_ms;
		int64_t now_ms;

		result = video_next_frame(&video);
		if (result < 0)
			goto cleanup;
		if (result == 0)
			break;
		if (video.frame->best_effort_timestamp != AV_NOPTS_VALUE) {
			pts_ms = av_rescale_q(video.frame->best_effort_timestamp,
				video.format->streams[video.stream_index]->time_base,
				(AVRational){ 1, 1000 });
		} else {
			pts_ms = frame_number * fallback_frame_ms;
		}
		if (frame_number == 0) {
			first_pts_ms = pts_ms;
			playback_start_ms = monotonic_milliseconds();
		}
		++frame_number;
		target_ms = playback_start_ms + pts_ms - first_pts_ms;
		while (keep_running && (now_ms = monotonic_milliseconds()) < target_ms) {
			struct timespec sleep_time = { .tv_sec = 0, .tv_nsec = 5000000L };

			service_lvgl(&previous_ms);
			if (target_ms - now_ms < 5)
				sleep_time.tv_nsec = (target_ms - now_ms) * 1000000L;
			nanosleep(&sleep_time, NULL);
		}
		if (!keep_running) {
			av_frame_unref(video.frame);
			break;
		}
		if (monotonic_milliseconds() - target_ms > 150)
			playback_start_ms = monotonic_milliseconds() -
				(pts_ms - first_pts_ms);
		if (video_render_frame(&video) != 0)
			goto cleanup;
		++displayed_frames;
		av_frame_unref(video.frame);
		service_lvgl(&previous_ms);
	}
	service_lvgl(&previous_ms);
	if (frame_number == 0) {
		fprintf(stderr, "video contains no decodable frames\n");
	} else {
		fprintf(stderr, "playback finished: %lld frames displayed in %lld ms\n",
			(long long)displayed_frames,
			(long long)(monotonic_milliseconds() - run_start_ms));
		status = EXIT_SUCCESS;
	}

cleanup:
	video_close(&video);
	framebuffer_close();
	if (drm_fd >= 0)
		close(drm_fd);
	free(draw_buffer);
	free(video_pixels);
	return status;
}
