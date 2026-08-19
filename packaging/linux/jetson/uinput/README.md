# NVIDIA Jetson uinput DKMS source

This directory restores the in-tree Linux `uinput` driver as an external DKMS
module for NVIDIA Jetson Linux kernels that disable `CONFIG_INPUT_UINPUT`.

- DKMS package: `uinput-tegra/6.8.12-r39.2`
- Source release: NVIDIA Jetson Linux R39.2 (`jetson_39.2_GA`)
- Kernel source version: `6.8.12`
- NVIDIA source bundle:
  `https://developer.download.nvidia.com/embedded/L4T/r39_Release_v2.0/sources/public_sources.tbz2`
- NVIDIA source bundle SHA-256:
  `87d2e31ff55beaf2373e2f288538585995b231fd5745ec21f39a668e36efab2f`
- `drivers/input/misc/uinput.c` SHA-256:
  `a426ace8f25052d3f7c9f0f61270f12bd21d0405f2553ecab50a3ef8a0d8b5a2`
- `drivers/input/input-compat.h` SHA-256:
  `9cdafd83fcc42d3ea22402558a1b3b3e9092d28292fc0271f162ad92fcb44c0f`

The source retains its Linux kernel SPDX license identifiers. `COPYING`
contains the GNU GPL version 2 text; the SPDX identifiers on individual files
state whether later versions are permitted.

The module must be compiled against the headers and `Module.symvers` belonging
to the target kernel. The DKMS configuration therefore restricts automatic
builds to AArch64 `6.8.12-*-tegra` kernels that have the input subsystem enabled
and built-in uinput disabled.
