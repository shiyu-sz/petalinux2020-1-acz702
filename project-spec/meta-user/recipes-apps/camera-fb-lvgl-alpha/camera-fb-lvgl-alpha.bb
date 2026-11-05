SUMMARY = "OV2640 camera with an alpha blended LVGL status overlay"
SECTION = "PETALINUX/apps"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

DEPENDS = "lvgl libdrm"
SRC_URI = "file://camera-fb-lvgl-alpha.c \
           file://Makefile \
          "

S = "${WORKDIR}"

do_compile() {
	oe_runmake STAGING_INCDIR=${STAGING_INCDIR} \
		STAGING_LIBDIR=${STAGING_LIBDIR}
}

do_install() {
	install -d ${D}${bindir}
	install -m 0755 ${S}/camera-fb-lvgl-alpha ${D}${bindir}/camera-fb-lvgl-alpha
}
