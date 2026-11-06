do_configure_append () {
	# Ignore any environment saved by the previous QSPI image. SD boot must
	# use the default distro boot command, which searches the SD card.
	sed -i 's/^CONFIG_ENV_IS_IN_SPI_FLASH=y$/# CONFIG_ENV_IS_IN_SPI_FLASH is not set/' ${B}/.config
	if grep -q '^# CONFIG_ENV_IS_NOWHERE is not set$' ${B}/.config; then
		sed -i 's/^# CONFIG_ENV_IS_NOWHERE is not set$/CONFIG_ENV_IS_NOWHERE=y/' ${B}/.config
	else
		echo 'CONFIG_ENV_IS_NOWHERE=y' >> ${B}/.config
	fi
	grep -q '^CONFIG_ENV_IS_NOWHERE=y$' ${B}/.config
}
