#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_BIN="${ROOT_DIR}/images/linux/BOOT-QSPI-ALL.BIN"
MAX_QSPI_SIZE=$((16 * 1024 * 1024))
QSPI_KERNEL_OFFSET=$((0x2A0000))
QSPI_BOOTENV_OFFSET=$((0xFD0000))
DO_FLASH=1

usage() {
    cat <<'EOF'
Usage: ./build-and-flash-qspi.sh [--no-flash]

Update the hardware description from vivado_project, clean and build the
PetaLinux project, package the QSPI boot image, and program it over JTAG.

Options:
  --no-flash   Build and package only; do not write the board's QSPI flash
  -h, --help   Show this help

Optional environment variables:
  PETALINUX_SETTINGS  Path to PetaLinux settings.sh
  VITIS_SETTINGS      Path to Vitis settings64.sh
  JTAG_URL            hw_server URL, for example tcp:localhost:3121
  JTAG_TARGET_NAME    Optional JTAG target name for program_flash
EOF
}

for arg in "$@"; do
    case "$arg" in
        --no-flash) DO_FLASH=0 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

source_first_available() {
    local tool_name="$1"
    shift
    local candidate
    for candidate in "$@"; do
        [[ -n "$candidate" ]] || continue
        if [[ -f "$candidate" ]]; then
            # shellcheck disable=SC1090
            source "$candidate"
            return 0
        fi
    done
    echo "Cannot find settings for ${tool_name}. Set the corresponding settings environment variable." >&2
    return 1
}

if ! command -v petalinux-build >/dev/null 2>&1; then
    source_first_available PetaLinux \
        "${PETALINUX_SETTINGS:-}" \
        /opt/pkg/petalinux/settings.sh \
        /tools/petalinux-v2020.1/settings.sh
fi
if ! command -v petalinux-build >/dev/null 2>&1; then
    echo "petalinux-build is not available in PATH." >&2
    exit 1
fi

if [[ "$DO_FLASH" -eq 1 ]] && ! command -v program_flash >/dev/null 2>&1; then
    source_first_available Vitis \
        "${VITIS_SETTINGS:-}" \
        /tools/Xilinx/Vitis/2020.2/settings64.sh
fi
if [[ "$DO_FLASH" -eq 1 ]] && ! command -v program_flash >/dev/null 2>&1; then
    echo "program_flash is not available in PATH." >&2
    exit 1
fi

cd "$ROOT_DIR"

if [[ ! -s vivado_project/design_1_wrapper.xsa ]]; then
    echo "Hardware description is missing or empty: vivado_project/design_1_wrapper.xsa" >&2
    exit 1
fi

echo "[1/5] Updating PetaLinux hardware description from vivado_project..."
petalinux-config --get-hw-description ./vivado_project --silentconfig

echo "[2/5] Cleaning PetaLinux build..."
petalinux-build -x mrproper -f

echo "[3/5] Building PetaLinux project..."
petalinux-build

for artifact in \
    images/linux/zynq_fsbl.elf \
    images/linux/system.bit \
    images/linux/u-boot.elf \
    images/linux/system.dtb \
    images/linux/boot.scr \
    images/linux/image.ub; do
    if [[ ! -s "$artifact" ]]; then
        echo "Required boot artifact is missing or empty: $artifact" >&2
        exit 1
    fi
done

FIT_SIZE="$(wc -c < images/linux/image.ub)"
FIT_CAPACITY=$((QSPI_BOOTENV_OFFSET - QSPI_KERNEL_OFFSET))
if (( FIT_SIZE > FIT_CAPACITY )); then
    echo "image.ub is ${FIT_SIZE} bytes; the QSPI kernel partition holds ${FIT_CAPACITY} bytes." >&2
    echo "Reduce image.ub before packaging or flashing. Any existing BOOT-QSPI-ALL.BIN may be stale." >&2
    exit 1
fi

echo "[4/5] Packaging QSPI boot image..."
petalinux-package --boot \
    --bif qspi-all.bif \
    --output "$OUTPUT_BIN" \
    --force

if [[ ! -s "$OUTPUT_BIN" ]]; then
    echo "QSPI boot image was not created: $OUTPUT_BIN" >&2
    exit 1
fi
IMAGE_SIZE="$(wc -c < "$OUTPUT_BIN")"
if (( IMAGE_SIZE > MAX_QSPI_SIZE )); then
    echo "QSPI image is ${IMAGE_SIZE} bytes, larger than the 16 MiB flash." >&2
    exit 1
fi
echo "QSPI image: $OUTPUT_BIN (${IMAGE_SIZE} bytes)"

if [[ "$DO_FLASH" -eq 0 ]]; then
    echo "Build and packaging complete; QSPI was not programmed."
    exit 0
fi

echo "[5/5] Programming QSPI over JTAG..."
FLASH_ARGS=(
    -f "$OUTPUT_BIN"
    -offset 0
    -flash_type qspi-x4-single
    -fsbl "$ROOT_DIR/images/linux/zynq_fsbl.elf"
    -verify
)
if [[ -n "${JTAG_TARGET_NAME:-}" ]]; then
    FLASH_ARGS+=(-target_name "$JTAG_TARGET_NAME")
fi
if [[ -n "${JTAG_URL:-}" ]]; then
    FLASH_ARGS+=(-url "$JTAG_URL")
fi
program_flash "${FLASH_ARGS[@]}"

echo "QSPI programming completed. Power-cycle or reset the board to boot the new image."
