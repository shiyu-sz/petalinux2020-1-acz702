FILESEXTRAPATHS_prepend := "${THISDIR}/files:"
SRC_URI += " \
	file://pl-uart16550.cfg \
	file://hdmi-display.cfg \
	file://ov2640-camera.cfg \
	file://0002-clk-Add-driver-for-axi_dynclk-IP-Core.patch \
	file://0003-drm-xlnx-add-hdmi-output-and-set-vtc-pixel-clock.patch \
	file://0004-media-xilinx-map-ov2640-dvp-uyvy.patch \
	file://0005-media-xilinx-initialize-mplane-capture-format.patch \
	file://0006-media-ov2640-default-rgb565-800x480.patch \
"
