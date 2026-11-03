# SPDX-License-Identifier: MIT
#
# I/O stack host tests (docs/io-stack.md). Included at the end of
# tests/CMakeLists.txt. Each owner adds its register-model tests to its own
# fragment under tests/io/ with kshim_test(); shared helpers available here:
#   KSHIM_ROOT, KSHIM_TEST_INCLUDE_DIRS, kshim_dt (libfdt + kernel/dt core),
#   crdk (the materialized CrDK library), DTC_EXECUTABLE, CRDK_ROOT.
# Fixtures go under tests/fixtures/io/ with a note on how they were made.

# Architect-owned: header contracts and the I/O core (completions, hooks).
kshim_test(io_headers_test io/io_headers_test.c)
target_link_libraries(io_headers_test PRIVATE crdk)
kshim_test(io_core_test io/io_core_test.c "${KSHIM_ROOT}/kernel/core/io.c")
target_include_directories(io_core_test PRIVATE "${KSHIM_ROOT}/tests/include")

foreach(fragment platform qup pci usb usbhid touch touch_goodix)
    include("${CMAKE_CURRENT_LIST_DIR}/io/${fragment}.cmake")
endforeach()
