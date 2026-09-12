# Ymir 配置功能梳理(模拟器与游戏相关)

> 说明:本文汇总 Ymir 的配置分层、`Ymir.toml` 字段、profile 目录模型、
> 逐游戏配置、区域/IPL、输入绑定、以及可在界面里调整的全部选项。
> 主要依据代码:`libs/ymir-core/.../core/configuration*` 与
> `apps/ymir-sdl3/src/app/{settings,settings_defaults,profile,shared_context}*`
> 及 `ui/views/settings/*`。

---

## 1. 配置分层与流向

```
  Ymir.toml  (profile 根)
     │ Settings::Load()/Save()   (apps/ymir-sdl3, 含版本迁移、脏检测自动保存)
     ▼
  app::Settings  (前端一个大结构,多数为 util::Observable<T>)
     │ Settings::BindConfiguration() / 观察者触发 events::emu::* 
     ▼
  ymir::core::Configuration  (运行在模拟线程上的“运行时配置”)
```

- 核心运行时可配对象是 `ymir::Saturn::configuration`(`core::Configuration`,
  `libs/ymir-core/include/ymir/core/configuration.hpp`),所有热改项都是 `util::Observable<T>`,
  观察者在模拟线程上触发。
- 前端 `Settings`(`apps/ymir-sdl3/src/app/settings.hpp`,几百行字段)负责磁盘往返与 UI;
  两者通过 `Settings::BindConfiguration()` 与各种 emu 事件同步。
- 磁盘文件:**`<profile>/Ymir.toml`**;`kConfigVersion = 5`,旧版本由加载器兼容读取,
  新版本号(>5)则提示“不支持的配置版本”。
- 保存策略:变更打脏后 250ms 防抖自动写盘(`Settings::CheckDirty()`)。

### 1.1 核心配置字段(`core::Configuration`)

| 小节 | 字段 | 默认 | 说明 |
|---|---|---|---|
| `system` | `autodetectRegion` | true | 根据光盘自动改 SMPC area code |
| | `preferredRegionOrder` | {北美, 日本, 欧洲PAL, 亚洲NTSC} | 自动判区时优先级 |
| | `videoStandard` | NTSC | 影响时序/时钟 |
| | `debugTracing` | false | 调试模式(慢) |
| | `emulateSH2Cache` | false | SH-2 cache 模拟(个别游戏需要) |
| | `sh2ClockFactor` | 100% | SH-2 时钟倍率(25%~10000%) |
| `rtc` | `mode` | Host | 主机时间或虚拟时间 |
| | `virtHardResetStrategy` | Preserve | 虚拟 RTC 在硬复位时的策略 |
| | `virtHardResetTimestamp` | 1994-01-01 | 虚拟 RTC 起点 |
| `swRenderer` | `threadedVDP1/threadedVDP2/threadedDeinterlacer` | true | 软渲染线程开关 |
| `audio` | `interpolation` | Linear | SCSP 采样插值 |
| | `threadedSCSP` | false | 独立线程跑 SCSP(暂未实现) |
| `cdblock` | `readSpeedFactor` | 2 | 读速倍率(2~200) |
| | `useLLE` | false | 低级 CD 块模拟(需要 CDB ROM,切换会硬复位) |

枚举定义在 `configuration_defs.hpp`:区域值即 SMPC area code 位
(`Japan=0x1, AsiaNTSC=0x2, NorthAmerica=0x4, EuropePAL=0xC`,另有已废弃别名);
`VideoStandard {NTSC,PAL}`、`rtc::Mode {Host,Virtual}`、
`audio::SampleInterpolationMode {NearestNeighbor,Linear}`。

---

## 2. `Ymir.toml` 字段参考(按节)

> “Obs” 表示 Observable(热更新);“[Obs]”字段同时绑定到核心。
> 未列全的默认值常量见 `settings_defaults.hpp`。

### `[General]`
| 键 | 默认 | 说明 |
|---|---|---|
| `PreloadDiscImagesToRAM` | false | 光盘镜像预载入内存 |
| `RememberLastLoadedDisc` | false | 启动时重开最近光盘 |
| `BoostEmuThreadPriority` | true | 提升模拟线程优先级 |
| `BoostProcessPriority` | true | 提升进程优先级 |
| `ScreenshotScale` | 2 | 截图缩放(1–4) |
| `EnableRewindBuffer` | false | 回退总开关 |
| `RewindCompressionLevel` | 12 | LZ4 压缩等级(0–16) |
| `MainSpeedFactor` / `AltSpeedFactor` | 1.0 / 0.5 | 主/备速度(10%–500%) |
| `UseAltSpeed` | false | 是否启用备速度 |
| `PauseWhenUnfocused` | false | 失焦暂停 |
| `UnpauseOnDiscLoad` | true | 载入光盘后取消暂停 |
| `StartPaused` | false | 启动即暂停 |
| `CheckForUpdates`、`IncludeNightlyBuilds`、`EnableDiscordPresence` | false | 更新检查 / nightly / Discord |

`[General.PathOverrides]`:对 profile 各类路径的单独覆盖(见 §4)。

### `[GUI]`
`OverrideUIScale`(false)、`UiScale`(1.0,UI 钳 1.0–2.0、25% 步进)、
`RememberWindowGeometry`、`ShowMessages`、`ShowGameNameOnTitleBar`、`ShowPerformanceOnTitleBar`、
`ShowFrameRateOSD` 与 `FrameRateOSDPosition`、`ShowSpeedIndicatorForAllSpeeds`。

### `[System]`
| 键 | 默认 | 说明 |
|---|---|---|
| `AutoDetectRegion` | true | [Obs] |
| `PreferredRegionOrder` | {NA,JP,EU-PAL,AsiaNTSC} | [Obs] |
| `VideoStandard` | NTSC | [Obs] |
| `EmulateSH2Cache` | false | |
| `SH2ClockFactor` | 100 | UI 25–500 |
| `InternalBackupRAMImagePath` | `<profile>/state/bup-int.bin` | 内部备份内存镜像 |
| `InternalBackupRAMPerGame` | false | 每游戏独立内部备份 |

`[System.IPL]`:`Override`、`Path`、`Variant`(Saturn/Hi-Saturn/V-Saturn…)。
`[System.RTC]`:`Mode`;`Virtual` → `HardResetStrategy`、`HardResetTimestamp`。

### `[Hotkeys]` 与 `[Input]`
- `[Hotkeys]`、`[Hotkeys.SaveStates]`:全部按键绑定(见 §6.4)。
- `[Input]`:`Port1`/`Port2`,各有 `PeripheralType`,及对应外设小节
  `[Input.Port#.ControlPad|AnalogPad|ArcadeRacer|MissionStick|VirtuaGun|ShuttleMouse]`
  (内含 `Binds` 数组与各外设参数);
  `[Input.Mouse]`:`CaptureMode`(SystemCursor/PhysicalMouse)、`LockToDisplay`;
  `[Input.Gamepad]`:`LSDeadzone`/`RSDeadzone`(0.15)、`AnalogToDigitalSensitivity`(0.20)。

### `[Video]`
| 键 | 默认 | 说明 |
|---|---|---|
| `GraphicsBackend` | 平台默认 | Null/D3D11/D3D12/Metal/Vulkan/SDLRenderer |
| `GraphicsAdapter` | 无 | 形如 `01:00.0` |
| `ForceIntegerScaling` / `ForceAspectRatio`/`ForcedAspect` | false/true/4:3 | 显示 |
| `Rotation` | Normal | 0/90CW/180/90CCW |
| `AutoResizeWindow` / `DisplayVideoOutputInWindow` | false | |
| `SyncInWindowedMode`/`SyncInFullscreenMode` | false/true | GUI 按渲染器同步 |
| `UseFullRefreshRateWithVideoSync` | false | 提帧到刷新率整倍数 |
| `ReduceLatency` | true | 优先展示最新帧 |
| `FullScreen`、`DoubleClickToFullScreen`、`BorderlessFullScreen` | false/false/true | |
| `UseHardwareAcceleration` | false | [Obs] 实验性 GPU VDP 渲染 |
| `FullScreenDisplay`(`Name/X/Y`)、`FullScreenMode`(`Width/Height/PixelFormat/RefreshRate/PixelDensity`) | | 全屏设备与模式 |

`[Video.SoftwareRenderer]`:`ThreadedVDP1/ThreadedVDP2/ThreadedDeinterlacer`。
`[Video.Enhancements]`:`Deinterlace`(高分辨率逐行)、`TransparentMeshes`(透明 mesh)。

### `[Audio]`
`Volume`(0.8)、`Mute`、`InterpolationMode`(Nearest/Linear)、`ThreadedSCSP`、
`StepGranularity`(0–5 精度滑杆)、`MidiInputPortId/Type`、`MidiOutputPortId/Type`。

### `[Cartridge]`
`Type`(None/BackupRAM/DRAM/ROM)、`BackupRAM.ImagePath/Capacity`(4/8/16/32Mbit)、
`DRAM.Capacity`(48/32/8Mbit)、`ROM.ImagePath`、`AutoLoadGameCarts`(默认 true)。

### `[CDBlock]`
`ReadSpeed`(2)、`UseLLE`(false)、`OverrideROM`、`RomPath`。

---

## 3. 与“游戏”相关的配置机制

### 3.1 内置游戏数据库 `ymir::db`
`game_db.hpp/cpp` 按 product code 或光盘 XXH128 查 `GameInfo`:
- **推荐卡带**(`Cart_MASK`)与 hack 标志:`ForceSH2Cache`、`FastBusTimings`、
  `FastMC68EC000`、`StallVDP1OnVRAMWrites`、`SlowVDP1`、`VirtuaGunJitter` 等。
- 装载光盘时核心按标志自动调整(`ConfigureAccessCycles`、`ForceSH2CacheEmulation`)。
- UI 上“Emulate SH-2 cache”会被游戏强制并禁用勾选;设置 Tab 显示“当前光盘推荐卡带”提示并可一键插入。

### 3.2 自动卡带(`ROMService::LoadRecommendedCartridge`)
`AutoLoadGameCarts` 开启时按数据库自动插 DRAM 8/32/48Mbit;KOF95/Ultraman 等 ROM 卡按哈希匹配;
`Cart_BackupRAM` 游戏自动生成 `<profile>/backup/games/bup-ext-32M-<game>.bin`。

### 3.3 按游戏隔离的数据
- 内部备份 RAM 每游戏版:`backup/games/bup-int-<game>.bin`(`InternalBackupRAMPerGame`)。
- 存档:`savestates/<discHash>/<slot>.savestate`(+ `-1` 备份 + `meta.txt`),共 10 槽;
  载入时校验 disc/IPL/CDB 哈希,缺失可自动去 `roms/ipl|cdb` 换对应 ROM。
- 除此以外**没有**按游戏分立的配置文件;游戏特例来自内置 game_db。

### 3.4 IPL / 区域
- 区域即 SMPC area code;`UsePreferredRegion()`/`AutodetectRegion()` 逻辑在 `saturn.cpp`。
- BIOS 自动选择:`roms/ipl/` 目录扫描→按“首选变体(Saturn/HiSaturn/VSaturn)+ 区域”匹配,
  支持 region-free;IPL Tab 里可手动指定。
- SMPC 持久数据写入 `<profile>/state/smpc-<region>.bin`。

---

## 4. Profile 目录模型(`ProfilePath`)

| 枚举 | 子目录 | 内容 |
|---|---|---|
| Root | `<profile>/` | `Ymir.toml`、`gamecontrollerdb.txt` |
| IPLROMImages | `roms/ipl/` | Saturn BIOS |
| CDBlockROMImages | `roms/cdb/` | CD 块 ROM(LLE) |
| ROMCartImages | `roms/cart/` | ROM 卡带(KOF95/Ultraman) |
| BackupMemory | `backup/`(+`games/`) | 备份内存镜像 |
| ExportedBackups | `backup/exported/` | 导出存档 |
| PersistentState | `state/` | `bup-int.bin`、`smpc-*.bin`、窗口几何、`recent_discs.txt` |
| SaveStates | `savestates/<discHash>/` | 存档 |
| Dumps / Screenshots | `dumps/`、`screenshots/` | 转储 / 截图 |

profile 解析优先级:命令行 `-p` → `-u`(强制用户目录)→ 当前目录便携 → 可执行文件旁便携 →
OS 用户目录;首个带 `Ymir.toml` 者胜出;都没有则首次运行弹窗选择(Flatpak 强制用户目录)。
路径可在 UI(设置→General)单独覆盖,TOML 中存相对、加载时再绝对化。

---

## 5. 设置 UI 的 11 个 Tab 一览

| Tab | 主要可调内容 | 视图文件 |
|---|---|---|
| General | 优先级提升、镜像预载、记住光盘;速度(主/备 10–500%);暂停行为;更新;截图倍率;回退压缩;profile 路径覆盖 | `general_settings_view.cpp` |
| GUI | 标题栏显示项、UI 缩放、窗口几何、消息/帧率/速度 OSD | `gui_settings_view.cpp` |
| Hotkeys | 全部快捷键表格与恢复默认 | `hotkeys_settings_view.cpp` |
| System | 区域(制式/区域/自动判区/优先级列表);SH-2 cache、SH-2 时钟;RTC;内部备份每游戏 | `system_settings_view.cpp` |
| IPL | 扫描到的 BIOS 表、变体、覆盖 | `ipl_settings_view.cpp` |
| Input | 双端口外设选择、鼠标捕获、手柄 DB、死区与灵敏度、各外设按键编辑 | `input_settings_view.cpp` + 各 `*_config_view.cpp` |
| Video | 图形后端/适配器、硬件加速(实验);软渲染线程;增强(去隔行/透明 mesh);显示/全屏/同步 | `video_settings_view.cpp` |
| Audio | 音量/静音;插值;MIDI;步进粒度;线程化 SCSP | `audio_settings_view.cpp` |
| Cartridge | 当前卡带、自动推荐、类型与容量、镜像路径 | `cartridge_settings_view.cpp` |
| CDBlock | LLE 开关、CDB ROM 表、读速 | `cdblock_settings_view.cpp` |
| Tweaks | 只读诊断文本、增强/精度/性能三组预设 | `tweaks_settings_view.cpp` |

---

## 6. 输入

### 6.1 支持的外设(每端口一个)
`None / ControlPad(手柄)/ AnalogPad(3D 手柄)/ ArcadeRacer / MissionStick / VirtuaGun / ShuttleMouse`,
默认 Port1=ControlPad、Port2=None。

### 6.2 绑定模型
`InputBind` = 一个动作 + 最多 5 个 `InputElement`;元素可为按键组合(`Ctrl+O`)、
鼠标键/轴、手柄键/轴(`GamepadA@0`,`@N` 为手柄号)。写盘格式为动作名→字符串数组。

### 6.3 默认键位(节选)
Port1 键盘:方向 WASD, A=J/B=K/C=L/X=U/Y=I/Z=O/L=Q/R=E, Start=F/G/H;
Port2 用小键盘 + 手柄 1;手柄侧沿用 A/B/X/Y/LB/RB 等映射。Virtua Gun:鼠标左=扳机、右=重装。

### 6.4 常用热键(节选)
F10 设置、Alt+Enter 全屏、F12 截图、Shift+F1 帧率、Ctrl+` 旋转、Ctrl+M 静音、
Ctrl+=/- 音量、Ctrl+O/W/T 光盘、Ctrl+R/Shift+R 复位、Tab/` 加速、Pause 暂停、
[ ] 前后单帧、Backspace 回退、F2/F3 快速存取、数字键选槽、Shift+数字 存 / Ctrl+数字 读。

---

## 7. 命令行参数

`ymir-sdl3 [OPTIONS] [光盘路径]`
`-d/--disc`、`-p/--profile`、`-u/--user`、`-f/--fullscreen`、`-P/--paused`、
`-F/--fast-forward`、`-D/--debug`(开 trace)、`-E/--exceptions`(捕获异常)、`-h/--help`。

`ymir-headless`:`--ipl/--game/--config/--bram/--slave|--no-slave`;配置合并
CLI > `Ymir-dbg.toml` > `Ymir.toml` > 默认,只认安全子集键。

---

## 8. 画质 / 性能 / 精度可调点小结

- 渲染精度与增强:`Video.Enhancements`、`Video.SoftwareRenderer` 线程、`UseHardwareAcceleration`(实验)。
- 画面呈现:整数缩放、强制宽高比、旋转、全屏设备/模式、窗口同步、全刷新率、减延迟。
- 无“内部分辨率缩放”滑杆 —— 软渲染输出原生分辨率,显示端统一做缩放。
- 系统/精度:`EmulateSH2Cache`、`SH2ClockFactor`、音频插值/步进粒度、CD 块 LLE 与读速。
- 速度控制:主/备速度因子、Turbo、前后单帧、回退(约 1 分钟 @60fps)。
- 当前**硬编码不可配**:音频 44.1kHz S16 立体声、缓冲 512 采样、设备选择(代码注释 TODO)。
- 回退缓冲内存上限:60×60 帧(LZ4 XOR 增量),压缩等级可调。
