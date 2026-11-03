SUMMARY = "LVGL embedded graphics library"
SECTION = "libs"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${S}/LICENCE.txt;md5=bf1198c89ae87f043108cea62460b03a"

FILESEXTRAPATHS_prepend := "${THISDIR}/files:"
SRC_URI = "file://lvgl-8.3.11.tar.gz \
           file://lv_conf.h \
           file://Makefile \
          "

S = "${WORKDIR}/lvgl-8.3.11"

ALLOW_EMPTY_${PN} = "1"
FILES_${PN}-staticdev += "${libdir}/liblvgl.a"

do_compile() {
	oe_runmake -C ${WORKDIR} LVGL_DIR=${S}
}

do_install() {
	install -d ${D}${libdir} ${D}${includedir}/lvgl
	install -m 0644 ${WORKDIR}/liblvgl.a ${D}${libdir}/liblvgl.a
	install -m 0644 ${S}/lvgl.h ${D}${includedir}/lvgl/lvgl.h
	install -m 0644 ${WORKDIR}/lv_conf.h ${D}${includedir}/lv_conf.h
	cd ${S}; \
	for header in $(find src -type f -name '*.h'); do \
		install -D -m 0644 ${S}/$header ${D}${includedir}/lvgl/$header; \
	done
}
