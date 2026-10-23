# Pixel Neko Next 🐾

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Architecture](https://img.shields.io/badge/Arch-x86--32-blue.svg)](https://en.wikipedia.org/wiki/IA-32)
[![Boot](https://img.shields.io/badge/Boot-Multiboot%20%2F%20GRUB-green.svg)](https://www.gnu.org/software/grub/)
[![Version](https://img.shields.io/badge/Version-C2.0%20(Build%201750.000)-brightgreen.svg)]()

**Pixel Neko Next** 是一个轻量级、开箱即用的 x86-32 位原生微桌面操作系统（Bare-metal GUI OS）。系统完全基于 C 语言与 NASM 汇编从零编写，具备高分辨率图形桌面、完整的窗口管理器、Windows 驱动器盘符映射、ISO9660 光盘文件系统、FAT12/16/32 文件系统、原生软驱驱动、丰富的内置应用程序以及极致复古的经典开机与关机停机体验。

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
│   ├── splash.c          # Mac OS 9 & Win 3.1 风格开机动画
│   └── timer.c           # 统一 PIT 模式 2 计时器与延时驱动
├── drivers/              # 硬件与总线驱动
│   ├── disk.c            # ATA / ATAPI 存储驱动
│   ├── font.c            # 基础字体渲染
│   ├── keyboard.c        # PS/2 键盘与按键缓冲
│   ├── linux_font.c      # Linux 内核字体位图
│   ├── mouse.c           # PS/2 鼠标驱动与超时保护
│   └── speaker.c         # PC 扬声器驱动与复古开机和弦
├── fs/                   # 文件系统实现
│   └── fat.c             # FAT16 / FAT32 文件系统
├── wm/                   # 窗口管理器核心
│   └── wm.c              # 窗口层叠、拖拽、菜单栏与图层混合
├── apps/                 # 内置桌面应用程序
│   ├── calc.c            # 简易计算器
│   ├── cpanel.c          # 控制面板（动态切分辨率、硬件信息）
│   ├── editor.c          # 文本编辑器
│   └── filemanager.c     # 图形化文件管理器
├── include/              # 全局公共头文件
│   └── *.h               # cpu.h, splash.h, timer.h, speaker.h, etc.
├── scripts/              # 编译与构建辅助脚本
│   ├── build_counter.sh
│   └── build_number.txt
├── link_grub.ld          # 内核 ELF 链接脚本
├── Makefile              # 顶级 Makefile 构建规则
└── LICENSE               # MIT 开源协议
```

---

## 🛠️ 构建与运行环境

### 依赖项
- **GCC**（支持 `-m32` 交叉编译）
- **NASM**（汇编器）
- **GNU ld**
- **grub-mkrescue** 与 **xorriso**（用于生成可启动 ISO）
- **QEMU**（推荐 `qemu-system-i386` 用于本地模拟运行）

### 编译与启动

1. **编译生成可启动 ISO 镜像**
   ```bash
   make
   ```
   编译完成后会在根目录生成 `pixel_neko_build_1700.000.iso`。

2. **在 QEMU 虚拟机中一键运行**
   ```bash
   make run
   ```

3. **GDB 远程调试模式**
   ```bash
   make debug
   ```

4. **清理构建生成物**
   ```bash
   make clean
   ```

---

## 📜 开源协议

本项目采用 [MIT 许可证](LICENSE) 开源。欢迎交流学习与改进！
