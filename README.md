# Sleep in Bed SKSE · 中文汉化版（v1.3.3）

汉化 [Sleep in Bed SKSE](https://www.nexusmods.com/skyrimspecialedition/mods/189803)
（Nexus 189803，作者 Elzar125）的 SKSE 插件 DLL。躺下再睡、随从跟睡/同床、睡前卸装。

## 上游与版权

- 上游源码：Nexus 页面 MISC 区 `Sleep In Bed SKSE - Source Code v1.3.3`（作者未建公开 Git 仓库）。
- 许可证：MIT（见 `LICENSE`），原样保留。
- 本仓库 = 上游 v1.3.3 源码 + 中文汉化提交。上游更新时，用新源码包覆盖 `src/`、`include/` 即可增量重编。

## 汉化方式（源码编译 + I18N 外置 JSON，绝不二进制打补丁）

- DLL 内保留英文原文；翻译全部外置在 `SleepInBedSKSE.json`（扁平 `{"英文原文": "中文"}`，
  与 DLL 同名，插件自身只读写 `.ini`，该名无占用）。
- **删掉 JSON 即还原英文**；改词条即改界面，无需重编译。
- 实现：`include/Localization.h`、`src/Localization.cpp`（rapidjson 加载，缺 key 回退英文），
  `src/Menu.cpp` 全部 UI 字符串经 `_T()` 查表；`_T` 定义见 `include/Localization.h`。
- `SleepInBedSKSE.ini` 注释已汉化（键值不动，UTF-8 无 BOM）。

## 与上游的差异（除汉化外）

- `src/plugin.cpp` 为本仓库重写（上游源码包不含入口文件）：按模块接口实现
  `SKSEPluginLoad`，`kDataLoaded` 时查表（棺材/换装动画），`kNewGame`/`kPreLoadGame` 时清状态。
- `include/PCH.h` 加 `NOMINMAX`（新版 SDK 必需）。
- `extern/SKSEMenuFramework.h` 取自 QTR-Modding/SKSE-Menu-Framework-3（动态加载，无需链接）。
- 构建基于 CommonLibVR（作者原用 `extern/CommonLibVR`），`--skyrim_vr=y`，SE/AE/VR 通用。

## 构建

```bat
set XMAKE_GLOBALDIR=C:\xmake-data
set XMAKE_PKG_CACHEDIR=C:\xmake-cache
xmake f -c -p windows --toolchain=msvc --vs=2026 --skyrim_vr=y -y
xmake build -y
```

依赖：`E:/CommonLibVR`（alandtse/CommonLibVR，`ng` 分支；需 `extern/openvr/headers`，
可用 `--filter=blob:none --sparse` 只拉 headers）、xmake 包 `rapidjson`/`simpleini`/`minhook`。
产物：`build/windows/x64/releasedbg/SleepInBedSKSE.dll` + 本仓库 `SleepInBedSKSE.json`，
放入 `Data/SKSE/Plugins/`。
