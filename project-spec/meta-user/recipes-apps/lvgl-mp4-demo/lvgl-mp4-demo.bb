SUMMARY = "Play a local MP4 in a 720x480 LVGL window"
SECTION = "PETALINUX/apps"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

DEPENDS = "lvgl ffmpeg libdrm"
SRC_URI = "file://lvgl-mp4-demo.c \
           file://Makefile \
          "

S = "${WORKDIR}"

do_compile() {
	oe_runmake STAGING_INCDIR=${STAGING_INCDIR} \
		STAGING_LIBDIR=${STAGING_LIBDIR}
}

do_install() {
	install -d ${D}${bindir}
	install -m 0755 ${S}/lvgl-mp4-demo ${D}${bindir}/lvgl-mp4-demo
}
