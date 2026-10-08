# LVGL 启动菜单风格

本功能改变设备上的 LVGL 启动菜单，不改变开发主机上 Python kconfiglib
`menuconfig` 的配色。旧 `.config` 和 defconfig 不需要新增字段，默认仍为 FLAT。
每个固件在编译时选择一个预设；本次不提供运行时主题切换。

## 风格计划与实现范围

| 预设 | 面板与菜单项 | 焦点与启动按钮 | 背景 |
| --- | --- | --- | --- |
| `FLAT` | 原有靛蓝半透明无边框面板与圆角行 | 蓝色选中行，浅色 Boot 按钮 | 原有动态渐变 |
| `CLASSIC` | 深蓝不透明面板、方角细框行、面板外框 | 蓝色选中行，浅灰 Boot 按钮 | 静态 |
| `CARDS` | 浅色面板、间距较大的白色圆角卡片 | 蓝色描边与浅蓝选中面，蓝色 Boot 按钮 | 静态 |
| `TERMINAL` | 近黑背景、绿色方角框线 | 绿色反色选中行与启动按钮 | 静态 |
| `MINIMAL` | 黑色无外框面板、方角行 | 白色左侧焦点标记，白色 Boot 按钮 | 静态 |
| `HIGH_CONTRAST` | 黑底、白色较粗方框，无半透明表面 | 黑白反色选中行，白色 Boot 按钮 | 静态 |

TERMINAL 是终端视觉风格，仍使用既有中文字体，不额外引入等宽字体。
所有风格保留原有入场、焦点、按压和确认动效；“静态”仅指背景不再逐帧生成。
不新增模糊、纹理图片、阴影渲染路径或外部资源依赖。

## 选择固件风格

在已有构建目录运行：

```sh
cmake --build build --target menuconfig
```

进入 `Framebuffer and UI -> Boot menu style`，选择风格并保存，再重新构建：

```sh
cmake --build build --parallel
```

对应 Kconfig 符号为 `CONFIG_KSHIM_MENU_STYLE_FLAT`、`_CLASSIC`、`_CARDS`、
`_TERMINAL`、`_MINIMAL`、`_HIGH_CONTRAST`。它们属于同一个互斥 choice。
例如 CARDS 的已保存配置包含 `CONFIG_KSHIM_MENU_STYLE_CARDS=y`。
修改 defconfig 时应删除或关闭其他风格符号，而不是同时追加多个 `=y`。
禁用 `CONFIG_KSHIM_LVGL` 时，风格 choice 自动不可用。

`KSHIM_TEST_MENU_STYLE` 是下面的**主机测试参数**，不是顶层固件 CMake 参数；
不要用顶层 `cmake -DKSHIM_TEST_MENU_STYLE=...` 代替 Kconfig 配置。

## 实现边界

`src/ui/menu_style.h` 保存不依赖 LVGL 的编译期预设数据和圆角计算。
`src/ui/lvgl_port.c` 统一把预设应用到屏幕、面板、行、焦点、滚动条和 Boot 按钮。
只编入选中的预设，不复制菜单状态机，也不增加动态主题对象。

面板框线使用 outline，避免改变内容区与固定 Boot 按钮的坐标。
行边框宽度在选中前后固定；新增上下边框对应减少行内垂直留白。
菜单重建时重新应用同一风格，包括方角面板，避免重建后恢复 FLAT 圆角。

五个静态预设不从 scratch 划分两张背景纹理，字体直接使用可用 scratch。
默认纹理预算为 `2 * 256 * 256 * 4 = 524288` 字节，即 512 KiB；
这是少占用的 scratch，不是固件镜像大小减少 512 KiB。
FLAT 的纹理布局和刷新逻辑保持原样。字体不足时仍走原有 Latin 回退。

物理按键、触摸坐标、滚动、倒计时、49 项动态菜单、启动镜像与 UEFI 调用不改动。
触摸菜单行只改变焦点；触摸固定 Boot 按钮才确认，取消触摸仍不启动镜像。

## 验证

不需要 LVGL 子模块或目标硬件的预设契约测试：

```sh
python3 tools/test_menu_styles.py --cc gcc
python3 tools/test_menu_styles.py --cc clang
# 安装项目使用的 kconfiglib 后，验证真实 choice 和生成的配置头文件：
python3 tools/test_menu_styles.py --cc gcc --kconfig
```

检查隐式/显式 FLAT、六种颜色表、0/1 宏、全部 15 组非法双选、
0..4096 高度和 UINT32_MAX 的圆角边界，以及新增不透明风格的文字对比度。
对比度检查不代表对动态透明 FLAT 或整套界面作无障碍认证。

使用仓库固定版本的 LVGL 和 CrDK 子模块运行真实渲染测试：

```sh
 git submodule update --init --recursive
cmake -S tests -B build-menu -DKSHIM_TEST_MENU_STYLE=CARDS
cmake --build build-menu --parallel 2 --target \
    lvgl_test lvgl_hdk_test menu_style_render_test
ctest --test-dir build-menu --output-on-failure \
    -R '^(lvgl_test|lvgl_hdk_test|menu_style_render_test)$'
```

主机测试需要现有测试工程的依赖，包括 C/C++ 工具链、CMake 和 dtc。
`menu_style_render_test` 检查实际 LVGL 状态颜色、方角/圆角、框线、按压状态、
静态/动态背景、字体专用 scratch、横竖屏、菜单重建和帧缓冲哨兵。
既有 `lvgl_test`、`lvgl_hdk_test` 同时在该风格下运行，继续检查按键、触摸、
取消确认、中文字体和六种帧缓冲格式。

`.github/workflows/menu-styles.yml` 为六种预设分别配置主机回归任务，
另设 GCC/Clang 和真实 Kconfig 契约任务。CI 与真实设备验收是不同层级；
发布前仍需在目标设备上检查触控、中文长标签、低分辨率和面板边缘显示效果。

已有 `ui_preview_test` 使用相同的 `KSHIM_TEST_MENU_STYLE` 参数，
因此不需要另写一个与真实渲染器脱离的主题预览工具。
