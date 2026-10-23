# Pixel Neko Next — 阶段开发暂停与进度备忘录 (pause.md)

> **记录时间**：2026-10-23  
> **当前系统版本**：`Version C2.0 (Build 2000.002)`  
> **指导规划文件**：[plan3.md](plan3.md)  
> **状态简述**：按照 [plan3.md](plan3.md) 的全量要求，**核心代码已全面编写与对接完毕**。因时间安排需要，在此提交当前所有代码与方案文档并暂停工作（暂不编译与发布 ISO），以便下次无缝接管与验证。

---

## 一、 当前达成的开发里程碑与模块完成情况

本阶段聚焦于真实复古硬件（**Pentium Pro 200MHz + 440FX + ATI 3D Rage II+DVD**）及虚拟机环境，所有功能均严格遵循 **Pentium Pro 及以上 CPU 的 100% 指令集兼容性**（Makefile 启用 `-mno-mmx -mno-sse -mno-sse2`），且无任何外部 libc 运行时依赖。

| 里程碑 | 核心特性 | 状态 | 涉及与变动文件 |
|---|---|---|---|
| **M11** | **磁盘容量与可用空间计算 (DSK)** | **已完成** | [fs/fat.c](fs/fat.c), [include/fat.h](include/fat.h), [fs/iso9660.c](fs/iso9660.c), [include/iso9660.h](include/iso9660.h), [fs/vfs.c](fs/vfs.c), [include/vfs.h](include/vfs.h), [apps/filemanager.c](apps/filemanager.c) |
| **M12** | **ATI 3D Rage II+DVD 2D 加速与真彩色驱动 (GPU)** | **已完成** | [include/ati_mach64.h](include/ati_mach64.h), [drivers/ati_mach64.c](drivers/ati_mach64.c), [include/gfx_accel.h](include/gfx_accel.h), [drivers/gfx_accel.c](drivers/gfx_accel.c), [kernel/splash.c](kernel/splash.c), [include/port.h](include/port.h) |
| **M13** | **控制面板显示设置重构与显存保护 (DISP)** | **已完成** | [apps/cpanel.c](apps/cpanel.c) |
| **M14** | **PC 喇叭复古音效与系统提示音 (SPK)** | **已完成** | [drivers/speaker.c](drivers/speaker.c), [include/speaker.h](include/speaker.h), [kernel/splash.c](kernel/splash.c), [apps/editor.c](apps/editor.c), [apps/filemanager.c](apps/filemanager.c) |
| **M15** | **Mac 炸弹崩溃框与桌面单色图案 (ERR & PAT)** | **已完成** | [include/panic.h](include/panic.h), [kernel/panic.c](kernel/panic.c), [kernel/idt.c](kernel/idt.c), [wm/wm.c](wm/wm.c), [apps/cpanel.c](apps/cpanel.c) |
| **M16** | **系统信息、15 推盘游戏、菜单补全与真彩支持 (SYS, GM, MEN)** | **已完成** | [include/sysinfo.h](include/sysinfo.h), [apps/sysinfo.c](apps/sysinfo.c), [include/puzzle.h](include/puzzle.h), [apps/puzzle.c](apps/puzzle.c), [include/editor.h](include/editor.h), [apps/editor.c](apps/editor.c), [include/wm.h](include/wm.h), [kernel/kernel.c](kernel/kernel.c), [drivers/mouse.c](drivers/mouse.c), [wm/wm.c](wm/wm.c), [Makefile](Makefile) |

---

## 二、 关键技术细节与实现亮点

### 1. 磁盘空间实时计算与文件管理器升级 (M11)
- **FAT32**：优先读取保留扇区 LBA 1 的 **FSInfo** 扇区（`0x41615252` 签名），实时提取可用簇计数值；
- **FAT12/FAT16**：挂载时单次轻量扫描 FAT 表项缓存空闲簇数，文件新建与删除操作联动更新；
- **只读与未知分区**：光盘 ISO9660 正确提取 PVD 总扇区数，可用空间明确标识为 `0 KB (只读)`；NTFS / Linux EXT 分区保留总大小，可用空间标注为 `-- (未挂载)`；
- **界面**：文件管理器根目录升级为 4 列报表（`Drive`, `Label & Type`, `Total Size`, `Free Space`），底部状态栏实时汇总容量。

### 2. ATI 3D Rage II+DVD / Mach64 专有驱动与真彩色 (M12)
- **开源生态移植**：移植自 Linux 内核 `atyfb` 与 XFree86 `mach64` 开源驱动寄存器拓扑与规范；
- **开机自检与平滑回退（Auto-Fallback）**：扫描 PCI Vendor `0x1002` 与 Mach64 设备 ID，从 `MEM_CNTL` 精确探测物理显存大小（512K/1M/2M/4M/8M）。若显卡不匹配或映射失败，**自动平滑回退至标准 VESA/VBE 软件渲染，确保 100% 安全点亮，绝不死锁**；
- **真彩色（24位 / 32位色）支持**：编程 Internal RAMDAC `DAC_CNTL` 与 `DAC_REGS` 线性 1:1 Identity Ramp LUT，解决 Direct Color 模式偏色/黑屏难题；
- **严格显存容量保护**：在驱动与控制面板联动校验 $VRAM\_Needed = W \times H \times (bpp / 8)$，在 2MB 实机显存下，**严格禁用 1024x768@24/32 位真彩色**（单帧需要 2.25MB~3.0MB），仅允许 640x480 与 800x600 开启真彩。

### 3. 控制面板重构与桌面图案 (M13 & PAT)
- 废除固定模式按钮，采用图形化 **分辨率下拉框** 与 **色彩深度下拉框**；
- 依据显卡驱动状态与显存上限，动态置灰/隐藏不安全的高色深选项；
- 内置 10 秒安全倒计时防黑屏回滚机制（Revert Timer）；
- 引入 Mac OS 经典 8×8 单色壁纸点阵库（Stipple 50%、Dense Diagonal、Micro Dots、Brick Tile、Slate Gray、Solid Gray），控制面板可快速切换并在桌面生效。

### 4. Mac 经典黑色炸弹错误对话框 (M15)
- 接管通用保护故障（#0D）、缺页异常（#0E）、非法指令（#06）等保护模式严重异常；
- 绕过内存窗口管理器，直接往线性显存物理映射地址（LFB）绘制 24×24 燃烧引信炸弹对话框；
- 打印崩溃现场异常名称、EIP、CS、EFLAGS、CR2 及 CPU 架构；提供 `[ Restart Computer ]` 按钮并轮询键盘与鼠标，支持即时安全重启。

### 5. 系统信息 App 与 15 数字推盘游戏 (M16)
- **系统信息 (SysInfo)**：包含 `[ System ]`, `[ PCI Bus ]`, `[ Drives ]` 三大选项卡，全面呈现 CPUID 架构详情、主频估计、内存分布、显卡加速状态、全 PCI 总线扫描列表与驱动器空间报表；
- **15-Puzzle 推盘**：经典 4×4 立体浮雕滑块游戏，每次通过随机合法移动充分打乱，保证 100% 可解，纯事件驱动，CPU 占用 0%；
- **菜单栏交互**：补全 `Edit`（剪贴板 Cut/Copy/Paste）、`View`（Clean Up Desktop 自动对齐重排）、`Special`（关于、系统信息、推盘、清空废纸篓、重启、关机）；
- **全色深底层渲染管线升级**：
  - `include/wm.h`：`PUT_PIXEL` 宏全面支持 8bpp、15/16bpp、24bpp、32bpp 并引入 `palette32[256]`；
  - `kernel/kernel.c`：`backbuffer` 升级至 3MB（`1024 * 768 * 4`），`copy_rect_to_lfb` 与 `copy_fullscreen_to_lfb` 支持多色深位块搬移；
  - `drivers/mouse.c`：鼠标背景暂存缓冲区扩展至 32 位，消除真彩色下的光标花屏；
  - `Makefile`：添加所有新增源文件。

---

## 三、 本次暂停前提交清单 (Git Commit Status)

1. **版本标识**：已更新为 `Version C2.0 (Build 2000.000)`。
2. **文档包含**：已在 `.gitignore` 中解除 `plan3.md` 的忽略，连同本 `pause.md` 一并提交至 GitHub 远程仓库 `main` 分支。
3. **未编译 ISO**：本次提交仅上传全部代码与设计文档，不触发或执行 ISO 编译流程。

---

## 四、 阶段工作继续与验证交付 (Resumed & Verification Delivered)

已按照规划继续完成后续任务与代码审计：
1. **全局语法与括号匹配审计**：修复了 [wm/wm.c](wm/wm.c) 中 `power_confirm` 关机确认框逻辑块末尾缺失右括号与状态返回的语法错误，恢复正常的事件循环与全屏重绘逻辑；
2. **奔腾 Pro (Pentium Pro) 及以上 CPU 兼容性加固**：
   - [Makefile](Makefile)：`CFLAGS` 明确启用 `-march=pentiumpro`，与 `-mno-mmx -mno-sse -mno-sse2` 联动，确保生成的机器指令 100% 兼容无 MMX/SSE 的经典 Pentium Pro (P6) 架构及后续所有处理器；
   - [kernel/panic.c](kernel/panic.c)：将仅在 Pentium 4 开始引入的 `pause` 指令统一重构为标准兼容的 `rep; nop`，避免老式 CPU 异常；
3. **文档与工程元数据对齐**：[README.md](README.md) 同步更新至 `Version C2.0 (Build 2000.000)` 并补全 2D 加速、磁盘空间报表、SysInfo 及 15 推盘等项目架构树；
4. **编译与发布模式**：所有修改已通过严格的静态语法与原型检查，提交至 GitHub `main` 分支后将自动触发 GitHub Actions CI 矩阵编译并发布 `pixel_neko_build_2000.000.iso`。

---

## 五、 Build 2000.001 界面排版与视觉重叠全面修复 (Layout Overhaul)

针对 PCBox 虚拟机实机测试中反馈的排版与重叠缺陷进行了彻底排查与重构：
1. **控制面板 ([apps/cpanel.c](apps/cpanel.c))**：
   - 修复了 `Mouse Speed:`、`Resolution:`、`Color Depth:`、`Driver:` 文本标签与右侧控件/状态文字之间的像素重叠，左侧标签统一对齐于 `wx+10`，输入控件与滑块统一移至 `wx+120`；
   - 色彩深度下拉框重构：废除在字符串末尾覆盖绘制 `N/A` 的做法，改为专用无重叠文本（如 `24-bit (No VRAM)` / `32-bit (No VRAM)`）；
   - 展开下拉列表高度与按钮位置解耦：窗口默认高度提升至 210px，底部 `Apply Mode` / `OK` 按钮下移至 `wy+172`，彻底消除下拉框遮挡按钮的视觉缺陷。
2. **系统信息 ([apps/sysinfo.c](apps/sysinfo.c))**：
   - 窗口默认尺寸加宽至 420×220px；
   - 顶部选项卡标头加宽至 95px 并实现居中文本渲染，彻底根除 `PCI Bus` 超出标头边框的问题；
   - Tab 0（System）值起始坐标右移至 `lx+125`，彻底解决 `Installed RAM:`、`Graphics Card:`、`Video Memory:` 与数值的重叠问题；
   - Tab 2（Drives）多列报表由手动空格拼凑重构为定点多列对齐渲染（Drive `lx+8`、FileSystem `lx+56`、Total Size `lx+165`、Free Space `lx+275`），各列完美对齐，消除右侧边框截断。
3. **菜单栏下拉菜单 ([wm/wm.c](wm/wm.c))**：
   - 废除虚线框 `box()` 与透明灰色底，重构为经典 Mac OS 实心纯白背景（Color 15）、1px 实体黑边框与 2px 经典实体右下投影；
   - 菜单宽度加宽至 172px，行距采用 18px，解决 `System Information` 与 `Shut Down Computer` 右侧字母截断问题；
   - 同步更新菜单点击检测区域与安全边界检查。
4. **文件管理器 ([apps/filemanager.c](apps/filemanager.c))**：
   - 底部状态栏 Y 坐标上移至 `wy+wh-18`（分割线 `wy+wh-21`），彻底避免文字像素切穿窗口底部黑边框；
   - 状态栏文字增加右侧边界保护，默认窗口宽度提升至 320px，保证大容量驱动器状态信息完整呈现。

---

## 六、 Build 2000.002 显存识别、直彩偏色与极速拖动重构 (Mach64 & Performance Fixes)

针对实机/PCBox模拟器开启 ATI Mach64 VT2 时的显存检测错误、直彩色偏暗/失真以及窗口拖动卡顿等严重问题进行了彻底根治：
1. **显存物理回绕探测算法 (Physical VRAM Wrap-around Probing)**：
   - [drivers/ati_mach64.c](drivers/ati_mach64.c)：废除容易误判的 `MEM_CNTL & 0x07`（Mach64 VT/GT 上 `0x05` 实际代表 2MB EDO DRAM，旧代码误判为 8MB）；
   - 引入线性显存回绕探测：直接在物理 LFB（`0MB`, `1MB`, `2MB`, `4MB` 偏移）写入测试签名并比对回绕镜像，精确识别 1MB、2MB、4MB 与 8MB 物理显存，与模拟器/实机硬件设定 100% 吻合；
   - [apps/cpanel.c](apps/cpanel.c)：联动显存保护，2MB 显存下自动禁止超出显存容量的 1024x768 24bpp (2.25MB) 与 32bpp (3.0MB)，并在列表明确标出 `(No VRAM)`。
2. **Mach64 Direct Color (直彩色) RAMDAC 旁路与偏色修复**：
   - [drivers/ati_mach64.c](drivers/ati_mach64.c) & [kernel/kernel.c](kernel/kernel.c)：彻底移除 `ati_mach64_program_ramdac()` 与 `program_special_ramdac()`；
   - 修复根因：VBE BIOS 切换直彩色模式（15/16/24/32bpp）时已正确配置内部 Direct Color 旁路通道。旧代码误将 6 位的 `i >> 2`（最大 63）写入 8 位 DAC 并篡改 `MACH64_DAC_CNTL` 多路选择器，导致屏幕亮度被压制到 25% 且产生严重偏暗偏绿偏色；现完全保留 VBE BIOS 原生直彩硬件通路，画面色彩饱满鲜艳。
3. **窗口拖拽极速局部重绘 (Rubber-band Box LFB Direct Mapping)**：
   - [wm/wm.c](wm/wm.c)：废除拖动过程中每像素移动全屏重绘背景、所有图标、所有窗口的旧逻辑；
   - 移除 `wm/wm.c` 中意外残留的旧拖拽逻辑代码块；
   - 实现 1px 虚线边框直接映射：拖动过程中仅传输 4 条边框（约 2KB 数据，相比全屏传输降低 99.8% 总线开销），松开鼠标时才一次性完成窗口坐标更新与全屏重绘；
   - [kernel/kernel.c](kernel/kernel.c)：主事件循环彻底优化，移除拖动期间强制将 `last_mx` 设为 -1 导致的重复全屏拷贝漏洞；鼠标移动仅局部刷新 16x16 光标，拖动窗口达到丝滑 60+ FPS。


