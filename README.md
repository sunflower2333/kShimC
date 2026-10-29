# kShim

kShimC(Kernel Shim revision C) is an In-Kernel BootLoader, which is one implementation of the generic Shim runtime protocol used by
[MultiBootKernelPatcher](https://github.com/Project-Aloha/DualBootKernelPatcher/).

- Inspired by shim from [SurfaceDuoDualBootKernelPatcher](https://github.com/WOA-Project/SurfaceDuoDualBootKernelImagePatcher).

# How it works
See my [blog](https://kancy.life/2024/11/01/Kshim1/)

# License
kShimC is licensed under the [MIT License](LICENSE), the same license as
[CrDK](https://github.com/Project-Aloha/CrDK).

Third-party code keeps its own license: the submodules (CrDK, picolibc, LVGL),
libfdt, and the Noto Sans CJK font under `user/ui/fonts/` (SIL Open Font
License, see `OFL.txt`).

The optional EROFS reader (`CONFIG_FS_EROFS`, off by default) is CrDK's
GPL-2.0-or-later `NhErofs.c`. A binary built with it enabled must be
distributed under the GPL.
