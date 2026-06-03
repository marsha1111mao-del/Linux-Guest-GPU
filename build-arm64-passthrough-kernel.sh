#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build the single arm64 guest kernel used by GPU passthrough performance VMs.
# This intentionally does not build the vmshm client/proxy role kernels.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARCH="${ARCH:-arm64}"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
BASE_DEFCONFIG="${BASE_DEFCONFIG:-defconfig}"
BASE_FRAGMENT="${BASE_FRAGMENT:-rk3588_fragment.config}"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/out/passthrough-arm64}"
TARGETS="${TARGETS:-Image}"
JOBS="${JOBS:-$(nproc)}"
FIRECRACKER_BINS="${FIRECRACKER_BINS:-/home/mzh/gpu/GPU-SFTP/firecracker-bins}"
PASSTHROUGH_IMAGE="${PASSTHROUGH_IMAGE:-${FIRECRACKER_BINS}/kernels/passthrough/Image}"

make_args=(
	-C "${ROOT_DIR}"
	O="${BUILD_DIR}"
	ARCH="${ARCH}"
	CROSS_COMPILE="${CROSS_COMPILE}"
)

mkdir -p "${BUILD_DIR}"

make "${make_args[@]}" "${BASE_DEFCONFIG}"
if [[ -f "${ROOT_DIR}/${BASE_FRAGMENT}" ]]; then
	"${ROOT_DIR}/scripts/kconfig/merge_config.sh" \
		-m -O "${BUILD_DIR}" "${BUILD_DIR}/.config" \
		"${ROOT_DIR}/${BASE_FRAGMENT}"
	make "${make_args[@]}" olddefconfig
fi

make "${make_args[@]}" -j"${JOBS}" ${TARGETS}

if [[ " ${TARGETS} " == *" Image "* || " ${TARGETS} " == *" all "* ]]; then
	image="${BUILD_DIR}/arch/${ARCH}/boot/Image"
	if [[ ! -f "${image}" ]]; then
		echo "missing passthrough Image: ${image}" >&2
		exit 1
	fi

	mkdir -p "$(dirname "${PASSTHROUGH_IMAGE}")"
	cp -f "${image}" "${PASSTHROUGH_IMAGE}"
	echo "passthrough Image installed: ${PASSTHROUGH_IMAGE}"
	strings "${PASSTHROUGH_IMAGE}" 2>/dev/null | grep -a -m 2 'Linux version' || true
else
	echo "passthrough build: ${BUILD_DIR}"
fi
