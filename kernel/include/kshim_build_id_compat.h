/* SPDX-License-Identifier: MIT */
#pragma once
/* KSHIM_BUILD_ID comes from the generated kshim_build_id.h (CMakeLists.txt);
 * builds without it (host tests) get a placeholder. */
#if __has_include(<kshim_build_id.h>)
#include <kshim_build_id.h>
#endif
#ifndef KSHIM_BUILD_ID
#define KSHIM_BUILD_ID "unversioned"
#endif
