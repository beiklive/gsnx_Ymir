# Ymir 移植到 Nintendo Switch —— 可行性研究与实践进度

> 状态说明:本文整理 Switch 目标(devkitA64/libnx)的可行性研究结论、仓库内
> 已做的改动、当前进度与剩余工作。**移植实现当前暂停**,以文档产出优先;
> 已完成“交叉编译 `ymir-core` 通过”这一步。文中带 `[未做]` 标注的是尚未验证的部分。

---

## 1. 结论摘要

- **可行**,条件是:把 `ymir-core` 当作库使用,并**新写一个最小前端**(桌面 SDL3/ImGui/vcpkg
  前端在 Switch 上不可复用)。
- 模拟核心可移植性极好:SH-1/SH-2/m68k、SCU/SCSP DSP 全部是**解释器**,无 JIT/x86 依赖;
  软件 VDP 渲染器只依赖 aarch64 NEON(devkitA64 默认 armv8-a 自带)。
- 主要障碍是少量 OS 抽象(newlib 缺 `sys/mman.h`、无 `setpriority`、`iconv` 有头无实现等),
  已用很小的补丁 + 一个 mmap shim 库解决。
- 渲染策略:**不用** D3D12(不可用)/Vulkan 计算渲染器(core 目前没有 Vulkan VDP 渲染器),
  而是跑 core 的**软件 VDP 渲染器**,把 CPU XRGB8888 帧交给前端上屏。

## 2. 环境

- `DEVKITPRO=/opt/devkitpro`,devkitA64 **GCC 15.2.0**(C++20 完整)。
- `Switch.cmake` 工具链齐全,`Platform/NintendoSwitch.cmake` 提供 `nx_create_nro()`(elf2nro/nacp)。
- portlibs 有 SDL2 2.28(SDL2_ttf/image/mixer/net)、libchdr 自带 switch CI 先例;`libnx` 4.x。
- 无 `sys/mman.h`、无 `dlfcn/execinfo`、`iconv.h` 无实现;无 vcpkg triplet;无 SDL3。
- Vulkan(用户自维护):仓库内 `nvk-switch/` = Mesa NVK(Switch)静态 ICD,见 §6。

## 3. CMake / 工具链层面的判定(实测)

配置命令(验证通过):

```sh
cmake -S . -B build-switch -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
  -DYmir_ENABLE_IPO=OFF -DYmir_DEV_BUILD=OFF \
  -DYmir_ENABLE_TESTS=OFF -DYmir_ENABLE_SANDBOX=OFF -DYmir_ENABLE_YMDASM=OFF \
  -DYmir_ENABLE_YMIR_HEADLESS=OFF -DYmir_ENABLE_YMIR_DBG=OFF
```

- `CMAKE_SYSTEM_NAME=NintendoSwitch`,`WIN32/APPLE/UNIX` 全假 → 自动走 `gpu_shaders_other.cpp`、
  跳过 iconv 链接、host_cd 用 dummy、架构判定为 `arm64`。
- Vulkan `find_package` 缺失**不致命**;无 DXC 时 shader 步骤自动空转(已实测)。
- 需要修的 CMake 关卡(均已加 `NintendoSwitch` 判定,不影响桌面):
  - 顶层 `find_package(Vulkan)` 增加 `AND NOT NintendoSwitch`;
  - `vendor/CMakeLists`:Switch 下跳过 imgui/lz4/discord-rpc(SDL3/nlohmann_json 依赖);
  - `vendor/libchdr/CMakeLists`:Switch 下走**自带 deps**(miniz/zstd/lzma)分支(等同 `Ymir_LIBRARY_ONLY`);
  - `apps/CMakeLists`:Switch 下跳过 `ymir-sdl3`、加入 `ymir-switch`;
  - 子模块 `vendor/mio` 需先 `git submodule update --init`(曾为空检出错)。

## 4. 核心可移植性:发现与处理

| 位置 | 问题 | 处理 |
|---|---|---|
| `mio`(进 PCH)、`util/virtual_memory.cpp` | include `<sys/mman.h>` | 新增 **`libs/switch-compat`**(mmap/munmap/msync shim,见下) |
| `util/process.cpp` | `setpriority/readlink/pthread_getschedparam` | 加 `__SWITCH__` 空分支 [已改] |
| `util/string.cpp` | `iconv` 链接缺失 | `__SWITCH__` 下手写 UTF-8↔UTF-32 [已改] |
| `util/thread_name.cpp` | 无 switch 分支 | `__SWITCH__` no-op [已改] |
| concurrentqueue | 未命中平台宏 → `#error` | 全局 `-D__unix__` [已改] |
| fmt | `putc_unlocked/flockfile` 不可见 | 全局 `-D_DEFAULT_SOURCE` [已改] |
| `vdp_renderer_sw.cpp` NEON | GCC 严格类型检查 | 全局 `-flax-vector-conversions` [已改] |

**mmap shim(`libs/switch-compat`)设计**:文件映射=一次性读入堆缓冲并登记;`munmap`/`msync`
对 MAP_SHARED+可写映射尽量回写;匿名映射=堆分配。对 Ymir 用法足够:
- 软渲染线程关闭(`threadedVDP* = false`),单线程跑帧;
- 光盘加载统一 `preloadToRAM=true` → 走 `MemoryBinaryReader`,运行时不需要文件 mmap;
- 备份内存仅用内存镜像(暂不做 mmap 文件落盘)。

## 5. 现状(仓库内已落地但未全部验证)

已改文件(全部对桌面构建透明):
- 根 `CMakeLists.txt`(Switch 探测 + `-D__unix__ -D_DEFAULT_SOURCE -flax-vector-conversions`)
- `vendor/CMakeLists.txt`、`vendor/libchdr/CMakeLists.txt`
- `apps/CMakeLists.txt`、`apps/ymir-switch/{CMakeLists.txt, src/main.cpp, res/font.ttf}`
- `libs/switch-compat/{CMakeLists.txt, include/sys/mman.h, src/mman.cpp}`
- `libs/ymir-core/CMakeLists.txt`(链接 switch-compat)
- `libs/ymir-core/src/ymir/util/{process.cpp, string.cpp, thread_name.cpp}`

验证进度:
- ✅ Switch 下 CMake configure 全绿;`ymir-core` 静态库交叉编译 **166/166 通过**。
- ⏳ `ymir-switch`(.nro)尚未完整编译(**暂停中**);后续需按 3.1 一节的 `nx_create_nro` 收尾、
  并解决 SDL2/链接/字体 romfs 等首轮报错。
- ⏳ 无实机,运行行为(文件列表、启动、帧率)未验证。

### 5.1 `apps/ymir-switch`(最小前端草稿,功能)

- SDL2 + SDL2_ttf,romfs 内嵌字体(`romfs:/font.ttf`);浏览 `sdmc:/`。
- 两个阶段:先选 512KiB Saturn IPL/BIOS,再选光盘(`.cue/.chd/.iso/.ccd/.mds`)。
- 启动:`ymir::Saturn` + `LoadIPL` + `media::LoadDisc(path, disc, /*preloadToRAM=*/true, cb)`
  → `UseSoftwareRenderer()`;绑定 `VDP2ResolutionChanged` 与软件帧回调(XRGB8888)→ SDL 纹理等比上屏;
  SCSP 采样回调攒块 `SDL_QueueAudio`(同时作帧节奏);SMPC Port1 接 `ControlPad`,Joy-Con 映射输入。
- 手柄:A 确定、B 返回/退出游戏、方向键/摇杆移动、+ 退出。
- 目录约定:IPL 放任意位置手动选;存档/备份/设置暂不落盘(`[未做]`)。

## 6. 关于用户自维护的 `nvk-switch`(Switch Vulkan 驱动)

- 内容:Vulkan SDK 头 + `lib/libvulkan.a`(约 47MB,987 个 obj)。
  从符号看是 **Mesa NVK for Switch 的 ICD**(含 `nvk_loaderless_shim.o`、`wsi_switch.c.o`、
  `drm_shim.o`),导出 `vk_icdNegotiateLoaderICDInterfaceVersion`、`vk_icdGetInstanceProcAddr`,
  **没有** `vkCreateInstance` 等加载器入口;磁盘上也没找到配套 Loader/ICD JSON。
- 参考实现(用户指点):`/Users/beiklive/Code/C++/GBAStation_ppsspp` 链接的是同一份库。
  其接入方式是 **loaderless**:见 `Common/GPU/Vulkan/VulkanLoader.cpp`
  - 用 `__asm__("vk_icdGetInstanceProcAddr")` 别名函数直连 ICD 导出符号;
  - 组装自己的 `vkGetInstanceProcAddr/vkCreateInstance`(实例句柄为空时取全局函数);
  - CMake 侧 `-Wl,--whole-archive libvulkan.a` 整体打入;
  - 该仓库 Switch 版用 `VK_NN_vi_surface`(`WINDOWSYSTEM_SWITCH` + `vkCreateViSurfaceNN`)
    建 surface,再用 `VK_KHR_swapchain` 呈现。
- 结论与取舍:`[未做]` —— 若要把它用于 Ymir,可行路径是**把前端呈现从 SDL 软渲染改成
  “软件 VDP 渲染出 CPU 帧 + Vulkan blit 上屏”**(core 本身没有 Vulkan VDP 渲染器,
  新写 Vulkan compute VDP 渲染器工作量巨大,不建议在本阶段做)。细节(surface/交换链、
  手柄窗口指针来源、是否需要 SDL 提供窗口)需对照 PPSSPP 的 Switch main 与 switchVK WSI 约定确认。

## 7. 剩余工作清单(将来恢复移植时)

1. 完成 `apps/ymir-switch` 首轮交叉编译,修到 `.nro` 产出(SDL2/ttf/romfs/链接库顺序)。
2. 复核 PCH + GCC 交叉编译稳定性;必要时对核心关 PCH。
3. (可选)把呈现改为 Vulkan:`nvk-switch` loaderless + `VK_NN_vi_surface` + blit,参照 PPSSPP。
4. 运行时配置/存档/备份落盘;IPL/光盘路径记忆;双核绑定与性能剖析。
5. 实机(或至少 Ryujinx/Yuzu 类环境)验证文件列表与启动流程。

## 8. 参考

- 核心文档与用法:`libs/ymir-core/docs/mainpage.hpp`(加载/渲染/输入/音频回调一站式示例)。
- 桌面参考接线:`apps/ymir-sdl3/src/app/app.cpp`(回调绑定、事件循环)。
- devkitPro:`/opt/devkitpro/examples/switch/{graphics/sdl2, fs/sdmc, audio/sdl2-audio}`。
- 用户 Vulkan 参考:`GBAStation_ppsspp` 的 `VulkanLoader.cpp` / `CMakeLists.txt`。
