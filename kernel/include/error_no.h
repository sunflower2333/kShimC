/* SPDX-License-Identifier: MIT */
#pragma once
// Error codes for common errors
enum {
    ERROR_NONE = 0,
    ERROR_FW,
    ERROR_DEVICE_ERROR,
    ERROR_INVALID_PARAMETER,
    ERROR_NOT_SUPPORTED,
    ERROR_NOT_FOUND,
    ERROR_TIMEOUT,
    ERROR_BUSY,      /* already owned (e.g. an INTID with a handler) */
    ERROR_NOT_READY, /* subsystem not initialised yet */
};
