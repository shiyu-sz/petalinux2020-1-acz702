#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "lvgl.h"

#define DISPLAY_WIDTH 1280
#define DISPLAY_HEIGHT 720
#define DISPLAY_REFRESH 60
#define CAMERA_WIDTH 800
#define CAMERA_HEIGHT 480
#define CAMERA_BYTES_PER_PIXEL 2U
#define CAMERA_BUFFER_COUNT 4U
#define CAMERA_FRAME_SIZE (CAMERA_WIDTH * CAMERA_HEIGHT * CAMERA_BYTES_PER_PIXEL)

struct framebuffer {
	int fd;
	uint8_t *memory;
	size_t memory_size;
	struct fb_fix_screeninfo fixed;
	struct fb_var_screeninfo variable;
	unsigned int bytes_per_pixel;
};

struct camera_buffer {
	void *memory;
	size_t length;
};

struct v4l2_camera {
	int fd;
	enum v4l2_buf_type type;
	int multi_planar;
	int streaming;
	uint32_t width;
	uint32_t height;
	uint32_t bytes_per_line;
	size_t image_size;
	unsigned int buffer_count;
	struct camera_buffer *buffers;
};

static struct framebuffer fb = { .fd = -1 };
static struct v4l2_camera camera = { .fd = -1 };
static int drm_fd = -1;
static uint32_t display_width = DISPLAY_WIDTH;
static uint32_t display_height = DISPLAY_HEIGHT;
static volatile sig_atomic_t keep_running = 1;
static uint8_t *camera_pixels;
static lv_img_dsc_t camera_image_descriptor;
static lv_obj_t *camera_image;
static lv_obj_t *camera_status_label;
static lv_obj_t *progress_bar;
static lv_obj_t *progress_label;
static int progress_value;
static int progress_direction = 1;

static int camera_ioctl(int fd, unsigned long request, void *argument)
{
	int result;

	do {
		result = ioctl(fd, request, argument);
	} while (result < 0 && errno == EINTR);

	return result;
}

static void camera_close(struct v4l2_camera *capture)
{
	unsigned int i;

	if (capture->fd >= 0 && capture->streaming) {
		enum v4l2_buf_type type = capture->type;

		camera_ioctl(capture->fd, VIDIOC_STREAMOFF, &type);
		capture->streaming = 0;
	}
	for (i = 0; i < capture->buffer_count; ++i) {
		if (capture->buffers[i].memory != NULL &&
		    capture->buffers[i].memory != MAP_FAILED)
			munmap(capture->buffers[i].memory, capture->buffers[i].length);
	}
	free(capture->buffers);
	capture->buffers = NULL;
	capture->buffer_count = 0;
	if (capture->fd >= 0)
		close(capture->fd);
	capture->fd = -1;
}

static int camera_queue_buffer(struct v4l2_camera *capture, unsigned int index)
{
	struct v4l2_buffer buffer;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];

	memset(&buffer, 0, sizeof(buffer));
	buffer.type = capture->type;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.index = index;
	if (capture->multi_planar) {
		memset(planes, 0, sizeof(planes));
		planes[0].length = (uint32_t)capture->buffers[index].length;
		buffer.m.planes = planes;
		buffer.length = VIDEO_MAX_PLANES;
	}
	return camera_ioctl(capture->fd, VIDIOC_QBUF, &buffer);
}

static int camera_set_format(struct v4l2_camera *capture)
{
	struct v4l2_format format;
	uint32_t pixel_format;
	uint32_t bytes_per_line;
	uint32_t width;
	uint32_t height;
	uint32_t planes = 1;
	size_t image_size;
	uint64_t required_size;
	uint64_t full_size;

	memset(&format, 0, sizeof(format));
	format.type = capture->type;
	if (capture->multi_planar) {
		format.fmt.pix_mp.width = CAMERA_WIDTH;
		format.fmt.pix_mp.height = CAMERA_HEIGHT;
		format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_RGB565;
		format.fmt.pix_mp.field = V4L2_FIELD_NONE;
		format.fmt.pix_mp.num_planes = 1;
		format.fmt.pix_mp.plane_fmt[0].bytesperline =
			CAMERA_WIDTH * CAMERA_BYTES_PER_PIXEL;
		format.fmt.pix_mp.plane_fmt[0].sizeimage = CAMERA_FRAME_SIZE;
	} else {
		format.fmt.pix.width = CAMERA_WIDTH;
		format.fmt.pix.height = CAMERA_HEIGHT;
		format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
		format.fmt.pix.field = V4L2_FIELD_NONE;
		format.fmt.pix.bytesperline = CAMERA_WIDTH * CAMERA_BYTES_PER_PIXEL;
		format.fmt.pix.sizeimage = CAMERA_FRAME_SIZE;
	}

	if (camera_ioctl(capture->fd, VIDIOC_S_FMT, &format) < 0) {
		fprintf(stderr, "VIDIOC_S_FMT on /dev/video0 failed: %s\n", strerror(errno));
		return -1;
	}

	if (capture->multi_planar) {
		width = format.fmt.pix_mp.width;
		height = format.fmt.pix_mp.height;
		pixel_format = format.fmt.pix_mp.pixelformat;
		planes = format.fmt.pix_mp.num_planes;
		bytes_per_line = format.fmt.pix_mp.plane_fmt[0].bytesperline;
		image_size = format.fmt.pix_mp.plane_fmt[0].sizeimage;
	} else {
		width = format.fmt.pix.width;
		height = format.fmt.pix.height;
		pixel_format = format.fmt.pix.pixelformat;
		bytes_per_line = format.fmt.pix.bytesperline;
		image_size = format.fmt.pix.sizeimage;
	}

	if (pixel_format != V4L2_PIX_FMT_RGB565 || width != CAMERA_WIDTH ||
	    height != CAMERA_HEIGHT || planes != 1) {
		fprintf(stderr,
			"/dev/video0 format mismatch: got %ux%u fourcc %c%c%c%c, %u plane(s); "
			"need 800x480 RGB565 with one plane\n",
			width, height, (int)(pixel_format & 0xffU),
			(int)((pixel_format >> 8) & 0xffU),
			(int)((pixel_format >> 16) & 0xffU),
			(int)((pixel_format >> 24) & 0xffU), planes);
		return -1;
	}

	if (bytes_per_line == 0)
		bytes_per_line = CAMERA_WIDTH * CAMERA_BYTES_PER_PIXEL;
	if (bytes_per_line < CAMERA_WIDTH * CAMERA_BYTES_PER_PIXEL) {
		fprintf(stderr, "/dev/video0 returned an invalid RGB565 line stride\n");
		return -1;
	}
	required_size = (uint64_t)(CAMERA_HEIGHT - 1U) * bytes_per_line +
		CAMERA_WIDTH * CAMERA_BYTES_PER_PIXEL;
	full_size = (uint64_t)bytes_per_line * CAMERA_HEIGHT;
	if (full_size > SIZE_MAX) {
		fprintf(stderr, "/dev/video0 frame size does not fit in memory\n");
		return -1;
	}
	if (image_size == 0)
		image_size = (size_t)full_size;
	if (image_size < required_size) {
		fprintf(stderr, "/dev/video0 returned invalid RGB565 stride or image size\n");
		return -1;
	}

	capture->width = width;
	capture->height = height;
	capture->bytes_per_line = bytes_per_line;
	capture->image_size = image_size;
	return 0;
}

static int camera_start(struct v4l2_camera *capture, const char *device)
{
	struct v4l2_capability capability;
	struct v4l2_requestbuffers request;
	uint32_t capabilities;
	unsigned int i;
	enum v4l2_buf_type type;

	capture->fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (capture->fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", device, strerror(errno));
		return -1;
	}

	memset(&capability, 0, sizeof(capability));
	if (camera_ioctl(capture->fd, VIDIOC_QUERYCAP, &capability) < 0) {
		fprintf(stderr, "VIDIOC_QUERYCAP on %s failed: %s\n", device,
			strerror(errno));
		goto error;
	}
	capabilities = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ?
		capability.device_caps : capability.capabilities;
	if (!(capabilities & V4L2_CAP_STREAMING)) {
		fprintf(stderr, "%s does not support streaming capture\n", device);
		goto error;
	}
	if (capabilities & V4L2_CAP_VIDEO_CAPTURE) {
		capture->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		capture->multi_planar = 0;
	} else if (capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
		capture->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		capture->multi_planar = 1;
	} else {
		fprintf(stderr, "%s is not a video capture device\n", device);
		goto error;
	}

	if (camera_set_format(capture) != 0)
		goto error;

	memset(&request, 0, sizeof(request));
	request.count = CAMERA_BUFFER_COUNT;
	request.type = capture->type;
	request.memory = V4L2_MEMORY_MMAP;
	if (camera_ioctl(capture->fd, VIDIOC_REQBUFS, &request) < 0) {
		fprintf(stderr, "VIDIOC_REQBUFS on %s failed: %s\n", device,
			strerror(errno));
		goto error;
	}
	if (request.count < 2) {
		fprintf(stderr, "%s provided fewer than two capture buffers\n", device);
		goto error;
	}
	capture->buffers = calloc(request.count, sizeof(*capture->buffers));
	if (capture->buffers == NULL) {
		perror("allocate V4L2 buffer descriptors");
		goto error;
	}
	capture->buffer_count = request.count;

	for (i = 0; i < capture->buffer_count; ++i) {
		struct v4l2_buffer buffer;
		struct v4l2_plane planes[VIDEO_MAX_PLANES];
		size_t length;
		off_t offset;

		memset(&buffer, 0, sizeof(buffer));
		buffer.type = capture->type;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = i;
		if (capture->multi_planar) {
			memset(planes, 0, sizeof(planes));
			buffer.m.planes = planes;
			buffer.length = VIDEO_MAX_PLANES;
		}
		if (camera_ioctl(capture->fd, VIDIOC_QUERYBUF, &buffer) < 0) {
			fprintf(stderr, "VIDIOC_QUERYBUF failed: %s\n", strerror(errno));
			goto error;
		}
		if (capture->multi_planar) {
			if (buffer.length < 1) {
				fprintf(stderr, "V4L2 returned no capture planes\n");
				goto error;
			}
			length = planes[0].length;
			offset = (off_t)planes[0].m.mem_offset;
		} else {
			length = buffer.length;
			offset = (off_t)buffer.m.offset;
		}
		if (length < capture->image_size) {
			fprintf(stderr, "V4L2 buffer is smaller than the negotiated image\n");
			goto error;
		}
		capture->buffers[i].memory = mmap(NULL, length, PROT_READ | PROT_WRITE,
						 MAP_SHARED, capture->fd, offset);
		if (capture->buffers[i].memory == MAP_FAILED) {
			capture->buffers[i].memory = NULL;
			fprintf(stderr, "mmap V4L2 buffer failed: %s\n", strerror(errno));
			goto error;
		}
		capture->buffers[i].length = length;
	}

	for (i = 0; i < capture->buffer_count; ++i) {
		if (camera_queue_buffer(capture, i) < 0) {
			fprintf(stderr, "VIDIOC_QBUF failed: %s\n", strerror(errno));
			goto error;
		}
	}
	type = capture->type;
	if (camera_ioctl(capture->fd, VIDIOC_STREAMON, &type) < 0) {
		fprintf(stderr, "VIDIOC_STREAMON on %s failed: %s\n", device,
			strerror(errno));
		goto error;
	}
	capture->streaming = 1;
	printf("V4L2 %s: %ux%u RGB565, stride %u, %u mmap buffers\n", device,
	       capture->width, capture->height, capture->bytes_per_line,
	       capture->buffer_count);
	return 0;

error:
	camera_close(capture);
	return -1;
}

static int camera_update_frame(struct v4l2_camera *capture, uint8_t *destination)
{
	struct v4l2_buffer buffer;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	const uint8_t *source;
	size_t bytes_used = 0;
	size_t data_offset = 0;
	size_t row_bytes = CAMERA_WIDTH * CAMERA_BYTES_PER_PIXEL;
	uint64_t required_size = (uint64_t)(CAMERA_HEIGHT - 1U) *
		capture->bytes_per_line + row_bytes;
	uint32_t frame_flags = 0;
	unsigned int frame_index;
	unsigned int y;
	int valid_frame = 1;
	int have_frame = 0;

	for (;;) {
		memset(&buffer, 0, sizeof(buffer));
		buffer.type = capture->type;
		buffer.memory = V4L2_MEMORY_MMAP;
		if (capture->multi_planar) {
			memset(planes, 0, sizeof(planes));
			buffer.m.planes = planes;
			buffer.length = VIDEO_MAX_PLANES;
		}
		if (camera_ioctl(capture->fd, VIDIOC_DQBUF, &buffer) < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				break;
			fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", strerror(errno));
			if (have_frame)
				camera_queue_buffer(capture, frame_index);
			return -1;
		}
		if (capture->multi_planar && buffer.length < 1) {
			fprintf(stderr, "V4L2 returned a frame without a capture plane\n");
			return -1;
		}
		if (buffer.index >= capture->buffer_count) {
			fprintf(stderr, "V4L2 returned an invalid buffer index\n");
			return -1;
		}
		if (have_frame && camera_queue_buffer(capture, frame_index) < 0) {
			fprintf(stderr, "discarding an old V4L2 frame failed: %s\n",
				strerror(errno));
			return -1;
		}
		frame_index = buffer.index;
		frame_flags = buffer.flags;
		if (capture->multi_planar) {
			bytes_used = planes[0].bytesused;
			data_offset = planes[0].data_offset;
		} else {
			bytes_used = buffer.bytesused;
			data_offset = 0;
		}
		have_frame = 1;
	}

	if (!have_frame)
		return 0;
	if ((frame_flags & V4L2_BUF_FLAG_ERROR) ||
	    data_offset > capture->buffers[frame_index].length ||
	    required_size > capture->buffers[frame_index].length - data_offset ||
	    bytes_used < required_size) {
		valid_frame = 0;
	} else {
		source = (const uint8_t *)capture->buffers[frame_index].memory + data_offset;
		for (y = 0; y < CAMERA_HEIGHT; ++y)
			memcpy(destination + (size_t)y * row_bytes,
			       source + (size_t)y * capture->bytes_per_line, row_bytes);
	}
	if (camera_queue_buffer(capture, frame_index) < 0) {
		fprintf(stderr, "requeue V4L2 buffer failed: %s\n", strerror(errno));
		return -1;
	}
	return valid_frame ? 1 : 0;
}

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
	lv_obj_t *camera_frame;
	lv_obj_t *title;
	lv_obj_t *button;
	lv_obj_t *button_label;
	lv_obj_t *details;
	lv_obj_t *camera_heading;

	lv_obj_set_style_bg_color(screen, lv_color_hex(0x10243A), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

	title = lv_label_create(screen);
	lv_label_set_text(title, "PetaLinux + LVGL / OV2640");
	lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

	camera_frame = lv_obj_create(screen);
	lv_obj_set_size(camera_frame, CAMERA_WIDTH + 4, CAMERA_HEIGHT + 4);
	lv_obj_align(camera_frame, LV_ALIGN_TOP_LEFT, 24, 88);
	lv_obj_clear_flag(camera_frame, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_pad_all(camera_frame, 0, LV_PART_MAIN);
	lv_obj_set_style_radius(camera_frame, 0, LV_PART_MAIN);
	lv_obj_set_style_bg_color(camera_frame, lv_color_hex(0x000000), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(camera_frame, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_border_color(camera_frame, lv_color_hex(0x3D6378), LV_PART_MAIN);
	lv_obj_set_style_border_width(camera_frame, 2, LV_PART_MAIN);

	camera_image = lv_img_create(camera_frame);
	lv_img_set_src(camera_image, &camera_image_descriptor);
	lv_obj_center(camera_image);

	camera_heading = lv_label_create(screen);
	lv_label_set_text(camera_heading, "OV2640 live capture");
	lv_obj_set_style_text_color(camera_heading, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
	lv_obj_align(camera_heading, LV_ALIGN_TOP_LEFT, 860, 110);

	camera_status_label = lv_label_create(screen);
	lv_label_set_text(camera_status_label, "Starting /dev/video0...");
	lv_obj_set_style_text_color(camera_status_label, lv_color_hex(0x80E0B0), LV_PART_MAIN);
	lv_obj_align(camera_status_label, LV_ALIGN_TOP_LEFT, 860, 148);

	button = lv_btn_create(screen);
	lv_obj_set_size(button, 270, 62);
	lv_obj_align(button, LV_ALIGN_TOP_LEFT, 860, 218);
	lv_obj_set_style_bg_color(button, lv_color_hex(0x087E8B), LV_PART_MAIN);
	button_label = lv_label_create(button);
	lv_label_set_text(button_label, "RGB565 / 800 x 480");
	lv_obj_center(button_label);

	progress_bar = lv_bar_create(screen);
	lv_obj_set_size(progress_bar, 300, 22);
	lv_obj_align(progress_bar, LV_ALIGN_TOP_LEFT, 860, 338);
	lv_bar_set_range(progress_bar, 0, 100);
	lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);

	progress_label = lv_label_create(screen);
	lv_label_set_text(progress_label, "LVGL 8.3.11  |  0%");
	lv_obj_set_style_text_color(progress_label, lv_color_hex(0xD7E8F2), LV_PART_MAIN);
	lv_obj_align(progress_label, LV_ALIGN_TOP_LEFT, 860, 378);

	details = lv_label_create(screen);
	lv_label_set_text_fmt(details,
		"HDMI: %ux%u  |  fb0: %u bpp\nV4L2 source: RGB565, no color conversion",
		display_width, display_height, fb.variable.bits_per_pixel);
	lv_obj_set_style_text_color(details, lv_color_hex(0x91AFC2), LV_PART_MAIN);
	lv_obj_align(details, LV_ALIGN_TOP_LEFT, 860, 448);

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
	uint32_t fps_start;
	unsigned int fps_frames = 0;
	int exit_status = EXIT_SUCCESS;
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
	if (camera_start(&camera, "/dev/video0") != 0) {
		framebuffer_close(&fb);
		close(drm_fd);
		return EXIT_FAILURE;
	}
	camera_pixels = calloc(1, CAMERA_FRAME_SIZE);
	if (camera_pixels == NULL) {
		perror("allocate camera image buffer");
		camera_close(&camera);
		framebuffer_close(&fb);
		close(drm_fd);
		return EXIT_FAILURE;
	}
	camera_image_descriptor.header.always_zero = 0;
	camera_image_descriptor.header.cf = LV_IMG_CF_TRUE_COLOR;
	camera_image_descriptor.header.w = CAMERA_WIDTH;
	camera_image_descriptor.header.h = CAMERA_HEIGHT;
	camera_image_descriptor.data_size = CAMERA_FRAME_SIZE;
	camera_image_descriptor.data = camera_pixels;

	printf("Framebuffer %s: backing %ux%u, active %ux%u, %u bpp, line length %u\n",
	       device, fb.variable.xres, fb.variable.yres, display_width, display_height,
	       fb.variable.bits_per_pixel, fb.fixed.line_length);

	buffer_lines = display_height < 40 ? display_height : 40;
	draw_buffer = calloc((size_t)display_width * buffer_lines, sizeof(*draw_buffer));
	if (draw_buffer == NULL) {
		perror("allocate LVGL draw buffer");
		free(camera_pixels);
		camera_pixels = NULL;
		camera_close(&camera);
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
		free(camera_pixels);
		camera_pixels = NULL;
		camera_close(&camera);
		framebuffer_close(&fb);
		close(drm_fd);
		return EXIT_FAILURE;
	}

	create_demo_ui();
	signal(SIGINT, stop_handler);
	signal(SIGTERM, stop_handler);
	previous_tick = monotonic_milliseconds();
	fps_start = previous_tick;
	while (keep_running) {
		int frame_result = camera_update_frame(&camera, camera_pixels);
		uint32_t current_tick = monotonic_milliseconds();

		if (frame_result < 0) {
			exit_status = EXIT_FAILURE;
			break;
		}
		if (frame_result > 0) {
			lv_obj_invalidate(camera_image);
			++fps_frames;
		}
		if (current_tick - fps_start >= 1000U) {
			lv_label_set_text_fmt(camera_status_label,
					      "800x480 RGB565  |  %u fps", fps_frames);
			fps_frames = 0;
			fps_start = current_tick;
		}
		lv_tick_inc(current_tick - previous_tick);
		previous_tick = current_tick;
		lv_timer_handler();
		nanosleep(&sleep_time, NULL);
	}

	free(draw_buffer);
	free(camera_pixels);
	camera_pixels = NULL;
	camera_close(&camera);
	framebuffer_close(&fb);
	close(drm_fd);
	return exit_status;
}
