# camera-fb-lvgl-alpha

This app copies each 1280x720 RGB565 camera frame to the DRM back buffer and
alpha blends an LVGL status panel over that frame before the page flip. It uses
the same `/dev/video0` and `/dev/fb0` devices as `camera-fb-lvgl`.

The shared LVGL recipe enables `LV_COLOR_SCREEN_TRANSP`. With 16-bit LVGL color,
its transparent draw buffer contains three bytes per pixel: RGB565 followed by
alpha. The display remains RGB565; `framebuffer_flush()` blends only the pixels
that LVGL redraws. The panel background is set to `LV_OPA_50` in
`create_status_panel()`.

Build the app with `petalinux-build -c camera-fb-lvgl-alpha`, then build the
image with `petalinux-build`. The rootfs configuration selects this package.
On the target, stop any other app using the camera or display and run
`camera-fb-lvgl-alpha`. Press Ctrl+C to exit.

Check that moving video remains visible through the panel, that the text and
rounded edges look correct, and that the panel stays visible on every frame.
Compare the reported FPS with `camera-fb-lvgl` on the same target.
