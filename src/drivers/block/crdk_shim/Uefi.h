/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * kShimC shim for the <Uefi.h> include found in CrDK target-data headers
 * (CrTargetUfsLib.h, CrTargetSdLib.h, CrDalDevice.h) and the UFS power
 * policy library. It is placed ahead of the CrDK include directory only for
 * the CrDK storage sources; it maps the few EFI names those files use onto
 * the CR_FREESTANDING OSKAL contract in crdk_os.h. No UEFI service exists.
 */
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>
#include <oskal/common.h>

typedef CR_STATUS EFI_STATUS;
#define EFI_SUCCESS           CR_SUCCESS
#define EFI_INVALID_PARAMETER CR_INVALID_PARAMETER
#define EFI_UNSUPPORTED       CR_UNSUPPORTED
#define EFI_NOT_FOUND         CR_NOT_FOUND
#define EFI_NOT_READY         CR_NOT_READY
#define EFI_DEVICE_ERROR      CR_DEVICE_ERROR
#define EFI_TIMEOUT           CR_TIMEOUT
#define EFI_COMPROMISED_DATA  CR_COMPROMISED_DATA
#define EFI_OUT_OF_RESOURCES  CR_OUT_OF_RESOURCES
#define EFI_BUFFER_TOO_SMALL  CR_BUFFER_TOO_SMALL
#define EFI_ERROR(s)          CR_ERROR(s)

typedef struct { UINT32 Data1; UINT16 Data2, Data3; UINT8 Data4[8]; } EFI_GUID;

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef BIT0
#define BIT0  0x00000001u
#define BIT1  0x00000002u
#define BIT2  0x00000004u
#define BIT3  0x00000008u
#define BIT4  0x00000010u
#define BIT5  0x00000020u
#define BIT6  0x00000040u
#define BIT7  0x00000080u
#endif
