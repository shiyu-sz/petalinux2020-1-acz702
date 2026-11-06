FILESEXTRAPATHS_prepend := "${THISDIR}/${PN}:"

SRC_URI += "file://boot.cmd.default.initrd"

BOOTMODE = "default"
BOOTFILE_EXT = ".initrd"

do_compile_prepend() {
	cp "${WORKDIR}/boot.cmd.default.initrd" "${WORKDIR}/boot.cmd.default.${SOC_FAMILY}"
}
