# FurryUP AirPlay 接收器 · 源码预览

本仓库公开 FurryUP iPhone 投屏使用的**独立 OBS 原生接收器**源码。它在 OBS 进程内接收 AirPlay 视频与音频，并以 OBS 输入源输出，不需要窗口捕获。

**本次只提供源码，不分发 DLL 或安装包。** 这是工程源码预览，不代表已经完成公开二进制的构建复现、设备实测或发行验收。

## 开源范围与许可

本源码快照整体按 **GNU GPL 第 3 版（GPL-3.0-only）** 提供，完整条款见 [LICENSE](LICENSE)。上游版权和原始许可说明保留在 [NOTICE.md](NOTICE.md)、[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 与 `LICENSES/` 中；依赖仍遵循各自的许可。

公开范围仅包括本仓库列出的原生 OBS 接收器、Windows 适配补丁、语言文件、构建与安装脚本。**FurryUP Studio 主程序、账户和服务端、其他插件，以及 FurryUP Studio 内的插件管理和控制界面源码均不在本仓库，也不在本仓库的授权范围内。**

接收器可由用户在 OBS 中独立创建与配置。FurryUP Studio 通过 OBS WebSocket 的来源设置操作控制外部 OBS 进程；本仓库不包含 FurryUP Studio 的实现。

## 当前代码能力

- 接收 iPhone/iPad 的 AirPlay 镜像，将 H.264 视频和音频直接送入 OBS。
- 提供中英文属性界面、无播放器窗口的连接状态画面与 OBS 工具菜单入口。
- 支持手动 OBS 来源，以及通过来源设置控制启停与重连。
- 包含 Windows 网络适配及断线重连修复；保留与历史客户端兼容的设置字段。

当前工程目标是 **Windows x64 与 OBS 32.2.2**，使用 OBS 30.0.0 的 API 头文件和目标 OBS 提供的 FFmpeg 8.1 运行库。运行还需要 Bonjour。代码不代表兼容所有 OBS 30/31/32 版本，也未承诺受保护内容的播放能力。

## 从源码构建

需要 Visual Studio 2022 或 2026 的 x64 C++ 工具、CMake、Git 与 PowerShell。在 **x64 Visual Studio Developer Command Prompt** 中运行：

```bat
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\check-source.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\prepare-deps.ps1
build.bat
```

`prepare-deps.ps1` 按 `dependencies.lock.json` 下载、校验和准备 UxPlay、libplist、OBS 头文件、FFmpeg 开发 SDK 和 OpenSSL；它不安装系统开发工具。源码快照不携带下载缓存、依赖二进制或个人构建目录。生成的接收器位于 `build-local/`。

只检查锁文件、补丁和 overlay、不下载依赖，可运行：

```powershell
.\scripts\prepare-deps.ps1 -ValidateOnly
```

本快照保留原生接收器自己的 `install.bat`、`uninstall.bat`、`installer.iss` 与 `scripts/package-binary.ps1` 作为安装/打包实现参考。它们不构成已发布的二进制产品。公开分发任何编译产物前，仍需完成 [二进制发行待办](RELEASE-GATE.md)。

## 源码清单与来源

- [SOURCE-FILES.txt](SOURCE-FILES.txt)：本次源码的明确文件白名单。
- [SOURCE-MANIFEST.json](SOURCE-MANIFEST.json)：快照文件 SHA-256；清单本身不包含自己的哈希。
- [PROVENANCE.md](PROVENANCE.md)：上游提交、实际构建边界与许可核查事实。
- `src/`：原生接收器源码；`patches/`：UxPlay Windows 与重连适配；`dependency-overlays/`：锁定的构建适配。

```powershell
.\scripts\package-source.ps1 -OutputDirectory ..\source-packages
```

打包只读取白名单中的源文件，拒绝额外源文件、凭据、本机用户路径或编译产物，不打包 Git 历史。
