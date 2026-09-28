/*
 * The imported PCIe TLM model includes <linux/pci.h> but uses no declarations
 * from it when CONFIG_TLM is enabled.  macOS does not ship this Linux UAPI
 * header, so keep a deliberately empty compatibility shim for that build.
 */

#ifndef QBOX_PCIE_COMPAT_LINUX_PCI_H
#define QBOX_PCIE_COMPAT_LINUX_PCI_H
#endif
