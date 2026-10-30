#include <configs/zynq-common.h>
#include <configs/platform-auto.h>

/* Keep the U-Boot script lookup aligned with qspi-all.bif. */
#undef CONFIG_BOOT_SCRIPT_OFFSET
#define CONFIG_BOOT_SCRIPT_OFFSET 0x00230000
