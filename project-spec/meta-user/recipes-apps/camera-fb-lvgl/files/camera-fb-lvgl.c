#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <poll.h>
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
#define CAMERA_WIDTH 1280
#define CAMERA_HEIGHT 720
#define CAMERA_BYTES_PER_PIXEL 2U
#define CAMERA_BUFFER_COUNT 4U
#define CAMERA_FRAME_SIZE (CAMERA_WIDTH * CAMERA_HEIGHT * CAMERA_BYTES_PER_PIXEL)
#define DISPLAY_BUFFER_COUNT 2U

struct display_buffer {
	uint32_t handle;
	uint32_t framebuffer_id;
	uint32_t pitch;
	uint64_t size;
	uint8_t *memory;
};

struct framebuffer {
	int fd;
	uint8_t *memory;
	size_t memory_size;
	struct fb_fix_screeninfo fixed;
	struct fb_var_screeninfo variable;
	unsigned int bytes_per_pixel;
	struct display_buffer buffers[DISPLAY_BUFFER_COUNT];
	unsigned int front_buffer;
	unsigned int draw_buffer;
	uint32_t connector_id;
	uint32_t crtc_id;
	uint32_t original_framebuffer_id;
	uint32_t original_x;
	uint32_t original_y;
	int original_mode_valid;
	int buffers_active;
	drmModeModeInfo mode;
	drmModeModeInfo original_mode;
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
static lv_obj_t *status_panel;
static lv_obj_t *fps_label;

static int framebuffer_copy_rgb565(struct framebuffer *destination,
				   const uint8_t *source,
				   uint32_t source_stride,
				   uint32_t width,
				   uint32_t height);
static int framebuffer_create_display_buffers(struct framebuffer *framebuffer);
static int framebuffer_present(struct framebuffer *framebuffer);

static void create_status_panel(void);

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
			"need %ux%u RGB565 with one plane\n",
			width, height, (int)(pixel_format & 0xffU),
			(int)((pixel_format >> 8) & 0xffU),
			(int)((pixel_format >> 16) & 0xffU),
			(int)((pixel_format >> 24) & 0xffU), planes,
			CAMERA_WIDTH, CAMERA_HEIGHT);
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

static int camera_update_frame(struct v4l2_camera *capture,
			       struct framebuffer *destination)
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
		if (framebuffer_copy_rgb565(destination, source,
					    capture->bytes_per_line,
					    capture->width, capture->height) != 0)
			valid_frame = 0;
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

	/* The DRM dumb buffers become the draw/scanout surfaces after modeset. */
	framebuffer->memory = NULL;
	framebuffer->memory_size = 0;
	return 0;
}

static int framebuffer_set_720p(struct framebuffer *framebuffer)
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

	framebuffer->connector_id = connector_id;
	framebuffer->crtc_id = crtc_id;
	framebuffer->original_framebuffer_id = crtc->buffer_id;
	framebuffer->original_x = crtc->x;
	framebuffer->original_y = crtc->y;
	framebuffer->original_mode_valid = crtc->mode_valid;
	if (crtc->mode_valid)
		framebuffer->original_mode = crtc->mode;
	framebuffer->mode = mode;

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

static void framebuffer_destroy_display_buffers(struct framebuffer *framebuffer)
{
	unsigned int i;

	for (i = 0; i < DISPLAY_BUFFER_COUNT; ++i) {
		struct display_buffer *buffer = &framebuffer->buffers[i];

		if (buffer->memory != NULL && buffer->memory != MAP_FAILED)
			munmap(buffer->memory, (size_t)buffer->size);
		if (buffer->framebuffer_id != 0 && drm_fd >= 0)
			drmModeRmFB(drm_fd, buffer->framebuffer_id);
		if (buffer->handle != 0 && drm_fd >= 0) {
			struct drm_mode_destroy_dumb destroy;

			memset(&destroy, 0, sizeof(destroy));
			destroy.handle = buffer->handle;
			if (ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0)
				fprintf(stderr, "destroy DRM buffer failed: %s\n",
					strerror(errno));
		}
		memset(buffer, 0, sizeof(*buffer));
	}
}

static int framebuffer_create_display_buffers(struct framebuffer *framebuffer)
{
	unsigned int i;

	if (drm_fd < 0 || framebuffer->crtc_id == 0 ||
	    framebuffer->connector_id == 0) {
		fprintf(stderr, "DRM display is not initialized\n");
		return -1;
	}
	if (framebuffer->variable.bits_per_pixel != 16) {
		fprintf(stderr, "double buffering requires a 16-bit RGB565 fb0\n");
		return -1;
	}

	for (i = 0; i < DISPLAY_BUFFER_COUNT; ++i) {
		struct display_buffer *buffer = &framebuffer->buffers[i];
		struct drm_mode_create_dumb create;
		struct drm_mode_map_dumb map;

		memset(&create, 0, sizeof(create));
		create.width = display_width;
		create.height = display_height;
		create.bpp = 16;
		if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
			fprintf(stderr, "create DRM display buffer failed: %s\n",
				strerror(errno));
			goto error;
		}
		buffer->handle = create.handle;
		buffer->pitch = create.pitch;
		buffer->size = create.size;
		if (buffer->pitch < display_width * CAMERA_BYTES_PER_PIXEL ||
		    buffer->size < (uint64_t)buffer->pitch * display_height ||
		    buffer->size > SIZE_MAX) {
			fprintf(stderr, "DRM returned an invalid display buffer layout\n");
			goto error;
		}

		if (drmModeAddFB(drm_fd, display_width, display_height, 16, 16,
				 buffer->pitch, buffer->handle,
				 &buffer->framebuffer_id) != 0) {
			fprintf(stderr, "register DRM framebuffer failed: %s\n",
				strerror(errno));
			goto error;
		}

		memset(&map, 0, sizeof(map));
		map.handle = buffer->handle;
		if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
			fprintf(stderr, "map DRM display buffer failed: %s\n",
				strerror(errno));
			goto error;
		}
		buffer->memory = mmap(NULL, (size_t)buffer->size,
				      PROT_READ | PROT_WRITE, MAP_SHARED,
				      drm_fd, (off_t)map.offset);
		if (buffer->memory == MAP_FAILED) {
			buffer->memory = NULL;
			fprintf(stderr, "mmap DRM display buffer failed: %s\n",
				strerror(errno));
			goto error;
		}
		memset(buffer->memory, 0, (size_t)buffer->size);
	}

	if (drmModeSetCrtc(drm_fd, framebuffer->crtc_id,
			   framebuffer->buffers[0].framebuffer_id, 0, 0,
			   &framebuffer->connector_id, 1,
			   &framebuffer->mode) != 0) {
		fprintf(stderr, "activate first DRM buffer failed: %s\n",
			strerror(errno));
		goto error;
	}

	framebuffer->buffers_active = 1;
	framebuffer->front_buffer = 0;
	framebuffer->draw_buffer = 1;
	framebuffer->bytes_per_pixel = 2;
	framebuffer->fixed.line_length = framebuffer->buffers[1].pitch;
	framebuffer->variable.xoffset = 0;
	framebuffer->variable.yoffset = 0;
	framebuffer->variable.red.offset = 11;
	framebuffer->variable.red.length = 5;
	framebuffer->variable.green.offset = 5;
	framebuffer->variable.green.length = 6;
	framebuffer->variable.blue.offset = 0;
	framebuffer->variable.blue.length = 5;
	framebuffer->variable.transp.offset = 0;
	framebuffer->variable.transp.length = 0;
	framebuffer->memory = framebuffer->buffers[framebuffer->draw_buffer].memory;
	framebuffer->memory_size =
		(size_t)framebuffer->buffers[framebuffer->draw_buffer].size;
	printf("DRM display buffers: %ux%u RGB565, pitch %u, page flip enabled\n",
	       display_width, display_height,
	       framebuffer->buffers[framebuffer->draw_buffer].pitch);
	return 0;

error:
	framebuffer_destroy_display_buffers(framebuffer);
	return -1;
}

static void framebuffer_page_flip_handler(int fd, unsigned int sequence,
					  unsigned int tv_sec,
					  unsigned int tv_usec,
					  void *user_data)
{
	int *completed = user_data;

	(void)fd;
	(void)sequence;
	(void)tv_sec;
	(void)tv_usec;
	*completed = 1;
}

static int framebuffer_present(struct framebuffer *framebuffer)
{
	drmEventContext event_context;
	struct pollfd drm_poll;
	int completed = 0;
	int result;

	memset(&event_context, 0, sizeof(event_context));
	event_context.version = DRM_EVENT_CONTEXT_VERSION;
	event_context.page_flip_handler = framebuffer_page_flip_handler;
	if (drmModePageFlip(drm_fd, framebuffer->crtc_id,
			    framebuffer->buffers[framebuffer->draw_buffer].framebuffer_id,
			    DRM_MODE_PAGE_FLIP_EVENT, &completed) != 0) {
		fprintf(stderr, "queue DRM page flip failed: %s\n", strerror(errno));
		return -1;
	}

	drm_poll.fd = drm_fd;
	drm_poll.events = POLLIN;
	drm_poll.revents = 0;
	while (!completed) {
		result = poll(&drm_poll, 1, 1000);
		if (result < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "wait for DRM page flip failed: %s\n",
				strerror(errno));
			return -1;
		}
		if (result == 0) {
			fprintf(stderr, "timed out waiting for DRM page flip\n");
			return -1;
		}
		if (drm_poll.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "DRM page flip event reported poll error\n");
			return -1;
		}
		if (drm_poll.revents & POLLIN &&
		    drmHandleEvent(drm_fd, &event_context) != 0) {
			fprintf(stderr, "handle DRM page flip event failed: %s\n",
				strerror(errno));
			return -1;
		}
	}

	framebuffer->front_buffer = framebuffer->draw_buffer;
	framebuffer->draw_buffer = 1U - framebuffer->front_buffer;
	framebuffer->memory = framebuffer->buffers[framebuffer->draw_buffer].memory;
	framebuffer->memory_size =
		(size_t)framebuffer->buffers[framebuffer->draw_buffer].size;
	framebuffer->fixed.line_length =
		framebuffer->buffers[framebuffer->draw_buffer].pitch;
	return 0;
}

static void framebuffer_close(struct framebuffer *framebuffer)
{
	if (framebuffer->buffers_active && drm_fd >= 0 &&
	    framebuffer->original_framebuffer_id != 0) {
		const drmModeModeInfoPtr restore_mode = framebuffer->original_mode_valid ?
			&framebuffer->original_mode : &framebuffer->mode;

		if (drmModeSetCrtc(drm_fd, framebuffer->crtc_id,
				   framebuffer->original_framebuffer_id,
				   framebuffer->original_x, framebuffer->original_y,
				   &framebuffer->connector_id, 1, restore_mode) != 0)
			fprintf(stderr, "restore previous DRM framebuffer failed: %s\n",
				strerror(errno));
		framebuffer->buffers_active = 0;
	}
	framebuffer_destroy_display_buffers(framebuffer);
	if (framebuffer->fd >= 0)
		close(framebuffer->fd);
	framebuffer->memory = NULL;
	framebuffer->memory_size = 0;
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

static uint32_t framebuffer_pixel_rgb565(uint16_t source)
{
	uint8_t red5 = (uint8_t)((source >> 11) & 0x1fU);
	uint8_t green6 = (uint8_t)((source >> 5) & 0x3fU);
	uint8_t blue5 = (uint8_t)(source & 0x1fU);
	uint32_t pixel;

	pixel = component_to_field((uint8_t)((red5 << 3) | (red5 >> 2)),
				    fb.variable.red) |
		component_to_field((uint8_t)((green6 << 2) | (green6 >> 4)),
				    fb.variable.green) |
		component_to_field((uint8_t)((blue5 << 3) | (blue5 >> 2)),
				    fb.variable.blue);
	if (fb.variable.transp.length != 0)
		pixel |= component_to_field(255, fb.variable.transp);
	return pixel;
}

static int framebuffer_copy_rgb565(struct framebuffer *destination,
				   const uint8_t *source,
				   uint32_t source_stride,
				   uint32_t width,
				   uint32_t height)
{
	uint32_t x_offset;
	uint32_t y_offset;
	uint32_t y;
	uint32_t row_bytes = width * CAMERA_BYTES_PER_PIXEL;
	int direct_rgb565;

	if (width > display_width || height > display_height ||
	    source_stride < row_bytes || destination->bytes_per_pixel == 0) {
		fprintf(stderr, "camera frame does not fit the active framebuffer\n");
		return -1;
	}
	x_offset = (display_width - width) / 2U;
	y_offset = (display_height - height) / 2U;
	direct_rgb565 = destination->variable.bits_per_pixel == 16 &&
		destination->variable.red.offset == 11 &&
		destination->variable.red.length == 5 &&
		destination->variable.green.offset == 5 &&
		destination->variable.green.length == 6 &&
		destination->variable.blue.offset == 0 &&
		destination->variable.blue.length == 5 &&
		destination->variable.transp.length == 0;

	for (y = 0; y < height; ++y) {
		size_t destination_offset =
			(size_t)(y + y_offset + destination->variable.yoffset) *
				destination->fixed.line_length +
			(size_t)(x_offset + destination->variable.xoffset) *
				destination->bytes_per_pixel;
		uint8_t *row_destination;
		const uint8_t *row_source = source + (size_t)y * source_stride;
		uint32_t x;

		if (destination_offset > destination->memory_size ||
		    (size_t)width * destination->bytes_per_pixel >
			destination->memory_size - destination_offset) {
			fprintf(stderr, "camera frame is outside the mapped framebuffer\n");
			return -1;
		}
		row_destination = destination->memory + destination_offset;
		if (direct_rgb565) {
			memcpy(row_destination, row_source, row_bytes);
			continue;
		}
		for (x = 0; x < width; ++x) {
			uint16_t source_pixel;
			uint32_t pixel;
			unsigned int byte;
			memcpy(&source_pixel, row_source + (size_t)x * 2U,
			       sizeof(source_pixel));
			pixel = framebuffer_pixel_rgb565(source_pixel);
			for (byte = 0; byte < destination->bytes_per_pixel; ++byte)
				row_destination[(size_t)x * destination->bytes_per_pixel + byte] =
					(uint8_t)(pixel >> (byte * 8U));
		}
	}
	return 0;
}

static uint32_t framebuffer_pixel_lvgl(lv_color_t color)
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
			pixel = framebuffer_pixel_lvgl(colors[source_index]);
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

static void create_status_panel(void)
{
	lv_obj_t *screen = lv_scr_act();
	lv_obj_t *title;

	lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
	status_panel = lv_obj_create(screen);
	lv_obj_set_size(status_panel, 360, 106);
	lv_obj_align(status_panel, LV_ALIGN_TOP_LEFT, 20, 20);
	lv_obj_clear_flag(status_panel, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_pad_all(status_panel, 12, LV_PART_MAIN);
	lv_obj_set_style_radius(status_panel, 8, LV_PART_MAIN);
	lv_obj_set_style_bg_color(status_panel, lv_color_hex(0x10243A), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(status_panel, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_border_color(status_panel, lv_color_hex(0x72A8C4), LV_PART_MAIN);
	lv_obj_set_style_border_width(status_panel, 2, LV_PART_MAIN);

	title = lv_label_create(status_panel);
	lv_label_set_text(title, "Camera -> HDMI");
	lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

	fps_label = lv_label_create(status_panel);
	lv_label_set_text(fps_label, "1280x720 RGB565  |  FPS: waiting...");
	lv_obj_set_style_text_color(fps_label, lv_color_hex(0xD7E8F2), LV_PART_MAIN);
	lv_obj_align(fps_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static uint32_t monotonic_milliseconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((uint64_t)now.tv_sec * 1000ULL +
			  (uint64_t)now.tv_nsec / 1000000ULL);
}

static void framebuffer_clear(struct framebuffer *destination)
{
	uint32_t y;
	size_t row_size = (size_t)display_width * destination->bytes_per_pixel;
	uint32_t x_offset = destination->variable.xoffset;
	uint32_t y_offset = destination->variable.yoffset;

	for (y = 0; y < display_height; ++y) {
		size_t offset = (size_t)(y + y_offset) * destination->fixed.line_length +
			(size_t)x_offset * destination->bytes_per_pixel;
		if (offset <= destination->memory_size &&
		    row_size <= destination->memory_size - offset)
			memset(destination->memory + offset, 0, row_size);
	}
}

int main(int argc, char **argv)
{
	const char *device = "/dev/fb0";
	struct pollfd camera_poll;
	lv_color_t *draw_buffer = NULL;
	lv_disp_draw_buf_t draw_buffer_descriptor;
	lv_disp_drv_t display_driver;
	uint32_t buffer_lines = display_height < 40U ? display_height : 40U;
	uint32_t previous_tick;
	uint32_t fps_start;
	unsigned int fps_frames = 0;
	int exit_status = EXIT_SUCCESS;

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
	if (framebuffer_set_720p(&fb) != 0) {
		framebuffer_close(&fb);
		return EXIT_FAILURE;
	}
	if (framebuffer_create_display_buffers(&fb) != 0) {
		framebuffer_close(&fb);
		close(drm_fd);
		drm_fd = -1;
		return EXIT_FAILURE;
	}
	if (camera_start(&camera, "/dev/video0") != 0) {
		framebuffer_close(&fb);
		close(drm_fd);
		drm_fd = -1;
		return EXIT_FAILURE;
	}
	printf("Framebuffer metadata from %s: backing %ux%u, active %ux%u, %u bpp\n",
	       device, fb.variable.xres, fb.variable.yres, display_width, display_height,
	       fb.variable.bits_per_pixel);
	framebuffer_clear(&fb);
	printf("Direct RGB565 camera output %ux%u to HDMI\n",
	       camera.width, camera.height);

	draw_buffer = calloc((size_t)display_width * buffer_lines, sizeof(*draw_buffer));
	if (draw_buffer == NULL) {
		perror("allocate LVGL draw buffer");
		camera_close(&camera);
		framebuffer_close(&fb);
		close(drm_fd);
		drm_fd = -1;
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
		camera_close(&camera);
		framebuffer_close(&fb);
		close(drm_fd);
		drm_fd = -1;
		return EXIT_FAILURE;
	}
	create_status_panel();
	/* Consume LVGL's initial full-screen invalidation before camera frames arrive. */
	lv_tick_inc(1);
	lv_timer_handler();

	signal(SIGINT, stop_handler);
	signal(SIGTERM, stop_handler);
	camera_poll.fd = camera.fd;
	camera_poll.events = POLLIN;
	camera_poll.revents = 0;
	previous_tick = monotonic_milliseconds();
	fps_start = previous_tick;
	while (keep_running) {
		int poll_result = poll(&camera_poll, 1, 10);
		int frame_ready = 0;
		uint32_t current_tick;

		if (poll_result < 0) {
			if (errno == EINTR && !keep_running)
				break;
			if (errno != EINTR) {
				perror("poll /dev/video0");
				exit_status = EXIT_FAILURE;
				break;
			}
		} else if (poll_result > 0) {
			if (camera_poll.revents & (POLLERR | POLLHUP | POLLNVAL)) {
				fprintf(stderr,
					"poll reported an error for /dev/video0 (0x%x)\n",
					camera_poll.revents);
				exit_status = EXIT_FAILURE;
				break;
			}
			if (camera_poll.revents & POLLIN) {
				int frame_result = camera_update_frame(&camera, &fb);

				if (frame_result < 0) {
					exit_status = EXIT_FAILURE;
					break;
				}
				if (frame_result > 0) {
					++fps_frames;
					/* Recompose the panel on the back buffer before presenting. */
					lv_obj_invalidate(status_panel);
					frame_ready = 1;
				}
			}
		}

		current_tick = monotonic_milliseconds();
		if (current_tick - fps_start >= 1000U) {
			uint32_t elapsed = current_tick - fps_start;
			unsigned int measured_fps =
				(unsigned int)(((uint64_t)fps_frames * 1000ULL) / elapsed);

			lv_label_set_text_fmt(fps_label,
					      "1280x720 RGB565  |  FPS: %u",
					      measured_fps);
			lv_obj_invalidate(status_panel);
			fprintf(stderr, "camera -> DRM double buffer + LVGL: %u fps\n",
				measured_fps);
			fps_frames = 0;
			fps_start = current_tick;
		}
		lv_tick_inc(current_tick - previous_tick);
		previous_tick = current_tick;
		lv_timer_handler();
		if (frame_ready && framebuffer_present(&fb) != 0) {
			exit_status = EXIT_FAILURE;
			break;
		}
	}

	free(draw_buffer);
	camera_close(&camera);
	framebuffer_close(&fb);
	close(drm_fd);
	drm_fd = -1;
	return exit_status;
}
