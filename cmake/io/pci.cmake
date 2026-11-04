# SPDX-License-Identifier: MIT
# Owner: pci. PCI core (kernel/drivers/pci/), generic ECAM and Qualcomm hosts.
# CrDK PcieLib selection: ports/crdk/io/pci.cmake. See docs/io-stack.md and
# docs/pci.md.
if(CONFIG_KSHIM_PCI)
    list(APPEND KERNEL_SOURCES kernel/drivers/pci/core.c kernel/drivers/pci/dt.c
        kernel/drivers/pci/alloc.c kernel/drivers/pci/map.c)
endif()
if(CONFIG_KSHIM_PCI_ECAM)
    list(APPEND KERNEL_SOURCES kernel/drivers/pci/ecam.c)
endif()
if(CONFIG_KSHIM_PCI_SELFTEST)
    list(APPEND KERNEL_SOURCES kernel/drivers/pci/selftest.c)
endif()
if(CONFIG_KSHIM_PCI_QCOM)
    set(CRDK_PCIE ON CACHE BOOL "" FORCE)
    list(APPEND KERNEL_SOURCES kernel/drivers/pci/qcom.c kernel/drivers/pci/qcom_pcielib.c
        kernel/drivers/pci/qcom_sm8650.c)
endif()
