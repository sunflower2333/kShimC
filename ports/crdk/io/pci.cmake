# SPDX-License-Identifier: MIT
# Owner: pci. CrDK PcieLib for the Qualcomm root complex (docs/io-stack.md).
# Included by ports/crdk/CMakeLists.txt before CRDK_SOURCES is made absolute:
# append CrDK-relative paths; set_source_files_properties() takes
# "${CRDK_ROOT}/..." paths.
option(CRDK_PCIE "Build PcieLib (Qualcomm RC + QMP PHY) for kShimC" OFF)
