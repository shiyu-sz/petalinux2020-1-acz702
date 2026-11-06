# LVGL MP4 demo

This PetaLinux app decodes a local MP4 with FFmpeg and displays video in a
720×480 LVGL window centered on the active framebuffer. It uses the project's
LVGL 8.3.11 RGB565 configuration and `/dev/fb0`. Video is scaled to fit while
preserving its aspect ratio. Audio is not played. Playback ends at the end of
the file, and Ctrl+C stops it early.

Build the app and image from the project root:

```sh
source /tools/petalinux-v2020.1/settings.sh
petalinux-build -c lvgl-mp4-demo
petalinux-build
```

The app is selected in `project-spec/configs/rootfs_config`. FFmpeg libraries
are installed through the app's runtime dependencies. The first build may
need to fetch and compile FFmpeg and its dependencies.

On the target, run the player with a local file. For the project's 1280×720
framebuffer, the app selects the connected HDMI output's 1280×720 mode itself:

```sh
lvgl-mp4-demo /path/to/video.mp4
```

Stop other apps that write to `/dev/fb0` before running this example. Software
decoding and scaling on the Zynq Cortex-A9 can run slower than the source video.
The decoder requests two FFmpeg threads for codecs that support it; the startup
log reports the mode selected, and the completion log reports elapsed time.

`/media/sy/BOOT` is the SD card mount point on the development host. The target
may mount the same partition somewhere else. On the target, check the actual
path before running the player:

```sh
mount | grep mmc
ls -l /media/sd-mmcblk0p1/mp4/168811_720p.mp4
lvgl-mp4-demo /media/sd-mmcblk0p1/mp4/168811_720p.mp4 2>&1
```
