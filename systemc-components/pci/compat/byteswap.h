/* Minimal glibc byteswap.h compatibility for the imported PCIe TLM model. */

#ifndef QBOX_PCIE_COMPAT_BYTESWAP_H
#define QBOX_PCIE_COMPAT_BYTESWAP_H

#include <stdint.h>

#define bswap_16(value) __builtin_bswap16(value)
#define bswap_32(value) __builtin_bswap32(value)
#define bswap_64(value) __builtin_bswap64(value)

#endif
