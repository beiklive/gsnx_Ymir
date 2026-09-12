# Ymir 项目结构梳理 —— 平台 / UI 层 / 模拟层 / 渲染后端

> 本文面向希望快速理解 Ymir(Sega Saturn 模拟器)代码库的人。
> 覆盖:顶层工程结构、四个可执行前端、`ymir-core` 模拟层、各平台差异,
> 以及从前端图形后端到模拟器核心渲染器的完整渲染结构。

---

## 0. 总览

```
                    ┌─────────────────────────────────────────────┐
                    │            桌面 / 其它平台前端                 │
                    │             apps/ymir-sdl3 …                │
                    │  SDL3 + Dear ImGui + 服务层 + gfx 图形后端     │
                    └───────────────┬─────────────────────────────┘
                                    │  调用 ymir::Saturn 门面
                    核心配置 Ymir.toml / Observable / 事件线程模型
                    ┌───────────────▼─────────────────────────────┐
                    │            libs/ymir-core  (模拟层)           │
                    │  sys: Saturn / Bus / SystemMemory / Scheduler │
                    │  hw : SH-1 SH-2 m68k SCU SCSP SMPC VDP CD块   │
                    │  media: 光盘镜像加载器 / CD 设备 / ISO9660      │
                    │  vdp 渲染器接口 + 具体渲染器(Software/D3D12)     │
                    └─────────────────────────────────────────────┘
```

**顶层目录**

| 路径 | 作用 |
|---|---|
| `CMakeLists.txt` | 顶层构建;检测系统/架构、选项与功能开关、引入 vendor/libs/apps/tests |
| `cmake/` | CMake 辅助模块(编译 shader、架构探测、PGO、资源内嵌 CMakeRC…)、预置 `presets/` |
| `libs/` | 库:`ymir-core`(模拟器核心)、`ymir-dbg-commons`(调试协议脚手架) |
| `apps/` | 可执行程序:主 GUI `ymir-sdl3`;工具 `ymir-headless`、`ymir-dbg`、`ymir-sandbox`、`ymdasm` |
| `vendor/` | 三方依赖(fmt、mio、libchdr、imgui、concurrentqueue 等 git 子模块) |
| `vcpkg/`、`vcpkg.json` | vcpkg 清单(主要用于前端与测试依赖;核心不依赖 vcpkg) |
| `tests/` | 单元测试 |
| `docs/` | 说明与本文档 |

---

## 1. 平台、架构与构建

- 支持平台:Windows 10+、macOS 15+、主流 Linux 发行版、FreeBSD;
  CPU:x86-64(SSE2 基线,可选 AVX2)与 arm64(NEON)。
- 语言标准 **C++20**;核心库**静态编译**,前端用 vcpkg/SDL3 等。
- 平台相关 CMake 判定用内置变量:`WIN32` / `APPLE` / `UNIX`、`CMAKE_SYSTEM_NAME`(FreeBSD)。
  CPU 架构由 `cmake/DetectArchitecture.cmake` 探测为 `x86_64` 或 `arm64`,其它架构直接报错。
- 关键宏(由核心目标向下传播):
  - `Ymir_DEV_BUILD / Ymir_ENABLE_DEVLOG / Ymir_ENABLE_DEV_ASSERTIONS / Ymir_EXTRA_INLINING`
  - `YMIR_PLATFORM_HAS_DIRECT3D`(=WIN32)、`YMIR_PLATFORM_HAS_METAL`(=APPLE)、`YMIR_PLATFORM_HAS_VULKAN`(=找到了 Vulkan)
  - 功能开关:`Ymir_FF_HOST_CD_DRIVES`(默认 OFF,主机光驱直读)

---

## 2. 四个前端(Application)简介

| app | 目录 | 说明 |
|---|---|---|
| `ymir-sdl3` | `apps/ymir-sdl3/` | **主桌面 GUI**。SDL3 窗口 + Dear ImGui(docking)驱动,内部再拆成“UI 层 / 服务层 / gfx 后端”。见 §3。 |
| `ymir-headless` | `apps/ymir-headless/` | 无头调试 worker(供 JSON-RPC 用),当前为骨架:合并默认→`Ymir.toml`→`Ymir-dbg.toml`→CLI 的路径配置后打印并退出。 |
| `ymir-dbg` | `apps/ymir-dbg/` | 命令行调试前端:定位 `ymir-headless` 二进制,`fork/exec` 后中继 stdin/stdout。 |
| `ymir-sandbox` | `apps/ymir-sandbox/` | 开发者沙盒程序(CD 加载、输入、VDP1 精度/多边形等小实验)。 |
| `ymdasm` | `apps/ymdasm/` | 反汇编工具(m68k / SH-2 / SH-1 / SCU DSP / SCSP DSP)。 |

所有 GUI/逻辑 app 只通过**核心门面 `ymir::Saturn`** 与核心交互(见 §4)。

---

## 3. 桌面 UI 层:`apps/ymir-sdl3`

### 3.1 目录与角色

| 路径 | 角色 |
|---|---|
| `src/main.cpp` / `src/winmain.cpp` | 入口;`winmain` 为 Windows 宽字符转发 |
| `src/app/app.{hpp,cpp}` | `app::App` 主类:SDL 窗口、主循环、事件、菜单/dockspace/OSD |
| `src/app/shared_context.hpp` | 全局状态袋(service locator、`Saturn` 包装、锁、事件队列、字体等) |
| `src/app/events/` | GUI↔模拟线程事件(由 emu/gui 队列收发) |
| `src/app/ui/` | UI 框架(见 3.3) |
| `src/app/services/` | 服务层(见 3.2)与图形后端 `services/gfx/` |
| `src/app/input/` | 输入后端(SDL3 键鼠手柄 → Ymir 输入原语)、绑定 |
| `src/app/settings*` | `Settings` 结构、`Ymir.toml` 读写/迁移、默认值 |
| `src/app/profile.{hpp,cpp}` | 用户数据目录模型(`ProfilePath`) |
| `src/app/audio_system.*` / `rewind_buffer.*` | SDL3 音频流/回退(LZ4 XOR 增量)缓冲 |
| `src/app/rom_manager.*` | ROM(IPL/CDB/卡带)扫描与散列登记 |
| `src/util/` | 跨平台工具:崩溃处理、OS 特性、ROM/文件加载、SDL 文件对话框等 |
| `res/` | 内嵌资源(字体、图标、shader、Info.plist 等) |
| `serdes/` | 存档序列化(Cereal) |

### 3.2 服务层(每个都是注册进 `SharedContext.serviceLocator` 的组件)

`GraphicsService`(图形上下文+ImGui+纹理注册)、`DisplayService`(UI 缩放/主题/字体/全屏模式/窗口几何)、
`WindowManagerService`(统一持有并绘制所有窗口、通用模态框/欢迎引导)、`InputService`、
`DiscService`(光盘对话框/最近光盘/加载)、`ROMService`(IPL/CDB/卡带/推荐卡带)、
`FileDialogService`(原生 SDL3 文件对话框)、`AudioSystem`、`SaveStateService`、
`ScreenshotService`、`UpdateCheckerService`、`MIDIService`(RtMidi)、`DiscordRPCService`、
`MouseCaptureService`、`PersistenceService`(SMPC/备份内存持久化)。

**线程模型**:主线程跑 GUI;模拟线程跑 `RunFrame()`。两侧用两个 moodycamel
`BlockingConcurrentQueue`(`SharedContext.eventQueues.emulator/gui`)交换 `EmuEvent`/`GUIEvent`。
若干互斥锁(`SharedContext.locks`)保护共享硬件对象。帧同步由 `screen.frameReadyEvent`
/`frameRequestEvent` 与音频缓冲水位完成。

### 3.3 UI 约定(基于 Dear ImGui 的自有分层)

| 子目录 | 含义 |
|---|---|
| `ui/window_base.*` | `app::ui::WindowBase`:一个 ImGui 窗口(标题/开关/聚焦/手柄关闭) |
| `ui/windows/` | 具体窗口:设置、外设按键、备份内存管理器、系统状态、消息历史、关于、更新… |
| `ui/windows/debug/` | 调试窗口与“窗口组”(SH2/SCU/SCSP/VDP/CDBlock 的 `WindowSet`) |
| `ui/views/` | 嵌在窗口内的可复用面板(无自己的 Begin/End) |
| `ui/views/debug/` | ~60 个硬件调试视图(寄存器、反汇编、trace、栈…) |
| `ui/views/settings/` | 每个设置 Tab 一个视图(基类 `SettingsViewBase`) |
| `ui/widgets/` | 小型通用元素(示波器、十字准星、RewindBar、区域/制式选择、提示 tooltip…) |
| `ui/model/debug/` | 调试模型状态(断点/观察点绑定到核心 SH2) |
| `ui/state/debug/` | `MemoryViewerState` 等;含完整 Saturn 地址映射表 |
| `ui/defs/` | `SettingsTab` 等枚举 |
| `ui/utils/debug/` | `WindowSetPrinter`(渲染 VDP 窗口逻辑) |
| `ui/fonts/` | Material Symbols 图标码位 |

设置窗口共 11 个 Tab:`General GUI Hotkeys System IPL Input Video Audio Cartridge CDBlock Tweaks`
(枚举 `SettingsTab`,详见 `docs/ymir-configuration.md`)。

---

## 4. 模拟层 `ymir-core`

### 4.1 门面与系统

- `include/ymir/ymir.hpp`:`Saturn` + 光盘加载器 + 版本。
- **`ymir::Saturn`**(`include/ymir/sys/saturn.hpp`):核心对象,无全局状态。
  成员几乎就是整台 Saturn 的接线:
  `mem`(`sys::SystemMemory`)、`mainBus`(`sys::SH2Bus`)、`masterSH2/slaveSH2`、
  `SCU`、`VDP`、`SMPC`、`SCSP`、`CDBlock`(HLE)、`SH1/SH1Bus/CDDrive/YGR/CDBlockDRAM`(LLE)、
  `configuration`(`core::Configuration`)。
- `sys::System`(`system.hpp`):制式/时钟的小结构;`sys::Bus<addrBits,pageBits>` 页式总线
  (`SH1Bus=Bus<28,19>`,`SH2Bus=Bus<27,16>`),每页有快速数组路径 + 慢速 MMIO 函数指针路径与等待周期。
- `sys::clocks`:`ClockRatios`(320/352、NTSC/PAL)与每帧时钟数;
  `sys::memory`/`backup_ram`:IPL 512KiB、低/高 WRAM 1MiB、内部备份 32KiB、BUP 容器(mio mmap / CoW)。
- `core::Scheduler`:`Advance()` 主时钟推进,7 个固定事件(VDPPhase、SCSPSample、CDBlock…)。

### 4.2 CPU 与芯片

| 组件 | 命名空间/文件 | 说明 |
|---|---|---|
| SH-2 ×2 | `ymir::sh2::SH2`(`include/ymir/hw/sh2/`) | 主/从 CPU,解释器(解码表 switch);含 BSC/Cache/Divu/DMAC/FRT/INTC/SCI/UBC/WDT 片内外设;可选 cache 模拟;断点/观察点/Probe |
| SH-1 | `ymir::sh1::SH1` | CD 块(LLE)CPU,含 AD/ITU/PFC/TPC 等 |
| m68k | `ymir::m68k::MC68EC000` | SCSP 内嵌声音 CPU,SCSP 兼作其总线 |
| SCU + SCU DSP | `ymir::scu::SCU`(+`SCUDSP`) | DMA 仲裁、A/B 总线切换;持有卡带槽 `cart::CartridgeSlot` |
| VDP1/VDP2 | `ymir::vdp::VDP` | 见 §5 |
| SCSP + DSP | `ymir::scsp::SCSP` | 32 声道 + DSP + m68k;`SetSampleCallback` 输出 44.1kHz s16 立体声 |
| SMPC + 外设 | `ymir::smpc::SMPC`(+`RTC`) | 系统管理/端口;外设 `ControlPad/AnalogPad/ArcadeRacer/MissionStick/VirtuaGun/ShuttleMouse` |
| 卡带 | `ymir::cart::*` | None/BackupRAM/DRAM 8/32/48Mbit/ROM(KOF95、Ultraman) |

> 重要:**CPU 目前全部为解释器**,没有 JIT/重编译器(SH-1/SH-2 源码中只有 TODO)。
> 因此不存在宿主 CPU 相关的代码生成依赖。

### 4.3 CD 子系统:HLE 与 LLE

- HLE(`cdblock::CDBlock`):在 A-Bus CS2 暴露寄存器,命令解释(CR/RR/HIRQ),无需 SH-1。
- LLE:真实固件 `sh1::SH1` + 物理驱动 `cdblock::CDDrive`(串行协议)+ 门阵 `cdblock::YGR`;
  需 CD block ROM。开关 `configuration.cdblock.useLLE`(切换会硬重置)。

### 4.4 媒体层 `ymir::media`

- `media::LoadDisc(path, Disc&, preloadToRAM, cb)`:依次尝试 CHD / BIN+CUE / MDF+MDS / IMG+CCD+SUB / ISO。
- `Disc`=`Session`→`Track` 树,带 `TOCEntry`;二进制读取器:
  `File/MemoryMapped/Memory/Composite/SharedSubview/Zero`。
- CD 设备 `ICDDevice`:`ImageCDDevice`(镜像)、`HostCDDevice`(真机,功能开关)、`NullCDDevice`;
  `CDInterface` 统一入口;ISO9660 `fs::Filesystem`。
- 主机光驱实现按平台分文件(`host_cd_windows/linux/macos/dummy.cpp`)。

### 4.5 存档 / 数据库 / 调试

- `savestate/`:每组件一份纯数据状态结构 + `savestate::SaveState` 聚合(disc/IPL/CDB ROM 哈希校验)。
- `db/`:`game_db`(逐游戏 hack 与推荐卡带)、`ipl_db`、`cdb_rom_db`、`rom_cart_db`,均以 XXH128 识别。
- `debug/`:各组件 tracer 接口(`ISH2Tracer`、`ISCUTracer`…)与 `Probe` 深检;Bus 提供 `Peek/Poke`。

---

## 5. 渲染结构(前端 gfx 后端 + 核心 VDP 渲染器)

渲染分两层:

```
核心 VDP 渲染器 (核心渲染,产出 Saturn 帧)
   软件渲染器 SoftwareVDPRenderer ── CPU 帧回调 (XRGB8888)
   硬件渲染器 Direct3D12VDPRenderer ── Windows, compute shader
                 │
                 ▼  帧/纹理交还
前端图形后端 gfx (呈现与合成,把帧显示到窗口/叠加 ImGui)
   IGraphicsContext ← Null / SDLRenderer / D3D11 / D3D12 / Metal / Vulkan
```

### 5.1 前端 `gfx` 后端层(`apps/ymir-sdl3/src/app/services/gfx/`)

类型:`gfx_types.hpp`
- `enum class Backend { Null, Direct3D11, Direct3D12, Metal, Vulkan, SDLRenderer }`
  (成员由编译宏 `YMIR_PLATFORM_HAS_*` 门控);
- `kGraphicsBackends[]`、`kDefaultBackend`(Win=D3D12,Apple=Metal,Linux/FreeBSD=Vulkan,否则 SDLRenderer);
- `PresentMode { VSync, Mailbox, Adaptive, NoSync }`、`PixelFormat`、
  `TextureAccess { Static, Streaming, RenderTarget }`、`Texture2DSpec`、`AdapterID`(PCI 总线:设备.功能)。

接口与实现:
- 基类 **`gfx::IGraphicsContext`**(`gfx_context.hpp`):初始化/交换链/纹理
  Create/Update/Destroy/`RenderToTexture`/`DrawTextureRotated`/present、
  以及“显示输出纹理”获取/释放(供核心硬件渲染器回传帧)。
- 实现类(见 `gfx_context_impls.hpp`):

| Backend | 类 | 备注 |
|---|---|---|
| Null | `NullGraphicsContext` | 空操作 |
| SDL Renderer | `SDLRendererGraphicsContext` | 通用兜底(`imgui_impl_sdlrenderer3`) |
| D3D11 | `Direct3D11GraphicsContext` | Windows,占位 |
| D3D12 | `Direct3D12GraphicsContext` | Windows;自行建交换链,`imgui_impl_dx12`;暴露 `GetNextDisplayOutputTexture()` |
| Metal | `MetalGraphicsContext` | Apple(`.mm`),`CAMetalLayer`,`imgui_impl_metal` |
| Vulkan | `VulkanGraphicsContext` | 有 Vulkan 时编译 |

- 适配器枚举:`gfx_adapters` + `gfx_d3d_utils`(DXGI)/`gfx_metal_utils`。
- ImGui 绑定统一走 `GraphicsService::ImGuiInit()`。
- 两段式缩放合成:软件帧纹理(nearest)→ 显示纹理(线性),再上屏。

### 5.2 核心渲染器接口(`libs/ymir-core/.../hw/vdp/renderer/`)

- **`IVDPRenderer`**(`vdp_renderer_base.hpp`):生命周期/配置/内存写通知/VDP1、VDP2
  帧管线钩子/调试层/存档;并带 `config::RendererCallbacks`(`Callbacks` 成员:
  `VDP1DrawFinished/VDP1FramebufferSwap/VDP2ResolutionChanged/VDP2DrawFinished`)。
- `HardwareVDPRendererBase`(`vdp_renderer_hw_base.hpp`)标记为硬件渲染器。
- `enum class VDPRendererType { Null, Software, Direct3D12 }`(`vdp_renderer_defs.hpp`,D3D12 受 `YMIR_PLATFORM_HAS_DIRECT3D` 门控)。
- `VDP::UseSoftwareRenderer()/UseDirect3D12Renderer()/UseNullRenderer()`(模板 `UseRenderer<T>`),
  切换时保留 `m_swRendererCallbacks/m_d3d12RendererCallbacks` 与增强设置。

### 5.3 三种具体渲染器

| 渲染器 | 类 | 说明 |
|---|---|---|
| Null | `NullVDPRenderer` | 不画图但照常触发回调(无头) |
| Software | `SoftwareVDPRenderer`(`vdp_renderer_sw.cpp`,约 5555 行) | CPU 参考实现;可选专用 VDP1/VDP2/去隔行线程(内部用 moodycamel 队列);宿主 SIMD 快路径:SSE2/AVX2(x86)与 NEON(aarch64);结束时回调 `SoftwareRendererCallbacks::FrameComplete`(XRGB8888) |
| Hardware | `Direct3D12VDPRenderer`(Windows-only,`vdp_renderer_hw_d3d12.cpp`) | compute shader 后端:pimpl `Impl`,含设备/队列/围栏、描述符堆、`UploadRingBuffer`、`BarrierTracker`;VDP1 VRAM 用脏位图增量上传,CPU 光栅化成水平 span,shader 按 32 个特化组合绘制;VDP2 用世代计数器 + 3 个 compute shader 按行块渲染合成;帧末通过 `FrameCopyRequest` 回调请前端给可拷贝的纹理 |

### 5.4 帧如何到屏幕

- **软件路径**:`RunFrame()` 内软渲染完成 → 回调把 XRGB8888 拷入前端双缓冲 → GUI 线程上传到
  streaming 纹理 → 二段缩放 → ImGui 或全屏呈现。
- **硬件路径(Windows)**:核心 D3D12 渲染器向 `Direct3D12GraphicsContext` 请求“显示输出纹理”,
  `CopyTextureRegion` 完成后交给 GUI 线程显示。
- 提示:核心侧**目前只有 D3D12 一个硬件 VDP 后端**;Metal/Vulkan 仅存在于桌面呈现层,没有对应 VDP 计算渲染器。

### 5.5 Shader 管线

- 源:`res/shaders/src/vdp/*.hlsl`(VDP1 多边形/ VDP2 背景/精灵/合成)与 `.hlsli` 公共定义;
  前端合成 shader `res/shaders/gctx/quad_*`。
- 平台实现 `include/ymir/gpu/shaders/`:
  Windows(DXC → DXIL,可选 SPIR-V)、Unix-like(shaderc)、macOS(Metal 存根)、其它(报“不支持”)。
- 编译由 `cmake/CompileShaders.cmake` 驱动,产物经 CMakeRC 内嵌(`ymir_core_shaders`、`ymir_sdl3_shaders`)。

---

## 6. 平台差异与抽象盘点

### 6.1 平台相关代码分布(典型文件)

| 文件 | 平台内容 |
|---|---|
| `apps/ymir-sdl3/src/winmain.cpp` | Windows GUI 子系统入口 |
| `apps/ymir-sdl3/src/util/os_exception_handler.*` | Win SEH / Linux/FreeBSD signal / Apple Mach 异常(MIG 生成) |
| `apps/ymir-sdl3/src/util/os_features.*` | Win11 圆角禁用、隐藏文件属性 |
| `apps/ymir-sdl3/src/util/stdio_suppress.cpp` | Windows 隐藏控制台 |
| `apps/ymir-sdl3/src/app/profile.cpp` | Apple 便携目录特例 |
| `apps/ymir-sdl3/src/app/update_checker.cpp` | 各平台安装包文件名/URL |
| `apps/ymir-sdl3/src/app/services/gfx/*` | D3D/Metal/Vulkan 后端门控 |
| `libs/ymir-core/src/ymir/util/virtual_memory.cpp` | Win `CreateFileMapping`/POSIX `mmap` |
| `libs/ymir-core/src/ymir/util/event.cpp` | Win `WaitOnAddress` / Linux futex / FreeBSD `_umtx_op` / 其它 condvar |
| `libs/ymir-core/src/ymir/util/process.cpp`、`thread_name.cpp`、`string.cpp` | 进程优先级、线程名、宽字符转换(iconv) |
| `libs/ymir-core/src/ymir/hw/vdp/renderer/vdp_renderer_sw.cpp` | SSE/AVX2/NEON 快路径 |
| `libs/ymir-core/src/ymir/media/host_cd_*.cpp` | 主机光驱(功能开关) |

### 6.2 平台抽象服务(桌面 app)

原生文件对话框(SDL3 `SDL_ShowFileDialogWithProperties`)、目录(profile + `SDL_GetPrefPath`)、
MIDI(RtMidi)、更新检查(curl+semver)、崩溃上报、进程优先级提升、线程命名。

### 6.3 用户数据目录(profile root)

Windows `%APPDATA%\StrikerX3\Ymir`、macOS `~/Library/Application Support/StrikerX3/Ymir`、
Linux `$XDG_DATA_HOME/StrikerX3/Ymir`(或 `~/.local/share/StrikerX3/Ymir`);也支持随行(portable)
目录或 `-p <path>` 覆盖。子目录约定见 `docs/ymir-configuration.md` §4。

---

## 7. 快速文件索引

- 核心门面:`libs/ymir-core/include/ymir/sys/saturn.hpp` / `src/ymir/sys/saturn.cpp`
- 渲染接口/实现:`libs/ymir-core/include/ymir/hw/vdp/renderer/`(base/sw/null/hw_d3d12)、`src/.../vdp_renderer_sw.cpp`
- 前端主类:`apps/ymir-sdl3/src/app/app.cpp`;UI 基类 `app/ui/window_base.*`;服务 `app/services/*`
- 图形后端:`app/services/gfx/gfx_context.hpp`、`gfx_types.hpp`、`gfx_context_impl_*`
- 设置:`app/settings.{hpp,cpp}`、`settings_defaults.hpp`;核心配置 `ymir/core/configuration.hpp`
