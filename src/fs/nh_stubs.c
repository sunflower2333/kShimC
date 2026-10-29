/* SPDX-License-Identifier: MIT */
/*
 * Stand-ins for FirmwareLoaderLib readers that are configured out.  NhFs.c
 * dispatches to every reader unconditionally; a disabled reader reports
 * NH_UNSUPPORTED so its filesystem is recognised but refused.  In particular
 * this keeps GPL-2.0-or-later NhErofs.c out of the link when CONFIG_FS_EROFS
 * is not set.  The signatures come from FirmwareLoaderLib's NhInternal.h.
 */
#include <config.h>

#include "NhInternal.h"

#ifndef CONFIG_FS_EXT4
NH_STATUS NhExtOpen(NH_FS *fs) { (void)fs; return NH_UNSUPPORTED; }
NH_STATUS NhExtInode(NH_FS *fs, uint64_t id, NH_FILE *file)
{ (void)fs; (void)id; (void)file; return NH_UNSUPPORTED; }
NH_STATUS NhExtRead(NH_FILE *file, uint64_t offset, void *buffer, size_t bytes)
{ (void)file; (void)offset; (void)buffer; (void)bytes; return NH_UNSUPPORTED; }
NH_STATUS NhExtCheckDirectory(NH_FILE *file, uint64_t offset, uint8_t *block, size_t bytes)
{ (void)file; (void)offset; (void)block; (void)bytes; return NH_UNSUPPORTED; }
#endif

#ifndef CONFIG_FS_FAT
NH_STATUS NhFatOpen(NH_FS *fs) { (void)fs; return NH_UNSUPPORTED; }
NH_STATUS NhFatRead(NH_FILE *file, uint64_t offset, void *buffer, size_t bytes)
{ (void)file; (void)offset; (void)buffer; (void)bytes; return NH_UNSUPPORTED; }
NH_STATUS NhFatLookup(NH_FILE *dir, const char *name, size_t length, NH_FILE *out)
{ (void)dir; (void)name; (void)length; (void)out; return NH_UNSUPPORTED; }
#endif

#ifndef CONFIG_FS_F2FS
NH_STATUS NhF2fsOpen(NH_FS *fs) { (void)fs; return NH_UNSUPPORTED; }
NH_STATUS NhF2fsInode(NH_FS *fs, uint64_t id, NH_FILE *file)
{ (void)fs; (void)id; (void)file; return NH_UNSUPPORTED; }
NH_STATUS NhF2fsRead(NH_FILE *file, uint64_t offset, void *buffer, size_t bytes)
{ (void)file; (void)offset; (void)buffer; (void)bytes; return NH_UNSUPPORTED; }
NH_STATUS NhF2fsLookup(NH_FILE *dir, const char *name, size_t length, NH_FILE *out)
{ (void)dir; (void)name; (void)length; (void)out; return NH_UNSUPPORTED; }
#endif

#ifndef CONFIG_FS_EROFS
NH_STATUS NhErofsOpen(NH_FS *fs) { (void)fs; return NH_UNSUPPORTED; }
NH_STATUS NhErofsInode(NH_FS *fs, uint64_t id, NH_FILE *file)
{ (void)fs; (void)id; (void)file; return NH_UNSUPPORTED; }
NH_STATUS NhErofsRead(NH_FILE *file, uint64_t offset, void *buffer, size_t bytes)
{ (void)file; (void)offset; (void)buffer; (void)bytes; return NH_UNSUPPORTED; }
#endif
