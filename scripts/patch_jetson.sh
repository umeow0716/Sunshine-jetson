#!/usr/bin/env bash

set -euo pipefail

readonly package_name="uinput-tegra"
readonly package_version="6.8.12-r39.2"
readonly supported_kernel_pattern='^6\.8\.12-[0-9]+-tegra$'

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly script_dir
repository_dir="$(cd -- "${script_dir}/.." && pwd)"
readonly repository_dir
readonly uinput_source_dir="${repository_dir}/packaging/linux/jetson/uinput"
readonly udev_rules_source="${repository_dir}/src_assets/linux/misc/60-sunshine.rules"
readonly modules_load_source="${repository_dir}/src_assets/linux/misc/60-sunshine.conf"

kernel_version="$(uname -r)"
target_user="${SUDO_USER:-${USER:-}}"
skip_test=0
temporary_dir=""

function usage() {
  cat <<EOF
Install and test the NVIDIA Jetson uinput DKMS module used by Sunshine.

Usage:
  $0 [options]

Options:
  -h, --help              Display this help message.
  --kernel-version=VALUE  Build for VALUE instead of the running kernel.
  --user=VALUE            Add VALUE to the input group and run the probe as it.
  --skip-test             Install without running the uinput runtime probe.
EOF
}

function fail() {
  echo "error: $*" >&2
  exit 1
}

function require_command() {
  command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"
}

function cleanup() {
  if [[ -n "${temporary_dir}" ]]; then
    rm -f -- "${temporary_dir}/uinput_probe"
    rmdir -- "${temporary_dir}" 2>/dev/null || true
  fi
}

trap cleanup EXIT

for argument in "$@"; do
  case "${argument}" in
    -h | --help)
      usage
      exit 0
      ;;
    --kernel-version=*) kernel_version="${argument#*=}" ;;
    --user=*) target_user="${argument#*=}" ;;
    --skip-test) skip_test=1 ;;
    *) fail "unknown option: ${argument}" ;;
  esac
done

for command in cc depmod dkms getent id install make modinfo modprobe stat udevadm uname; do
  require_command "${command}"
done

if [[ "$(uname -m)" != "aarch64" ]]; then
  fail "this patch supports only aarch64 Jetson hosts"
fi
if [[ ! "${kernel_version}" =~ ${supported_kernel_pattern} ]]; then
  fail "unsupported kernel ${kernel_version}; expected 6.8.12-*-tegra"
fi

readonly kernel_build_dir="/lib/modules/${kernel_version}/build"
readonly kernel_config="${kernel_build_dir}/.config"
[[ -d "${kernel_build_dir}" ]] || fail "kernel headers not found: ${kernel_build_dir}"
[[ -f "${kernel_config}" ]] || fail "kernel configuration not found: ${kernel_config}"
grep -Eq '^CONFIG_INPUT=(y|m)$' "${kernel_config}" || fail "CONFIG_INPUT is disabled"
if grep -Eq '^CONFIG_INPUT_UINPUT=(y|m)$' "${kernel_config}"; then
  fail "CONFIG_INPUT_UINPUT is already enabled; an external module is unnecessary"
fi

if ((EUID == 0)); then
  root_command=()
else
  require_command sudo
  root_command=(sudo)
fi
readonly root_command

function run_root() {
  "${root_command[@]}" "$@"
}

function run_as_target_user() {
  if [[ -z "${target_user}" || "${target_user}" == "root" ]]; then
    run_root "$@"
  elif ((EUID == 0)); then
    require_command runuser
    runuser -u "${target_user}" -g input -- "$@"
  else
    sudo -u "${target_user}" -g input -- "$@"
  fi
}

if [[ -n "${target_user}" && "${target_user}" != "root" ]]; then
  id "${target_user}" >/dev/null 2>&1 || fail "user not found: ${target_user}"
fi

readonly dkms_source_dir="/usr/src/${package_name}-${package_version}"
echo "Installing ${package_name}/${package_version} for ${kernel_version}."
run_root install -d -m 0755 "${dkms_source_dir}/drivers/input/misc"
run_root install -m 0644 \
  "${uinput_source_dir}/Makefile" \
  "${uinput_source_dir}/dkms.conf" \
  "${uinput_source_dir}/README.md" \
  "${uinput_source_dir}/COPYING" \
  "${dkms_source_dir}/"
run_root install -m 0644 \
  "${uinput_source_dir}/drivers/input/input-compat.h" \
  "${dkms_source_dir}/drivers/input/"
run_root install -m 0644 \
  "${uinput_source_dir}/drivers/input/misc/uinput.c" \
  "${dkms_source_dir}/drivers/input/misc/"

package_status="$(run_root dkms status -m "${package_name}" -v "${package_version}" 2>/dev/null || true)"
if [[ -z "${package_status}" ]]; then
  run_root dkms add -m "${package_name}" -v "${package_version}"
fi

kernel_status="$(run_root dkms status -m "${package_name}" -v "${package_version}" -k "${kernel_version}" 2>/dev/null || true)"
if [[ "${kernel_status}" != *": built"* && "${kernel_status}" != *": installed"* ]]; then
  run_root dkms build -m "${package_name}" -v "${package_version}" -k "${kernel_version}"
fi
if [[ "${kernel_status}" != *": installed"* ]]; then
  run_root dkms install -m "${package_name}" -v "${package_version}" -k "${kernel_version}"
fi
run_root depmod -a "${kernel_version}"

run_root install -m 0644 "${udev_rules_source}" /etc/udev/rules.d/60-sunshine.rules
run_root install -m 0644 "${modules_load_source}" /etc/modules-load.d/60-sunshine.conf

if ! getent group input >/dev/null; then
  require_command groupadd
  run_root groupadd --system input
fi
if [[ -n "${target_user}" && "${target_user}" != "root" ]] && ! id -nG "${target_user}" | grep -qw input; then
  require_command usermod
  run_root usermod -aG input "${target_user}"
  echo "Added ${target_user} to the input group; new sessions will inherit it."
fi

if [[ "${kernel_version}" != "$(uname -r)" ]]; then
  echo "Installed for ${kernel_version}; skipping load and runtime test because it is not running."
  exit 0
fi

run_root udevadm control --reload-rules
run_root modprobe uinput
run_root modprobe uhid
for sysfs_device in /sys/class/misc/uinput /sys/class/misc/uhid; do
  if [[ -e "${sysfs_device}" ]]; then
    run_root udevadm trigger --action=add "${sysfs_device}"
  fi
done
run_root udevadm settle

for device in /dev/uinput /dev/uhid; do
  [[ -c "${device}" ]] || fail "device node was not created: ${device}"
  [[ "$(stat -c '%a:%G' "${device}")" == "660:input" ]] || fail "unexpected permissions on ${device}"
done

module_path="$(modinfo -F filename uinput)"
[[ "${module_path}" == "/lib/modules/${kernel_version}/updates/dkms/uinput.ko" ]] || fail "unexpected uinput module: ${module_path}"

if ((skip_test == 0)); then
  temporary_dir="$(mktemp -d -t sunshine-uinput-probe.XXXXXX)"
  chmod 0755 "${temporary_dir}"
  cc -std=c11 -Wall -Wextra -Werror -O2 \
    "${uinput_source_dir}/uinput_probe.c" \
    -o "${temporary_dir}/uinput_probe"
  run_as_target_user "${temporary_dir}/uinput_probe"
fi

echo "Jetson uinput DKMS installation and virtual-input permissions are ready."
