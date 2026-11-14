# GpuColorLite v1.0.0

轻量级 NVIDIA 显卡 RGB 灯效控制器 —— Direct2D 全自绘界面，单文件零依赖。

---

## 亮点

- **单文件 358 KB** — 静态链接 CRT，仅依赖系统自带的 `d2d1` / `dwrite`
- **全自绘 UI** — 色相环 + 饱和度/明度方块，支持 HEX / R / G / B 精确输入
- **跟随系统深浅色** — 实时响应主题切换，无需重启
- **无边框窗口** — 保留 DWM 阴影、圆角与系统开关动画
- **状态持久化** — 注册表保存 HSV + RGB，灰色/黑色也能精确恢复
- **可集成** — 额外提供 DLL（C ABI），可嵌入其他程序

---

## 下载

| 文件 | 大小 | 说明 |
|---|---|---|
| `GpuColorLite.exe` | 358 KB | 主程序，双击即用 |
| `GpuColorLite.dll` | 278 KB | 集成用动态库 |
| `GpuColorLite.lib` / `.h` | — | 导入库与 C 头文件 |

**SHA256 校验**

```
B64D4527338C1F15E521F2F10B1E81C25C67659B0B591D44E7D55625B813D11A  GpuColorLite.exe
1B2C3AD6CA295BE1136419F29BB6FD7A0F450F4D0300981633D14EFA34D8BBCB  GpuColorLite.dll
```

---

## 使用

**图形界面**

拖动色相环改变色相，拖动中心方块调整饱和度与明度；或直接点击 `HEX` / `R` / `G` / `B` 输入框键入数值后回车。松开鼠标或回车即应用到显卡并保存。

右上角四个按钮：**关于** / **GitHub** / **置顶** / **关闭**

**命令行**

```bat
GpuColorLite.exe 255 0 0          :: 直接设置颜色（0-255）
GpuColorLite.exe --hex FF0000     :: 十六进制设置
GpuColorLite.exe --off            :: 关闭灯效
GpuColorLite.exe --shot out.png   :: 离屏渲染一帧 PNG
GpuColorLite.exe --light / --dark :: 强制指定主题
```

**DLL 集成**

```c
#include "GpuColorLite.h"

if (GclSetColor(255, 0, 0) != 0) {
    printf("失败: %s\n", GclLastError());
}
```

导出接口：`GclSetColor` / `GclSaveHsv` / `GclLoadHsv` / `GclSaveRgb` / `GclLoadRgb` / `GclLastError` / `GclVersion`，全部 `stdcall`，返回 `0` 表示成功。

---

## ⚠️ 兼容性说明

**本版本仅在以下单一配置上实测通过：**

| 项目 | 配置 |
|---|---|
| 操作系统 | Windows 11 专业版 25H2（Build 26200） |
| 显卡 | 七彩虹 iGame GeForce RTX 5060 Ultra —— **侧边灯条** |
| 显卡驱动 | 32.0.16.1664 |

灯效通过 NVAPI 的 I2C 接口向显卡写入**厂商私有报文**：

```
0xAA 0xEF 0x12 0x03 0x01 0xFF R G B + 16 位校验
I2C 设备地址 0xC2
```

这套格式并非公开标准，而是从上述型号逆向实测得出。

**其他品牌、其他型号的显卡均未做任何测试。** 即便同为 NVIDIA GPU，不同厂商（华硕 / 微星 / 技嘉 / 影驰 / 索泰等）乃至同厂商不同系列，其灯效控制器、I2C 地址与报文格式都可能不同，直接使用可能出现：

- **无效果** — 写入返回成功但灯不亮
- **颜色错误** — 通道顺序或位深不一致
- **意外行为** — 极少数情况下可能影响同一 I2C 总线上的其他设备

使用前请自行评估风险。如果你在其他型号上测试成功，欢迎提交 Issue 说明型号与结果，帮助完善兼容性列表。

---

## 已知限制

- 仅支持 NVIDIA 显卡，需要驱动提供的 `nvapi64.dll`
- 若 iGame Center / iGC.Lite 等工具正在占用 I2C，写入会失败，关闭后重试
- 开关动画由系统 DWM 提供，需系统「动画效果」开启

## 系统要求

| 项目 | 要求 |
|---|---|
| 操作系统 | Windows 10 1809+ / Windows 11 |
| 显卡 | NVIDIA（需 `nvapi64.dll`） |
| 运行时 | 无额外依赖 |

---

## 从源码构建

需要 Visual Studio 2022 Build Tools（MSVC v143 + Windows SDK 10）。

```bat
build.bat        :: -> output\GpuColorLite.exe
build_dll.bat    :: -> output\GpuColorLite.dll + .lib + .h
```

---

**完整变更**：首次发布。

**许可证**：[MIT](LICENSE)
