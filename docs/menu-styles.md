# LVGL 启动菜单风格

## 本轮范围

新增十个现代预设：FLUENT、MATERIAL3、SURFACE、CUPERTINO、GLASS、AURORA、
SOFT_UI、BENTO、NEON、NORD。Surface 包含在十个之内。
不增加 LVGL Default / Simple / Mono，也不增加 Win98、Retro 或 Material 2。
既有 FLAT、CLASSIC、CARDS、TERMINAL、MINIMAL、HIGH_CONTRAST 保留兼容；
旧配置的默认选择仍为 FLAT。每次固件构建选择一个完整预设，不提供运行时换肤。
本轮每款采用表中固定明暗方案，并非每款均有 Light/Dark 两套变体。

| 预设 | 明暗 | 形状、层次和状态差异 |
| --- | --- | --- |
| FLUENT | 浅色 | 中性分层面板、左侧蓝色焦点标记、小圆角、轻面板阴影 |
| MATERIAL3 | 浅色 | Material 3 色调容器、大圆角选中面、紫色胶囊 Boot、独立按压色 |
| SURFACE | 浅色 | Surface UEFI-inspired 宽屏双栏：左侧真实启动项、右侧选中项与 Boot；窄屏单栏 |
| CUPERTINO | 浅色 | 圆角外分组、方形列表行和底部分隔线、蓝色主按钮 |
| GLASS | 深色 | 静态渐变背景、半透明玻璃感面板、细亮边与有限阴影 |
| AURORA | 深色 | 青紫背景与选中面渐变、分层深色面板、薄荷色胶囊按钮 |
| SOFT_UI | 浅色 | 同色表面、白色亮边、有限下阴影，按压时阴影收缩，独立焦点轮廓 |
| BENTO | 浅色 | 带编号双列卡片、行优先按键顺序、窄屏自动单列 |
| NEON | 深色 | 青色描边和仅焦点项的小范围发光，不使用持续装饰动画 |
| NORD | 深色 | 冷灰层次、青色焦点标记、小圆角、克制的状态位移 |

这些是适合裸机 LVGL 的自定义设计实现，不是 Windows、Android 或 Apple UI
工具包的移植或完整规范认证。Fluent 不调用 Windows Mica API；Glass 不做实时
背景模糊；Soft UI 是亮边与单层阴影实现，不声称复刻双向光照。Material 3
使用预置色板与按压状态，不依赖 Android 动态取色服务，也不实现涟漪 shader。
Surface 不显示虚假的设备信息或未实现的 UEFI 开关，不附带 Microsoft 标志。

## 选择与编译

在已有固件构建目录运行：

```sh
cmake --build build --target menuconfig
```

进入 `Framebuffer and UI -> Boot menu style`，选择并保存，再构建：

```sh
cmake --build build --parallel
```

例如 Material 3 对应 `CONFIG_KSHIM_MENU_STYLE_MATERIAL3=y`，Surface 对应
`CONFIG_KSHIM_MENU_STYLE_SURFACE=y`。它们和旧预设属于同一个互斥 choice。
修改 defconfig 时关闭其他风格，不要同时追加多个 `=y`。
新增现代选项定义位于 `src/ui/styles/Kconfig`。

主机测试参数与固件 Kconfig 分开：

```sh
cmake -S tests -B build-menu -DKSHIM_TEST_MENU_STYLE=MATERIAL3
cmake --build build-menu --parallel 2 --target \
    lvgl_test lvgl_hdk_test menu_style_render_test menu_modern_test ui_preview_test
ctest --test-dir build-menu --output-on-failure \
    -R '^(lvgl_test|lvgl_hdk_test|menu_style_render_test|menu_modern_test)$'
mkdir -p previews
./build-menu/ui_preview_test previews
python3 tools/preview_manifest.py previews
```

## 实现与资源边界

`src/ui/menu_style.h` 保持旧色板并定义可选的现代效果字段。
`src/ui/styles/*.h` 分别保存十个独立预设；只编入当前选择。
`src/ui/menu_style_render.h` 应用渐变、阴影、分隔线、焦点和按压状态。
`src/ui/lvgl_port.c` 共用控件树与事件回调，不复制启动状态机。

Surface 在宽度至少 720 且横屏时采用双栏；Bento 在面板至少 560 像素宽、
屏幕至少 320 像素高时采用双列。布局会随动态菜单重建重新计算，窄屏自动回落。
菜单行触摸只选中，固定 Boot 按钮才确认；触摸取消仍不会启动镜像。
中文字体、49 项上限、按键映射、倒计时和 UEFI 启动接口继续使用原有路径。

新预设全部不划分动态背景纹理 scratch；背景渐变直接由 LVGL 绘制。
FLAT 仍按原逻辑使用两张 256*256*4 字节预算纹理。新预设少占用的默认
scratch 预算是 512 KiB，不代表固件镜像缩小 512 KiB。
小范围阴影和透明度仍会产生软件绘制成本；未在真机测量帧时间前不承诺帧率。
极小屏幕提供裁剪/滚动和内存安全退化，不代表能完整显示全部标题与长标签。

## 验证和真实预览

```sh
python3 tools/test_menu_styles.py --cc gcc --kconfig
python3 tools/test_menu_styles.py --cc clang
```

轻量检查覆盖 16 个预设、120 组非法双选、隐式/显式 FLAT、0/1 宏、
圆角边界、真实 Kconfig 生成头，以及现代色板的合成透明度、渐变采样和按压态
文字对比度（阈值 4.5:1；不等同于整体无障碍认证）。

CI 对 16 款新旧预设运行带 ASan/UBSan 的真实 LVGL 回归，并为十款新主题
分别交叉编译 ARM64 runtime。`menu_modern_test` 检查 1/2/3/49 项、长标签、
横竖屏布局、触摸确认/取消和帧缓冲哨兵。旧输入及字体测试不被现代测试替代。
FLAT 另与上一版渲染器逐字节比较两种分辨率的实际 PPM 帧。

每个 `menu-STYLE` Actions artifact 包含真实 LVGL 绘制的 PNG、索引 HTML、
图像 SHA256 清单和测试日志，保留 14 天。PNG 只转换现有 PPM，不重新绘制 UI。
两种预览尺寸是 1080x2340 与 1920x1080，每种包含入场、就绪、焦点和确认阶段。
是否通过应以对应提交的 CI 结果为准；交叉编译不等同于 QEMU 或真实设备验收。

设计参考：
- Fluent 2 material: https://fluent2.microsoft.design/material
- Fluent 2 elevation: https://fluent2.microsoft.design/elevation/
- Material 3 color roles: https://m3.material.io/styles/color/the-color-system
- Surface UEFI: https://learn.microsoft.com/en-us/surface/manage-surface-uefi-settings
