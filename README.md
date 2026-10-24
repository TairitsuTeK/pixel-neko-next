# Pixel Neko Next 🐾

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Architecture](https://img.shields.io/badge/Arch-x86--32-blue.svg)](https://en.wikipedia.org/wiki/IA-32)
[![Boot](https://img.shields.io/badge/Boot-Multiboot%20%2F%20GRUB-green.svg)](https://www.gnu.org/software/grub/)
[![Version](https://img.shields.io/badge/Version-C2.0%20(Build%202010.011)-brightgreen.svg)]()

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
│   └── *.h               # ati_mach64.h, gfx_accel.h, panic.h, sysinfo.h, puzzle.h, etc.
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

## 🚀 Build 2010.010 更新特性
- **📁 FAT16/FAT32 长文件名（LFN / VFAT）全栈支持**：实现 VFAT 0x0F 长目录项双向解析与校验合成，突破 8.3 格式限制，文件管理器与应用全线支持长文件名无缝浏览与打开。
- **🎛️ CMI8738 声卡 48kHz 硬件 DAC 采样率同步**：修复 `FUNCTRL1` 中仅设置 `ASFC` 未配置 `DSFC` 导致 DAC 始终运行在 44.1kHz 的硬件分频抖动与卡顿缺陷，实现 48kHz MP3 / 96kHz 降采样极致流畅回放。
- **🔇 关机停机画面静音与 DMA 彻底停止**：在退出系统进入经典 Safe to Turn Off 画面及重启前立即切断声卡 DMA 环形缓冲与静音输出，彻底消除关机停机时的硬件残余卡碟循环循环。
- **💽 Intel H55 (ASUS P7H55-M LE) SATA / IDE Native PCI 驱动器自适应探测**：新增 PCI 存储控制器（Class 0101）全面枚举与 BAR0~BAR3 Native PCI I/O 端口识别，解决现代与近现代主板兼容 IDE 模式下找不到硬盘与光驱的问题。
- **🏷️ 开机画面与系统内核版本号迭代同步**：开机 Boot Splash 及系统关于视窗全面联动动态版本号，正式迭代至 `Build 2010.010`。

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

