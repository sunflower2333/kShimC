# SPDX-License-Identifier: MIT
# Owner: qup. Host register-model tests (docs/io-stack.md "Test plan").

# kernel/drivers/console.c sits on the UART layer (kshim/uart.h) now: the
# existing console test links it too.
target_sources(usb_acm_test PRIVATE "${KSHIM_ROOT}/kernel/drivers/qup/geni_uart.c")
