#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build separate arm64 kernels for the vmshm client VM and proxy VM.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARCH="${ARCH:-arm64}"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
BASE_DEFCONFIG="${BASE_DEFCONFIG:-defconfig}"
BASE_FRAGMENT="${BASE_FRAGMENT:-rk3588_fragment.config}"
BUILD_ROOT="${BUILD_ROOT:-${ROOT_DIR}/out/vmshm-arm64}"
TARGETS="${TARGETS:-Image}"
JOBS="${JOBS:-$(nproc)}"
IN_TREE_CONFIG_DIR="${IN_TREE_CONFIG_DIR:-${BUILD_ROOT}/configs}"
FIRECRACKER_BINS="${FIRECRACKER_BINS:-/home/mzh/gpu/GPU-SFTP/firecracker-bins}"
CLIENT_KERNEL_DIR="${CLIENT_KERNEL_DIR:-${FIRECRACKER_BINS}/kernels/shared/client}"
PROXY_KERNEL_DIR="${PROXY_KERNEL_DIR:-${FIRECRACKER_BINS}/kernels/shared/proxy}"
IN_TREE_BUILD=false

make_args=(
	-C "${ROOT_DIR}"
	ARCH="${ARCH}"
	CROSS_COMPILE="${CROSS_COMPILE}"
)

write_role_fragment() {
	local role="$1"
	local fragment="$2"

	case "${role}" in
	client)
		cat >"${fragment}" <<'EOF'
CONFIG_DRM=y
# CONFIG_DRM_PANTHOR is not set
CONFIG_DRM_PANTHOR_CLIENT=y
# CONFIG_DRM_PANTHOR_PROXY is not set
# CONFIG_PROXY_VMSHM_MANAGER is not set
# CONFIG_PROXY_VMSHM_MANAGER_SELFTEST is not set
# CONFIG_PROXY_VMSHM_COMM is not set
CONFIG_CLIENT_VMSHM_COMM=y
CONFIG_CLIENT_VMSHM_MANAGER=y
CONFIG_DRM_PANTHOR_CLIENT_DEV_QUERY_SELFTEST=y
CONFIG_DRM_PANTHOR_CLIENT_DEV_QUERY_PERF_SELFTEST=y
CONFIG_CLIENT_VMSHM_COMM_PERF_SELFTEST=y
EOF
		;;
	proxy)
		cat >"${fragment}" <<'EOF'
CONFIG_DRM=y
CONFIG_DRM_PANTHOR=y
# CONFIG_DRM_PANTHOR_CLIENT is not set
CONFIG_DRM_PANTHOR_PROXY=y
CONFIG_PROXY_VMSHM_MANAGER=y
# CONFIG_PROXY_VMSHM_MANAGER_SELFTEST is not set
CONFIG_PROXY_VMSHM_COMM=y
CONFIG_PROXY_VMSHM_COMM_PERF_SELFTEST=y
# CONFIG_CLIENT_VMSHM_COMM is not set
# CONFIG_CLIENT_VMSHM_MANAGER is not set
EOF
		;;
	*)
		echo "unknown role: ${role}" >&2
		exit 2
		;;
	esac
}

configure_role() {
	local role="$1"
	local out_dir="${BUILD_ROOT}/${role}"
	local role_fragment="${out_dir}/${role}-vmshm.config"
	local config_file
	local fragments=()

	mkdir -p "${out_dir}"
	write_role_fragment "${role}" "${role_fragment}"

	if [[ -e "${ROOT_DIR}/.config" ||
	      -d "${ROOT_DIR}/include/config" ||
	      -d "${ROOT_DIR}/arch/${ARCH}/include/generated" ]]; then
		mkdir -p "${IN_TREE_CONFIG_DIR}"
		out_dir="${ROOT_DIR}"
		role_fragment="${IN_TREE_CONFIG_DIR}/${role}-vmshm.config"
		config_file="${IN_TREE_CONFIG_DIR}/${role}.config"
		write_role_fragment "${role}" "${role_fragment}"

		IN_TREE_BUILD=true
		make "${make_args[@]}" KCONFIG_CONFIG="${config_file}" "${BASE_DEFCONFIG}"
	else
		config_file="${out_dir}/.config"
		make "${make_args[@]}" O="${out_dir}" "${BASE_DEFCONFIG}"
	fi

	if [[ -f "${ROOT_DIR}/${BASE_FRAGMENT}" ]]; then
		fragments+=("${ROOT_DIR}/${BASE_FRAGMENT}")
	fi
	fragments+=("${role_fragment}")

	if [[ "${out_dir}" == "${ROOT_DIR}" ]]; then
		KCONFIG_CONFIG="${config_file}" "${ROOT_DIR}/scripts/kconfig/merge_config.sh" \
			-m \
			"${IN_TREE_CONFIG_DIR}/${role}.config" "${fragments[@]}"
		make "${make_args[@]}" KCONFIG_CONFIG="${config_file}" olddefconfig
	else
		"${ROOT_DIR}/scripts/kconfig/merge_config.sh" \
			-m -O "${out_dir}" "${out_dir}/.config" "${fragments[@]}"
		make "${make_args[@]}" O="${out_dir}" olddefconfig
	fi
}

copy_in_tree_artifacts() {
	local role="$1"
	local artifact_dir="${BUILD_ROOT}/${role}/artifacts"
	local image="${ROOT_DIR}/arch/${ARCH}/boot/Image"

	if [[ " ${TARGETS} " != *" Image "* && " ${TARGETS} " != *" all "* ]]; then
		return 0
	fi

	mkdir -p "${artifact_dir}"
	if [[ -f "${image}" ]]; then
		cp -f "${image}" "${artifact_dir}/Image"
	fi
}

build_role() {
	local role="$1"
	local out_dir="${BUILD_ROOT}/${role}"

	configure_role "${role}"
	if [[ "${IN_TREE_BUILD}" == true ]]; then
		make "${make_args[@]}" KCONFIG_CONFIG="${IN_TREE_CONFIG_DIR}/${role}.config" \
			-j"${JOBS}" ${TARGETS}
		copy_in_tree_artifacts "${role}"
	else
		make "${make_args[@]}" O="${out_dir}" -j"${JOBS}" ${TARGETS}
	fi
}

role_image_path() {
	local role="$1"

	if [[ "${IN_TREE_BUILD}" == true ]]; then
		printf '%s/%s/artifacts/Image\n' "${BUILD_ROOT}" "${role}"
	else
		printf '%s/%s/arch/%s/boot/Image\n' "${BUILD_ROOT}" "${role}" "${ARCH}"
	fi
}

install_role_image() {
	local role="$1"
	local dst_dir="$2"
	local image

	if [[ " ${TARGETS} " != *" Image "* && " ${TARGETS} " != *" all "* ]]; then
		return 0
	fi

	image="$(role_image_path "${role}")"
	if [[ ! -f "${image}" ]]; then
		echo "missing ${role} Image: ${image}" >&2
		exit 1
	fi

	mkdir -p "${dst_dir}"
	cp -f "${image}" "${dst_dir}/Image"
	echo "${role} Image installed: ${dst_dir}/Image"
}

if [[ -e "${ROOT_DIR}/.config" ||
      -d "${ROOT_DIR}/include/config" ||
      -d "${ROOT_DIR}/arch/${ARCH}/include/generated" ]]; then
	echo "source tree has existing generated config; using sequential in-tree builds"
	echo "role configs will be written under ${IN_TREE_CONFIG_DIR}"
fi

build_role client
build_role proxy

install_role_image client "${CLIENT_KERNEL_DIR}"
install_role_image proxy "${PROXY_KERNEL_DIR}"

if [[ "${IN_TREE_BUILD}" == true ]]; then
	if [[ " ${TARGETS} " == *" Image "* || " ${TARGETS} " == *" all "* ]]; then
		echo "client Image: ${BUILD_ROOT}/client/artifacts/Image"
		echo "proxy Image:  ${BUILD_ROOT}/proxy/artifacts/Image"
	else
		echo "client config: ${IN_TREE_CONFIG_DIR}/client.config"
		echo "proxy config:  ${IN_TREE_CONFIG_DIR}/proxy.config"
	fi
else
	echo "client build: ${BUILD_ROOT}/client"
	echo "proxy build:  ${BUILD_ROOT}/proxy"
fi
