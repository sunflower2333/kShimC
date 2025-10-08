#pragma once
#include "kshim.h"
#include "qupv3.h"

int setup_se_geni_uart(uint64_t mmio_base);
void geni_uart_write(uint64_t mmio_base, const char *buf, uint32_t len);
