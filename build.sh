#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SD_DIR=/media/sy/BOOT
cd "$ROOT_DIR"

usage() {
    echo "Usage: $0 [clean] [build] [sd]" >&2
    echo "Run one or more steps in this order; sd copies files to $SD_DIR." >&2
}

if (( $# == 0 )); then
    usage
    exit 2
fi

# Validate every argument before making changes.
last_step=0
for step in "$@"; do
    case "$step" in
        clean) step_number=1 ;;
        build) step_number=2 ;;
        sd)    step_number=3 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown step: $step" >&2; usage; exit 2 ;;
    esac
    if (( step_number <= last_step )); then
        echo "Steps must be unique and in the order: clean build sd" >&2
        exit 2
    fi
    last_step=$step_number
done

setup_petalinux() {
    if ! command -v petalinux-build >/dev/null 2>&1; then
        # shellcheck disable=SC1090
        source "${PETALINUX_SETTINGS:-/tools/petalinux-v2020.1/settings.sh}"
    fi
}

check_sd_files() {
    local artifact
    for artifact in BOOT.BIN boot.scr image.ub; do
        if [[ ! -s "images/linux/$artifact" ]]; then
            echo "Missing build artifact: images/linux/$artifact" >&2
            exit 1
        fi
    done
    if ! grep -aq 'fatload mmc 0:1 0x10000000 image.ub' images/linux/boot.scr; then
        echo "boot.scr does not contain the SD boot command; refusing to copy stale artifacts" >&2
        exit 1
    fi
}

for step in "$@"; do
    case "$step" in
        clean)
            setup_petalinux
            petalinux-build -x mrproper -f
            ;;
        build)
            setup_petalinux
            if [[ ! -s vivado_project/design_1_wrapper.xsa ]]; then
                echo "Missing Vivado hardware description: vivado_project/design_1_wrapper.xsa" >&2
                exit 1
            fi
            petalinux-config --silentconfig
            petalinux-build

            for artifact in zynq_fsbl.elf system.bit u-boot.elf boot.scr image.ub; do
                if [[ ! -s "images/linux/$artifact" ]]; then
                    echo "Missing build artifact: images/linux/$artifact" >&2
                    exit 1
                fi
            done
            if ! grep -aq 'fatload mmc 0:1 0x10000000 image.ub' images/linux/boot.scr; then
                echo "boot.scr does not contain the SD boot command; refusing to package stale artifacts" >&2
                exit 1
            fi

            petalinux-package --boot \
                --fsbl images/linux/zynq_fsbl.elf \
                --fpga images/linux/system.bit \
                --u-boot images/linux/u-boot.elf \
                --output images/linux/BOOT.BIN \
                --force
            [[ -s images/linux/BOOT.BIN ]] || { echo "BOOT.BIN was not created" >&2; exit 1; }
            ;;
        sd)
            check_sd_files
            if [[ ! -d "$SD_DIR" ]] || ! mountpoint -q "$SD_DIR"; then
                echo "SD boot partition is not mounted at $SD_DIR" >&2
                exit 1
            fi
            mount_options="$(findmnt -no OPTIONS --mountpoint "$SD_DIR")"
            if [[ ",$mount_options," == *,ro,* ]] || [[ ! -w "$SD_DIR" ]]; then
                echo "SD boot partition at $SD_DIR is not writable" >&2
                exit 1
            fi
            cp images/linux/BOOT.BIN images/linux/boot.scr images/linux/image.ub "$SD_DIR/"
            sync
            echo "Copied BOOT.BIN, boot.scr and image.ub to $SD_DIR"
            ;;
    esac
done
