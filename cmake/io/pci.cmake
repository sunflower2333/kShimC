# SPDX-License-Identifier: MIT
# Owner: pci. PCI core (kernel/drivers/pci/), generic ECAM and Qualcomm hosts.
# CrDK PcieLib selection: ports/crdk/io/pci.cmake. See docs/io-stack.md.
if(CONFIG_KSHIM_PCI_QCOM)
    set(CRDK_PCIE ON CACHE BOOL "" FORCE)
endif()
