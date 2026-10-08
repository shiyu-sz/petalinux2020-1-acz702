#include <configs/zynq-common.h>
#include <configs/platform-auto.h>

/* qspi-all.bif stores boot.scr in the final 64 KiB block before image.ub. */
#undef CONFIG_BOOT_SCRIPT_OFFSET
#define CONFIG_BOOT_SCRIPT_OFFSET 0x001F0000
