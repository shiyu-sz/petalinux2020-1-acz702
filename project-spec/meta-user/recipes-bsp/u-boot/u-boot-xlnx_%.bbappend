FILESEXTRAPATHS_prepend := "${THISDIR}/files:"

SRC_URI += "file://platform-top.h"

do_configure_append () {
	if [ "${U_BOOT_AUTO_CONFIG}" = "1" ]; then
		install ${WORKDIR}/platform-auto.h ${S}/include/configs/
		install ${WORKDIR}/platform-top.h ${S}/include/configs/
	fi
	# The selected xilinx_zynq_virt_defconfig also uses this Kconfig value.
	# Keep U-Boot's QSPI script lookup aligned with qspi-all.bif.
	sed -i 's/^CONFIG_BOOT_SCRIPT_OFFSET=.*/CONFIG_BOOT_SCRIPT_OFFSET=0x1F0000/' ${B}/.config
	grep -q '^CONFIG_BOOT_SCRIPT_OFFSET=0x1F0000$' ${B}/.config
}

do_configure_append_microblaze () {
	if [ "${U_BOOT_AUTO_CONFIG}" = "1" ]; then
		install -d ${B}/source/board/xilinx/microblaze-generic/
		install ${WORKDIR}/config.mk ${B}/source/board/xilinx/microblaze-generic/
	fi
}
