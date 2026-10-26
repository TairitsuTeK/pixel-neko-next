# Pixel Neko Next 🐾

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Architecture](https://img.shields.io/badge/Arch-x86--32-blue.svg)](https://en.wikipedia.org/wiki/IA-32)
[![Boot](https://img.shields.io/badge/Boot-Multiboot%20%2F%20GRUB-green.svg)](https://www.gnu.org/software/grub/)
[![Version](https://img.shields.io/badge/Version-C2.0%20(Build%202012.200)-brightgreen.svg)]()

**Pixel Neko Next** 是一个轻量级、开箱即用的 x86-32 位原生微桌面操作系统（Bare-metal GUI OS）。系统完全基于 C 语言与 NASM 汇编从零编写，具备高分辨率图形桌面、完整的窗口管理器、Windows 驱动器盘符映射、ISO9660 光盘文件系统、FAT12/16/32 文件系统、原生软驱驱动、USB 2.0 键鼠输入、丰富的内置应用程序以及极致复古的经典开机与关机停机体验。

---

## 🌟 核心特性

- **🎬 经典复古开机动画（Boot Splash Animation）**
  - **Happy Neko（阶段 1）**：致敬经典 Mac OS 9 "Happy Mac"，在 50% 棋盘灰度背景上呈现复古一体机与可爱的 Happy Neko 像素猫笑脸，并伴随 PC 扬声器 4 音符升调开机和弦。 
  - **欢迎窗口 & 扩展游行（阶段 2）**：融合 Windows 3.1 徽标与 Mac OS 9 风格的 3D 浮雕视窗；搭载经典的 **45° 动态斜向条纹进度条（Barber-Pole Striped Progress Bar）** 与屏幕底部 **Extension Parade** 硬件图标流（CPU、内存、显卡、键盘、鼠标、硬盘、文件卷、桌面环境）。
  - **平滑缩放过渡（阶段 3）**：通过 Finder 风格经典虚线缩放矩形动画平滑切入桌面，启动期间随时按 `Esc` / `Space` / `Enter` 或点击鼠标即可立即跳过动画直达桌面。
- **💻 全面而精准的 CPU 识别（完美兼容奔腾 Pro 及以上全系处理器）**
  - **高版本 CPU 品牌字符串优先**：自动安全读取 CPUID 扩展叶（0x80000002..0x80000004），针对现代 Core i3/i5/i7/i9、Xeon、AMD Ryzen 等输出规范化品牌全称。
  - **经典处理器三级匹配策略**：对不支持扩展品牌串的早期 CPU（如奔腾 Pro、奔腾 II、早期奔腾 III），结合 Intel Brand Index 及精确的 Family / Model 查表策略，完美呈现 `Intel Pentium Pro`、`Intel Pentium II (Klamath)`、`Intel Pentium III (Katmai)` 等经典型号。
  - **架构级安全边界探测**：严格遵守 x86 规范对叶片范围进行校验，确保在奔腾 Pro、各种虚拟机（QEMU、VMware、VirtualBox、Bochs）及实机硬件上均稳定无虞。
- **🔌 完善的电源管理与经典安全停机画面（针对 ASUS KN97 等奔腾 Pro 机型）**
  - **阶梯式关机策略**：依序执行 ACPI S5 状态切换、标准 APM 1.2 握手切断电源、模拟器 I/O 关机端口。
  - **经典安全停机画面（Safe to Turn Off）**：针对 Baby AT 架构物理机械开关主板（如奔腾 Pro 华硕 KN97），关机后呈现 Windows 95/98 经典全屏 *"It is now safe to turn off your computer."*，并永久挂起 CPU，绝不静默返回保护模式桌面。
  - **可靠硬件重启**：通过 8042 键盘控制器复位脉冲（0xFE）与 PCI 复位向量（0xCF9）实现秒级干净重启。
- **⌨️ USB 2.0 主机控制器与 HID 键鼠输入**
  - **统一 USB 核心层**：提供 Host Controller Interface 抽象（控制传输 / 中断 IN / 批量传输 / 根端口复位）、标准描述符解析与 `GET_DESCRIPTOR`、`SET_ADDRESS`、`SET_CONFIGURATION` 完整枚举流程，最多管理 4 个控制器与 8 个设备。
  - **UHCI（USB 1.1）**：自动探测 PCI I/O BAR，使用帧列表 + QH/TD 调度完成全速 / 低速设备枚举，支持 2 个根端口复位与低速设备识别。
  - **EHCI（USB 2.0）**：解析 MMIO 能力寄存器与运行寄存器，使用 qTD 完成高速传输，支持最多 8 个根端口；遇到 64 位 EHCI 时明确记录不支持。
  - **HID 启动协议键鼠**：USB 键盘 / 鼠标报告经 HID Usage → PS/2 Set-1 扫描码转换后注入现有输入管线，可与 PS/2 键鼠同时使用，最多支持 4 个 HID 设备。
  - **安全降级与诊断**：OHCI / xHCI 自动识别并记录"暂不支持"，不影响 PS/2 输入；枚举与驱动状态通过 COM1 串口日志（`[USB ]`、`[UHCI]`、`[EHCI]`、`[HID ]`）输出。
- **💾 全新统一虚拟文件系统（VFS）与存储子系统**
  - **Windows 对齐盘符体系**：A: 与 B: 专给软驱预留；硬盘数据分区从 C: 开始依次分配；光驱紧随已分配硬盘分区之后（若无硬盘则光驱自动为 C: 盘）；自动隐藏 EFI 系统分区与扩展容器分区。
  - **CDFS 光盘驱动 (ISO9660 + Joliet)**：完整支持标准 ISO9660 与 Joliet 长文件名扩展，解决 CD/DVD 点开为空的问题，支持只读浏览与读取光盘文件。
  - **原生软盘驱动 (FDC)**：原生 82077AA / NEC765 控制器驱动 + ISA DMA 通道 2 传输，支持 1.44M/720K 软盘及 FAT12 格式。
  - **文件系统容错交互**：识别 NTFS、exFAT、Linux ext 等分区并分配盘符；双击不支持的分区时友好弹出 *"Cannot read partition"* 模态提示框。
  - **FAT12 / FAT16 / FAT32 文件系统**：完整保留记事本及文件管理器对可写磁盘分区的文件新建、编辑、写入保存及删除能力；对光盘等只读介质进行写保护拦截。
- **📱 丰富的内置应用程序**
  - **文本编辑器（Editor）**：支持文件打开、键盘光标导航、多行编辑与磁盘回写保存。
  - **计算器（Calculator）**：四则运算，拟物化 3D 浮雕按钮与即时计算。
  - **控制面板（Control Panel）**：动态分辨率切换（带 10 秒无操作安全回滚机制）、鼠标灵敏度调节、系统 CPU / 内存信息展示。
  - **文件管理器（File Manager）**：直观浏览驱动器分区目录，支持双击打开关联文件。
- **🛡️ 硬件级容错与系统崩溃视窗**
  - 内核发生异常或 Trap 时，直接在图形模式下渲染复古"系统错误"视窗，打印异常名称、EIP 寄存器及 CPU 状态，并支持按任意键安全复位重启。
  - 多级重启降级机制：依次尝试 8042 键盘控制器复位、PCI 0xCF9 端口复位及三重故障（Triple Fault）兜底。

---

## 📁 项目目录结构

```text
├── .github/              # CI/CD 工作流与 CODEOWNERS
├── boot/                 # GRUB Multiboot 协议与实模式蹦床汇编
│   ├── idt_asm.asm       # 中断与异常处理桩
│   ├── multiboot.asm     # Multiboot 头与内核入口
│   └── vbe.asm           # 实模式蹦床
├── kernel/               # 内核核心层
│   ├── cpu.c             # CPU 品牌字符串识别与奔腾 Pro+ 查表策略
│   ├── idt.c             # IDT 中断描述符表与图形崩溃处理
│   ├── kernel.c          # 内核主入口、显示设定与主事件泵
│   ├── mm.c              # 物理页框分配器（PMM Bitmap）
│   ├── panic.c           # Mac 炸弹崩溃视窗与异常现场转储
│   ├── power.c           # 阶梯式电源管理与停机画面
│   ├── splash.c          # Mac OS 9 & Win 3.1 风格开机动画
│   └── timer.c           # 统一 PIT 模式 2 计时器与延时驱动
├── drivers/              # 硬件与总线驱动
│   ├── ati_mach64.c      # ATI 3D Rage II+ / Mach64 专有驱动与 2D 加速
│   ├── gfx_accel.c       # 2D 硬件加速统一抽象与平滑降级调度
│   ├── blkdev.c          # 块设备抽象层
│   ├── disk.c            # ATA / ATAPI 存储驱动
│   ├── fdc.c             # 原生软驱控制器驱动 (FDC 82077AA)
│   ├── usb_core.c        # USB 2.0 核心层、描述符解析与设备枚举
│   ├── usb_uhci.c        # UHCI (USB 1.1 全速/低速) 主机控制器驱动
│   ├── usb_ehci.c        # EHCI (USB 2.0 高速) 主机控制器驱动
│   ├── usb_hid.c         # HID 启动协议键盘/鼠标驱动
│   ├── isadma.c          # ISA DMA 控制器驱动
│   ├── font.c            # 基础字体渲染
│   ├── keyboard.c        # PS/2 键盘与按键缓冲
│   ├── mouse.c           # PS/2 鼠标驱动与超时保护
│   └── speaker.c         # PC 扬声器驱动与复古开机和弦
├── fs/                   # 文件系统实现
│   ├── fat.c             # FAT12 / FAT16 / FAT32 文件系统与空闲簇统计
│   ├── iso9660.c         # CDFS 光盘文件系统
│   ├── part.c            # MBR 分区表解析
│   └── vfs.c             # 统一虚拟文件系统与 Windows 驱动器盘符
├── wm/                   # 窗口管理器核心
│   └── wm.c              # 窗口层叠、桌面图案、菜单栏与图层混合
├── apps/                 # 内置桌面应用程序
│   ├── calc.c            # 简易计算器
│   ├── cpanel.c          # 控制面板（动态切分辨率、真彩限制、桌面图案）
│   ├── editor.c          # 文本编辑器与剪贴板
│   ├── filemanager.c     # 图形化文件管理器（容量与空闲空间统计）
│   ├── sysinfo.c         # 系统信息工具与全 PCI 总线设备浏览器
│   └── puzzle.c          # 经典 15 数字推盘小游戏
├── include/              # 全局公共头文件
│   └── *.h               # ati_mach64.h, gfx_accel.h, usb_core.h, usb_uhci.h, usb_ehci.h, usb_hid.h, panic.h, sysinfo.h, puzzle.h, etc.
├── scripts/              # 编译与构建辅助脚本
│   ├── build_counter.sh
│   └── build_number.txt
├── link_grub.ld          # 内核 ELF 链接脚本
├── Makefile              # 顶级 Makefile 构建规则 (-march=pentiumpro)
└── LICENSE               # MIT 开源协议
```

---

## 🛠️ 构建与运行环境

### 依赖项
- **GCC**（支持 `-m32`；Windows 下需 `i686-elf-gcc` 交叉工具链，Linux / MSYS2 / WSL2 可直接使用原生 `gcc`）
- **NASM**（汇编器）
- **GNU ld**（Windows 下对应 `i686-elf-ld`）
- **grub-mkrescue** 与 **xorriso**（用于生成可启动 ISO）
- **QEMU**（推荐 `qemu-system-i386` 用于本地模拟运行）与 **qemu-img**（`make run` 自动创建 `disk.img`）

> Windows 下 `make` 需要可用的 POSIX Shell（w64devkit 或 MSYS2 自带），因为构建计数器与文件清理依赖 `sh`；生成 ISO 还需要 `grub-mkrescue` 与 `xorriso`，建议在 MSYS2 或 WSL2 中完成。

### 编译与启动

1. **编译生成可启动 ISO 镜像**
   ```bash
   make
   ```
   编译完成后会在根目录生成固定命名的 `pixel_neko.iso`；内核与 GRUB 菜单内嵌的版本号取自 `include/version.h` 的 `KERNEL_BUILD`（当前为 `2012.200`）。

2. **在 QEMU 虚拟机中一键运行（ISO + 64MB 数据盘）**
   ```bash
   make run
   ```

3. **不生成 ISO，直接以 `-kernel` 启动内核 ELF**
   ```bash
   make run-kernel
   ```

4. **USB 输入自测模式**
   ```bash
   make run-usb
   ```
   自动附加 `piix3-usb-uhci` 控制器与 USB 键盘 / 鼠标，并将 `[USB ]`、`[UHCI]`、`[EHCI]`、`[HID ]` 枚举日志写入 `usb_log.txt`。

5. **GDB 远程调试模式**
   ```bash
   make debug
   ```

6. **清理构建生成物**
   ```bash
   make clean
   ```

---

## 💾 Build 2012.200 更新特性
- **⌨️ 全新 USB 2.0 子系统与 HID 键鼠输入**：
  - 新增 USB 核心层 `drivers/usb_core.c`，提供 Host Controller Interface 抽象、标准描述符（Device / Config / Interface / Endpoint / HID）解析与 `GET_DESCRIPTOR`、`SET_ADDRESS`、`SET_CONFIGURATION` 完整枚举流程；最多支持 4 个控制器与 8 个设备；
  - 新增 UHCI（USB 1.1 全速 / 低速）驱动 `drivers/usb_uhci.c`：PCI I/O BAR 探测、帧列表与 QH/TD 调度、控制 / 中断 IN / 批量传输、2 个根端口复位及低速设备识别；
  - 新增 EHCI（USB 2.0 高速）驱动 `drivers/usb_ehci.c`：MMIO 能力 / 运行寄存器解析、qTD 传输、最多 8 个根端口，64 位 EHCI 明确记录不支持；
  - 新增 HID 类驱动 `drivers/usb_hid.c`：请求 Boot Protocol，将 HID Usage 映射为 PS/2 Set-1 扫描码并注入既有键盘 / 鼠标输入管线，最多 4 个 HID 设备，可与 PS/2 键鼠同时工作；
  - `usb_init()` 在开机动画结束后扫描 PCI 类代码 `0C/03` 的控制器，`usb_poll()` 并入桌面主循环；OHCI / xHCI 自动识别并记录“暂不支持”，不影响原有 PS/2 输入。
- **🧰 构建系统与工具链适配**：
  - Windows 下自动使用 `i686-elf-` 交叉工具链（可通过 `TOOLCHAIN_PREFIX` 覆盖），Linux 保持原生 `gcc` / `ld`；
  - ISO 输出改为固定文件名 `pixel_neko.iso`，不再随版本号变化；版本头生成物统一放入 `build/generated`，`include/version.h` 不再随每次编译被重写；
  - 新增 `make run-kernel`（直接启动 ELF）、`make run-usb`（USB 键鼠自测 + 串口日志）目标，`make run` / `make debug` 自动通过 `qemu-img` 创建 64MB `disk.img`；
  - 头文件依赖改为递归发现并排除自动生成的版本头，避免仅版本号变化触发全量重编译。
- **🏷️ 版本号同步**：内核、开机画面、系统关于视窗、README 与 CI 发布说明统一为 `C2.0 Build 2012.200`。

---

## 🚀 Build 2010.015 更新特性
- **📂 文件管理器完整目录浏览与右侧经典垂直滚动条（Scrollbar）**：
  - **目录容量突破与动态视窗渲染**：将文件管理器单目录最大承载量由原本的 64 个扩充至 512 个（`MAX_FM_FILES 512`）；移除了每页最多固定只显示 10 个项目的生硬限制（`max_display > 10`），完全依据视窗当前拉伸高度自适应计算最大可见行数。
  - **经典复古 3D 垂直滚动条**：在文件管理器视窗右侧构建 Win95 / Mac OS 经典立体滚动条，由顶部向上箭头按钮（▲）、底部向下箭头按钮（▼）、点阵抖动灰阶轨道（Track）以及带防滑抓握条纹（Grip lines）的立体滑块（Thumb）组成。
  - **全功能鼠标交互体系**：
    - 点击顶部/底部箭头按钮进行单行平滑步进；
    - 点击滑块上/下方轨道区域执行翻页跳转（Page Up / Page Down）；
    - 鼠标左键按住滑块自由纵向拖拽，实时精准映射至整个文件列表的任意偏移量；鼠标在视窗外部释放时平滑解脱拖拽锁定。
  - **完备的键盘导航快捷支持**：
    - 方向键 `Up` / `Down`：步进选中项，当选区到达视窗边界时自动联动垂直滚动条；
    - `Page Up` / `Page Down`：按整屏快速翻页；
    - `Home` / `End`：一键直达文件列表顶部与尾部；
    - `Enter`：直接打开/运行当前选中项；
    - `Backspace`：返回上一级目录。
  - **文件条目与列宽自适应排版**：全面调整文件名、驱动器类型、总容量/可用容量及文件大小列的右对齐基准至滚动条左缘，杜绝长文件名及容量数值被滚动条遮挡，保证排版清晰优雅。

---

## 🚀 Build 2010.014 更新特性
- **🔊 CMI8738 物理实体声卡物理 DAC 输出与通道方向致命修复**：
  - **CH0 通道方向与暂停标志更正（FUNCTRL0）**：彻底清除 `CM_CHADC0`（Bit 0 = 0，强制配置为播放/DAC模式而非录音/ADC模式），修复因位定义错位导致实卡冷启动进入 ADC 捕获模式而实体 DAC 无法接收 PCM 数据出声的致命硬伤（86Box 模拟器因忽略录音标志强制播放而有声，实卡则彻底静音）。
  - **PAUSE0 暂停标志位校准**：修正 `CM_PAUSE0` 掩码（Bit 2，0x04），修复以往误操作 Bit 4 导致 DMA 启动时无法真正清除硬件暂停状态的问题。
  - **MISC_CTRL 寄存器纯净配置**：清除导致部分物理 CMI8738 声卡内部 DAC 路由异常与测试信号旁路的测试监听位（Bits 29:28）及 Double DAC 冲突位（`CM_ENDBDAC`）；保留四声道镜像（`CM_N4SPK3D`），确保 Front Out（绿色孔）与 Rear Out 同时畅响。
  - **PCI 全总线/设备/功能深度枚举**：将声卡 PCI 扫描扩展至全部 256 个总线、32 个设备和 8 个功能，彻底解决 Intel H55（如 ASUS P7H55-M LE）等现代南桥下扩展槽位于高总线号或多功能桥下时探测不到独立声卡的隐患。
  - **SB16 慢速混音器 I/O 时延与全通道解闭**：在混音器读写指令间引入 `io_wait()` 端口 0x80 硬件时延，消除现代 3GHz+ 高主频 CPU 突发 I/O 导致物理声卡丢失混音设置；清除 `0xF0` 扬声器与电话静音（Bits 3 & 4），激活 `0x3C` 全通道输出开关。
  - **硬件分频比规范化**：校准 48000Hz 对应分频代码为 7（原代码误传 4 导致 48k 实际被配置为 8000Hz）。

---

## 🚀 Build 2010.013 更新特性
- **🔊 CMI8738 物理实体声卡绿色音频孔（Front Out）发声与硬件寄存器修正**：
  - **混音器端口反转修复**：更正 SB16 兼容混音器地址端口（0x23）与数据端口（0x22）的颠倒定义，解决实卡开机音量默认被硬件置零（静音）且音量调节无法写入硬件的致命问题。
  - **PCM 硬件静音位彻底解除**：在 `CM_REG_MIXER1` (0x24) 中主动清除 `CM_WSMUTE` (Bit 6, 0x40)，打通物理 DAC 的 PCM 模拟输出通路。
  - **声道路由与前置放大增强**：在 `CM_REG_MISC_CTRL` (0x18) 中启用 Double DAC (`CM_ENDBDAC`)、四声道同步复制 (`CM_N4SPK3D`) 以及模拟输出 +6dB 前级放大 (`CM_VMGAIN`)，强制将 CH0 映射至绿色 Line-Out/耳机孔（`CM_XCHGDAC = 0`），并关闭节电休眠（`CM_PWD = 0`）。
  - **PCI 总线主控请求（BREQ）激活**：在 `FUNCTRL1` (0x04) 中置位 `CM_BREQ` (Bit 4)，使物理声卡在实机 PCI 总线上能够正常发起 DMA 内存拉取。
  - **扬声器开关解闭**：在混音器 0xF0 扩展寄存器中清除 `CM_VSPKM`，在 0x3C 输出开关中开启全通道通路，保证耳机与桌面音箱纯净出声。

---

## 🚀 Build 2010.010 更新特性
- **📁 FAT16/FAT32 长文件名（LFN / VFAT）全栈支持**：实现 VFAT 0x0F 长目录项双向解析与校验合成，突破 8.3 格式限制，文件管理器与应用全线支持长文件名无缝浏览与打开。
- **🎛️ CMI8738 声卡 48kHz 硬件 DAC 采样率同步**：修复 `FUNCTRL1` 中仅设置 `ASFC` 未配置 `DSFC` 导致 DAC 始终运行在 44.1kHz 的硬件分频抖动与卡顿缺陷，实现 48kHz MP3 / 96kHz 降采样极致流畅回放。
- **🔇 关机停机画面静音与 DMA 彻底停止**：在退出系统进入经典 Safe to Turn Off 画面及重启前立即切断声卡 DMA 环形缓冲与静音输出，彻底消除关机停机时的硬件残余卡碟循环循环。
- **💽 Intel H55 (ASUS P7H55-M LE) SATA / IDE Native PCI 驱动器自适应探测**：新增 PCI 存储控制器（Class 0101）全面枚举与 BAR0~BAR3 Native PCI I/O 端口识别，解决现代与近现代主板兼容 IDE 模式下找不到硬盘与光驱的问题。
- **🏷️ 开机画面与系统内核版本号迭代同步**：开机 Boot Splash 及系统关于视窗全面联动动态版本号，正式迭代至 `Build 2010.010`。

---

## 💾 Build 2012.104 更新特性
- **🛠️ HDA BDL DMA 布局修复**：
  - HDA BDL Entry 正式修正为 16 字节布局：`ADDR_LOW / ADDR_HIGH / LENGTH / FLAGS`；
  - 修复旧 12 字节布局导致 VMware 把 Period Length 误读为地址高 32 位的问题；
  - 修复 VMware 日志中的 `Invalid BDL entry address: 400000802000`；
  - HDA Stream 现在能够正确启动 DMA，LPIB 与播放进度可正常推进。

---

## 💾 Build 2012.103 更新特性
- **🔊 HDAudio 上层驱动选择修复**：
  - 统一 `hdaudio_init()` 的返回约定为 `0 = 成功`、负数 = 失败；
  - 修复 HDA 已完成 Codec 路由并显示 `48kHz/16-bit stereo ready`，却因驱动注册层判断 `init() == 0` 失败而没有设置活动 Audio HAL 的问题；
  - Audio Engine 现在会正确绑定 HDA 后端，NekoPlayer 不再错误显示 `NO AUDIO HARDWARE`；
  - 保留 CT4810、CMI8738 的后备选择路径。

---

## 💾 Build 2012.102 更新特性
- **🔊 VMware HDA Codec 输出路径修复**：
  - 修正 HDA Connection List 短格式解析：短格式每个 32-bit Response 包含 4 个 8-bit Node ID；
  - 修正 16-bit 长格式 Connection List 的查询步进和双 Node 解包；
  - 修复 Mixer/Selector/Pin 路径中 Node ID 拼接错误导致的 `No routable HDA Codec`；
  - 修正后可继续解析 VMware `15AD:1977` 的 Audio Converter → Mixer/Selector → Pin 输出路径。

---

## 💾 Build 2012.101 更新特性
- **🔎 VMware HDAudio 识别修复**：
  - System Information 不再只显示前 24 个 PCI 设备，支持完整浏览所有枚举结果；
  - PCI 设备缓存扩大到 512 项，避免高版本 VMware 大量 PCIe Root Port 隐藏后续设备；
  - HDA 驱动除 PCI Class `04/03` 外，同时按 VMware `15AD:1977` 精确 Device ID 探测；
  - HDA 初始化失败时，System Information 直接显示 Controller reset、DMA、Codec 路由等具体状态，不再统一显示 `No PCI Sound Card`。

---

## 💾 Build 2012.100 更新特性
- **🔊 Intel HDAudio 与 VIA VT1708S**：
  - 新增 Intel HDA Controller reset、CORB/RIRB 命令通道、Codec 枚举、Widget 拓扑与输出路径解析；
  - 支持 VMware `15AD:1977` 和 Intel 5 Series/PCH HDA Controller；
  - 针对 ASUS P7H55-M LE 的 VIA VT1708S 识别 `1106:0397` 等 Codec 变体，动态选择 Line Out/Headphone；
  - 支持 44100Hz 与 48000Hz、16-bit Stereo、四 Period BDL、音量、Mute、Pause、Seek 和 Underrun 恢复；
  - 兼容 VIA `0x16` Mixer 的隐藏 DAC 路径，并在无可用 Codec/Pin 时回退到 CT4810/CMI8738。
- **🖥️ SVGA 3D-capable 配置兼容**：
  - 高版本 VMware 配置声明 `SVGA_CAP_3D` 时不再被视为不兼容设备；
  - 继续优先使用标准 SVGA FIFO 的 `UPDATE/RECT_FILL/RECT_COPY` 2D 路径；
  - SysInfo 与日志标记 `3D-capable profile: FIFO 2D, 2D-only mode`；
  - FIFO 超时、光标异常或 Surface 路径不可用时安全回退，不启用完整 3D 管线。

---

## 💾 Build 2012.001 更新特性
- **🖱️ VMware 启动画面冻结与鼠标不动修复**：
  - 暂时停用尚未通过真实 VMware 运行验证的 SVGA 硬件光标，改用已验证的软件光标，避免 cursor FIFO 命令影响后续 `UPDATE` 显示刷新；
  - 修正光标 mask 的 32 位 scanline 对齐布局，并加入 `MOVE_CURSOR` 命令支持，保留后续重新启用硬件光标的正确实现；
  - PS/2 键盘、鼠标增加带中断临界区保护的轮询兜底：即使 IRQ1/IRQ12 未触发，也不再出现鼠标完全无响应；
  - IRQ 基础设施、PCI/DMA/MMIO 和 VMware SVGA II FIFO 2D 加速继续保留。

---

## 💾 Build 2012.000 更新特性
- **🧱 P0 内核基础设施**：
  - 新增统一的 PCI 0..255 总线枚举、设备缓存、32/64 位 BAR 解码和显存控制器资源查询；
  - 新增可共享 IRQ 的注册/注销、PIC 屏蔽控制、BIOS 调用掩码保存恢复和 spurious IRQ 处理；
  - PIT、PS/2 键盘与鼠标改为真实硬件 IRQ 驱动，100Hz 系统 tick 不再依赖桌面主循环轮询；
  - 新增 DMA 连续缓冲区、MMIO 屏障、串口/内存环形日志和按优先级探测的驱动注册表；
  - CMI8738、CT4810/ES1371 的 DMA 缓冲区改由统一 DMA 层分配。
- **🖥️ P1 VMware SVGA II 2D 显卡**：
  - 支持 VMware SVGA II、Fusion 与 SVGA Adapter 的寄存器与能力探测，`800×600×32` 及动态 2D 模式切换；
  - 初始化 VMware FIFO，使用 `UPDATE`、`RECT_FILL`、`RECT_COPY` 完成 dirty-rectangle present 与 2D 加速命令；
  - 支持 16×16 硬件光标，FIFO 或光标不可用时自动回退到软件光标；
  - 保留线性 framebuffer、ATI Mach64 和标准 VBE 的安全回退路径；
  - 识别声明 `SVGA_CAP_3D` 的高版本 VMware 显卡配置，但系统仍只使用 2D 加速，不实现完整 3D 管线。

---

## 💾 Build 2011.110 更新特性
- **🎨 Mach64 32bpp 扫描行跨度修复**：
  - 修正 `CRTC_OFF_PITCH` 与 2D 引擎 `OFF_PITCH` 的单位错误：Mach64 使用每行 8 像素单元，而不是 32bpp 字节跨度除以 8；
  - 修复 32bpp 下每行地址跨越 4 倍、形成重复画面与黑色横带的问题；
  - 重新校准 32bpp 原生模式的 CRTC pitch、内存像素模式和 GUI 引擎格式。
- **🎼 Creative CT4810 / Ensoniq 兼容探测增强**：
  - 同时支持标准 `1274:1371` 与兼容变体 `1274:0371`；
  - PCI BAR0 不是 I/O 空间时，自动继续搜索 BAR0～BAR5 中真实 I/O BAR；
  - 保留 ES1371 AC’97、SRC 和 DAC2 DMA 初始化路径。

---

## 💾 Build 2011.102 更新特性
- **🎨 ATI Mach64 / 3D Rage II+DVD 32 位真彩模式**：
  - Mach64 VT2 与 Rage II+DVD 的 GPU/CRTC 原生支持 `ARGB8888`，但 PCBox 的 VT2 VBE BIOS 和 Rage II+ BIOS 只公开到 24bpp；
  - 优先使用 BIOS 直接提供的 32bpp VBE 模式；BIOS 未提供时，自动以同分辨率 24bpp 稳定时序初始化，再切换 Mach64 CRTC 到原生 32bpp；
  - 修正 Mach64 MMIO 寄存器窗口定位以及 CRTC、数据通道、FIFO、2D 引擎寄存器偏移；
  - 4MB 显存卡支持 640×480、800×600、1024×768 32bpp，其他分辨率仍受实际 VRAM 与 VBE 时序限制。

---

## 💾 Build 2011.101 更新特性
- **🖥️ 系统信息与音频设备排版修复**：
  - System Information 默认窗口扩大至 `520×250`，为处理器型号、图形设备与音频设备留出更合理的显示空间；
  - 长设备名不再越界覆盖窗口边框，改为按单词自动换行；其他系统字段增加边界裁切和省略号保护；
  - 当前音频设备可完整分行显示控制器与 AK4540 / TR28023 (CT1297) 编解码器信息。
- **🎵 NekoPlayer 边框与硬件信息布局修复**：
  - 播放器面板改为严格按窗口客户区尺寸绘制，不再覆盖右侧和底部窗口边框；
  - 窗口高度增加至 215px，硬件名称移至独立 `DEV:` 区域并自动换行，避免文字溢出播放器。
- **📁 文件管理器默认尺寸优化**：首次打开由 `320×190` 扩大至 `440×260`，默认可显示更多文件和更长的文件名。
- **⏸️ 暂停状态持续噪声修复**：
  - 暂停时先静音硬件 PCM/主输出，再设置 DMA 暂停位，彻底阻断 Codec 重复输出最后一个采样造成的持续“滋滋”声；
  - 暂停期间调整音量或静音不会意外解除硬件静音；
  - 恢复播放时先恢复原音量，再解除 DMA 暂停；停止或关闭窗口后会恢复硬件混音器状态。

---

## 💾 Build 2011.020 更新特性
- **🎼 Creative CT4810 / Ensoniq ES1371 实体机与 Pcbox 无声修复**：
  - **Bus Master 与 DMA 时序校正**：修正 ES1371 `CONTROL.BREQ` 为 Bit 7，播放启动时同时使能 PCI Bus Master 与 DAC2，恢复硬件从 64KB 环形缓冲区取数；
  - **AC'97 冷/热复位与就绪握手**：补全 ES1371 AC'97 Link 热复位、CT5880/ES1373 修订版专用复位脉冲及 Codec RDY 等待，兼容 AK4540 与 TriTech TR28023 / Creative CT1297 两类 Codec；
  - **SRC 与 DAC2 串行格式修正**：按 ALSA `ens1371` 的两阶段 SRC RAM 读取握手重写采样率配置，并补齐 DAC2 Start/End Offset、循环模式与 32KB 周期计数；
  - **Codec 识别**：启动时读取 AC'97 Vendor ID，在 SysInfo 中显示 `AK4540 AC'97` 或 `TR28023 (CT1297) AC'97`。
- **🔊 CMI8738 实体机无声回归修复**：
  - **恢复 Dual DAC 输出路由**：重新设置 `CMI_MISC_ENDBDAC`，保持 `CMI_MISC_XCHGDAC = 0`，使 Channel 0 按 ALSA `cmipci` 定义输出至前置绿色接口并镜像到后置接口；
  - **恢复扩展路由初始化**：向 `EXT_MISC` 写入 CMI8738 所需的 `0x00000920` 魔数，避免只清除该寄存器导致部分实体卡 DAC 无输出；
  - **修正 SB Mixer 误操作**：移除对输入选择寄存器 `0x3D/0x3E` 的错误写入，保留 `0x3C = 0x1F` 的模拟输出开关使能。

---

## 💾 Build 2011.100 更新特性
- **🎼 Creative CT4810 (CT5880 / Ensoniq ES1371) PCI 声卡原生硬件驱动**：
  - **PCI 总线即插即用识别**：自动探测 Vendor ID `0x1274`（Ensoniq）及 Device ID `0x1371` / `0x5880`（Creative CT4810），配置 PCI Bus Mastering 与 Latency Timer；
  - **AC'97 规范混音器与音量控制**：建立与 AC'97 物理 Codec 芯片的读写状态机握手，支持 Master / Headphone / PCM 各级通道 0dB 无衰减高保真输出与静音切换；
  - **原生硬件 SRC 采样率同步**：写入 CT5880 / ES1371 内部 Sample Rate Converter RAM，智能匹配 44.1kHz 与 48kHz 播放时钟比率；
  - **DAC2 64KB 连续双缓冲 DMA**：利用物理内存页框对齐分配器构建环形双缓冲，实现零丢帧无缝流式播放。
- **🎛️ 统一音频硬件抽象层（Kernel Audio HAL）与智能声卡识别**：
  - 重构内核音频架构，构建统一的 `audio_driver_ops_t` 硬件抽象表；
  - 系统启动时自适应级联探测：优先智能识别 **Creative CT4810**，其次识别 **C-Media CMI8738**；
  - NekoPlayer 播放器与 SysInfo 系统信息视窗全面联动，智能显示当前真实激活的声卡硬件全名。
- **🔊 CMI8738 实体机模拟前置音频孔（绿色孔）出声彻底修复**：
  - **通道路由校准**：消除开启 `CMI_MISC_ENDBDAC`（双 DAC 结构）导致 Channel 0 播放被重定向至后置环绕插孔而使前置绿孔无声的硬件缺陷，回退至纯净 Single DAC 结构，并将 Channel 0 严格绑定至前置主输出并镜像至后置孔；
  - **SB16 模拟主控/PCM 开关全解禁**：在软复位后向 SB16 Mixer 寄存器 `0x3D` 与 `0x3E` 真实写入 `0x00`，彻底解除实体卡默认的 Master Output / Voice Mute 硬件拦截。

> 注：本节中的 Single DAC 与 `0x3D/0x3E` 路由判断已在 Build 2011.020 中依据 ALSA `cmipci` 修正，最新行为以上一节为准。

---

## 💾 Build 2011.004 更新特性
- **🎵 CMI8738 实体机声卡无声与时钟 2 倍速飞逝彻底根治**：
  - **DMA 启动时序与寄存器保护**：消除在配置 `CH0_FRAME1`（物理内存基址）与 `CH0_FRAME2`（样本帧计数器）之后意外复位通道导致 DMA 寄存器被清零的关键缺陷，调整为先执行通道软复位，再写入物理基地址与帧计数器，确保物理 DMA 真实读取并输出 PCM 数据；
  - **输出通道与总线主控兼容**：开启硬件 Dual DAC 模式（`CMI_MISC_ENDBDAC`）与非 VX 芯片组 PCI 总线主控突发时序兼容（`CMI_MISC_TXVX`），写入 `EXT_MISC` 扩展路由寄存器，配置 PCI Latency Timer 至少 64 周期，确保实体机模拟输出插孔（绿色孔与后置孔）正常出声；
  - **重构半缓冲轮询状态机**：增加硬件计数器活跃度动态感知，在计数器正常跑动时采用严格的乒乓翻转逻辑，彻底消除实体机超时看门狗与帧状态冲突引发的“现实 1 秒、播放器快进 2 秒”的倍速倒带现象；在计数器卡死时保持精准 1x 原速推进。
- **🌄 自定义桌面壁纸重启后变灰故障修复**：
  - **开机壁纸加载时序修正**：修复开机初始化时序中窗口管理器 `wm_init()` 内部调用 `wallpaper_init()` 导致 `settings_load()` 已加载到内存的壁纸指针被覆写清零的致命缺陷；
  - **配置加载持久化保留**：将壁纸子系统清空逻辑前置于配置读取之前，消除开机读图耗时后桌面依然回退默认纯灰背景的问题，实现重启后用户壁纸与排布方式 100% 完美呈现。

---

## 💾 Build 2011.003 更新特性
- **⚠️ 安装前智能引导/系统文件探测与覆盖防呆确认**：
  - 在正式开始安装前对目标盘符进行深度扫描，自动探测是否已存在 `\boot\kernel.elf`、`\grldr`、`\menu.lst` 或 Windows 引导文件（`bootmgr` / `ntldr`）；
  - 若检测到已有引导或旧版系统，弹出醒目的复古警示视窗，明确罗列检测到的文件名，并提供 `[Overwrite Now]`（覆盖并继续安装）、`[< Back]`（返回重选其他驱动器）与 `[Cancel]`（取消并退出向导）选项，防止静默覆盖破坏用户重要分区；
- **🖥️ 桌面 Install 一键安装向导图标全环境常驻**：
  - 无论从光盘 Live 启动还是本地硬盘启动，桌面始终保留 Install 快捷图标，方便多硬盘互装或随时重装修复引导；
- **📁 文件管理器点击穿透与驱动器打不开根治**：
  - 修复置顶窗口索引（`z_order[10]`）事件过滤缺陷，全面升级为由窗口管理器标准派发的 `fm_click`、`fm_drag`、`fm_release`；
  - 工具栏新增 `[Open]` 快捷按钮，支持直接选中行后按回车键、双击或点击 `[Open]` 畅通进入 `C:\` 等驱动器与子文件夹。
- **📊 FAT32 空闲容量动态刷新与 FSInfo 扇区精确同步**：
  - 解决安装到硬盘后剩余空间仍停留在未安装前数值（如 231 MB）的缓存脱节故障；
  - 在簇分配中引入 `ensure_free_calculated()` 动态校准，并在所有 FAT 卷同步与写入刷新时向磁盘扇区 1 真实写入最新的 FAT32 FSInfo 结构；
  - 容量格式化支持保留 1 位小数（如 `228.2 MB` / `231.2 MB`），告别因整数截断显示相同大小的误解。
- **⚙️ 本地硬盘启动设置持久化与 CD-ROM Live 模式智能区分**：
  - 引入内核级配置系统 `C:\settings.ini`，本地硬盘启动时自动持久化与恢复**屏幕分辨率（宽/高）**、**色彩深度（BPP）**、**桌面背景与壁纸路径/排布模式**；
  - 智能感知启动介质（BIOS 驱动器号与 Multiboot 启动参数），CD-ROM 光盘启动维持 Live 模式（纯内存体验，不污染物理磁盘）。
- **⚡ 安装向导全流程动态进度条与写入速度深度优化**：
  - 告别点击开始安装后系统静默无响应 10 余秒的体验问题，新增复古 3D 嵌入式进度条与实时扇区写入百分比提示；
  - 消除旧版冗余的 1.8MB 内核双重写入拷贝，挂接 FAT 驱动层实时回调，每写入 64KB 平滑更新进度与子步骤提示，流畅度大幅提升。

---

## 💾 Build 2011.000 更新特性
- **💿 从 CD/DVD 光盘一键安装至硬盘 (OS Installer)**：
  - 从光盘或虚拟机光驱启动时，桌面自适应呈现“Install”安装向导图标；
  - 自动扫描并智能识别可用 FAT16 / FAT32 目标硬盘分区（清晰展示驱动器号、文件系统类型、总容量与剩余空间）；
  - 具备二次防误触确认与安全警告对话框；
  - 核心系统文件一键高速克隆复制至硬盘（`\boot\kernel.elf`、`\grldr`、`\menu.lst`）；
  - 完整植入 GRUB4DOS 主引导记录（MBR）与分区引导记录（PBR），严格保留原磁盘分区表、NT 磁盘签名及 FAT BPB 参数，确保实体机与虚拟机冷启动即开即用；
  - 安装完毕后弹出交互式“立即重启”或“继续留用桌面”对话框，支持一键秒级软复位。
- **💽 彻底修复硬盘冷启动后所有盘文件打不开的故障**：
  - 重构 FAT 存储子系统驱动器挂载架构，支持 26 个驱动器盘符（A: ~ Z:）直接索引与 `blkdev_t` 块设备直接寻道；
  - 消除旧版 `fat_init()` 将包含 GRUB4DOS MBR 的启动盘误判为软盘超级引导块导致卷挂载失败的缺陷；
  - VFS 与 FAT 实现智能按需动态自适应挂载（Auto-Mount Fallback），无论冷启动顺序如何，所有 IDE、SATA、软盘驱动器均可秒级识别与畅快读写。
- **🎵 CMI8738 实体机声卡播放卡死（Play 00:00）根治与硬件 DMA 防越界**：
  - 采用物理页对齐分配器为 PCI Bus Master DMA 分配 64KB 物理地址边界严密对齐的物理连续内存页，杜绝在 PCI 突发传输中穿越 64KB 物理回绕边界导致的硬件挂起；
  - 引入 3 次重试原子读取硬件当前传输计数器，消除实体卡高频抖动造成的瞬态越界读数；
  - 增加双缓冲轮询软件超时安全看门狗（Watchdog），在实体卡未能及时触发中断或寄存器回绕时自动平滑推移播放指针与填充数据，彻底根除实体卡卡在 `00:00` 的问题；
  - 默认开启 Line Out / Headphone 双硬件增益通道（0x41/0x42）与绿色孔输出通道，确保实体卡接驳音箱与耳机清晰出声。

---

## 🎵 Build 2010.000 更新特性
- **🔊 CMI8738 PCI 原生声卡驱动与 DMA 环形缓冲**：支持 C-Media CMI8738/CMI8338 PCI 独立声卡，配置 PCI 总线主控与 64KB 连续物理页 DMA 环形双缓冲，杜绝音频爆音。
- **🎛️ 内核级 DSP 音频处理核心**：内置双基准时钟（44.1k/48k）多相滤波抽取下采样，支持 Hi-Res 高采样率（88.2k/96k/176.4k/192k）保真播放；支持 24/32 位高位深 TPDF 三角抖动降位与纯整型 IEEE 浮点转换。
- **🎼 纯定点数全格式音频解码引擎**：零外部库与 FPU 依赖，完整支持 WAV（PCM/IEEE Float）、FLAC（无损定点 LPC/Rice 预测）、MP3（CBR 与 VBR 动态码率，带 Seek 静默预充防爆音）。
- **📻 NekoPlayer 复古图形音频播放器**：Winamp / QuickTime 经典复古美学，墨绿 LCD 显示屏、跑马灯曲名展示、交互式 Seek 进度条、音量控制；硬件同步的 10 段实时动态频谱仪与双声道 VU 表；未检测到声卡时智能提示“无输出设备”。

---

## 🛠️ Build 2006.001 更新特性
- **🖥️ 32位真彩色扫描线严密对齐**：彻底修复老式 VBE ROM（如 ATI Mach64 VT2）在 800×600 模式下因缺少原生 32 位支持导致的 $4:3$ 扫描线跨度错位与屏幕斜纹分裂问题；并在控制面板精确标注 `(Not Supported)`。
- **🖼️ 图片查看器菜单纯净 ASCII 呈现**：修复点阵字库不支持多字节 UTF-8 中文造成的文字乱码与边框溢出问题。
- **📁 文件管理器顶层点击事件路由修复**：重构窗口焦点与事件派发索引（`z_order[8]`），彻底恢复文件管理器条目的交互与点击。
- **📝 记事本 60 FPS 选区优化与菜单体系**：引入字符单元格去重缓存，鼠标选中时不再产生冗余全屏重绘；提供完整的 `File`（New / Save / Save As / Quit）与 `Edit`（Cut / Copy / Paste / Select All / Clear）菜单功能，并引入保存失败模态错误弹窗提示。

---

## 🎨 Build 2006.000 更新特性
- **⚡ 丝滑桌面图标拖拽优化**：彻底消除了每次像素位移触发的 2MB 全屏 backbuffer 重绘，全面升级为 LFB 局部透明虚线幽灵框，拖动帧率直达 60+ FPS。
- **🖥️ 32位全彩真彩色模式探测与持久化**：深度扫描 VESA VBE 控制器模式列表，识别并切换 32-bit ARGB/RGBA 硬件显示模式，解决控制面板设置后回退 24 位色的问题。
- **🖼️ 4K 32位全彩图片查看器 (Image Viewer)**：内置零外部依赖的 BMP 与 PNG 硬件级解码引擎，最高支持 4K (3840×2160) 全彩图片查看、平移拖动画布、阶梯式缩放（Zoom In/Out/1:1/Fit）；具备严格物理内存（PMM）越界与 OOM 检测防护。
- **🌄 Live 桌面壁纸系统**：支持一键将当前图片设为桌面壁纸，提供 Windows 标准的 4 种排布方式：**填充 (Fill)**、**适应 (Fit)**、**拉伸 (Stretch)**、**平铺 (Tile)**，同时打通图片查看器与控制面板双向设置。

---

## 📜 开源协议

本项目采用 [MIT 许可证](LICENSE) 开源。欢迎交流学习与改进！

