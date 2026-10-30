FILESEXTRAPATHS_prepend := "${THISDIR}/files:"
SRC_URI += " \
	file://pl-uart16550.cfg \
	file://hdmi-display.cfg \
	file://0002-clk-Add-driver-for-axi_dynclk-IP-Core.patch \
	file://0003-drm-xlnx-add-hdmi-output-and-set-vtc-pixel-clock.patch \
"
