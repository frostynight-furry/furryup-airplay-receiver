# 架构说明

## 0.1.0：OBS 进程内接收

当前版本直接把 UxPlay 的 RAOP/AirPlay 回调接入 OBS：

```text
Bonjour / mDNS
      |
      v
UxPlay RAOP callbacks
      |
      +-- H.264 -> FFmpeg decoder -> NV12 frame -> obs_source_output_video2
      |
      +-- AAC/ALAC/PCM -> FFmpeg decoder -> PCM -> obs_source_output_audio
```

OBS 将本插件注册为 `OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO` 输入源。
接收线程直接提交带时间戳的异步帧，所以桌面上没有需要捕获的播放器窗口。

为了避免“安装后没有入口”的体验，插件在 `obs_module_post_load` 后通过 OBS
frontend API 注册“工具 → iPhone 投屏控制台（AirPlay）…”。该动作只负责在当前
场景创建/复用来源并打开 OBS 原生属性窗口；frontend 函数从宿主 DLL 动态解析，
因此不引入 Qt 构建依赖，也不会改变无播放器窗口的数据链路。

这条链路用最小改动解决首要问题，并能尽快验证延迟、音画同步、横竖屏和长时间
直播稳定性。

0.1.0 仅面向 OBS 32.2.2，并复用宿主已加载的 FFmpeg 8.1 DLL。Windows 不会按
插件目录隔离同名 DLL，因此不能在 OBS 进程内并排放置另一套“私有 FFmpeg”。
跨 OBS 版本的依赖隔离由下述 worker 架构解决。

## 生产版：独立 worker

0.1.0 验证成功后，推荐把网络协议和解码器移出 OBS 进程：

```text
OBS source plugin (MSVC)
      ^
      | shared memory: video frames / PCM / timestamps
      | named pipe: control / status / reconnect
      v
AirPlay worker (UxPlay + decoder)
```

这样可以避免第三方网络协议、OpenSSL 和 FFmpeg 故障直接拖垮 OBS，也能让 worker
独立重启。共享内存槽位应采用固定头部、版本号、帧序号和双缓冲/环形缓冲；控制
管道只传小消息，不传视频帧。

## 端口

- TCP 7000：RTSP/HTTP
- TCP 7100：镜像数据
- UDP 6000：音频数据
- UDP 6001：控制
- UDP 7011：时间同步
- UDP 5353：Bonjour/mDNS（系统服务）

0.1.0 使用固定端口，便于添加最小防火墙规则。原版 `uxplay-windows` 必须退出，
否则可能发生端口冲突。

## 生命周期

- 创建 OBS 源时启动 AirPlay 接收器。
- 修改接收器名称、分辨率、FPS 或设备标识时安全重启接收器。
- 删除 OBS 源时先停止 HTTP/RTP 回调，再注销 Bonjour，最后释放解码器。
- iPhone 断开时向 OBS 输出空视频帧，避免保留陈旧画面。

### FurryUP Studio 按需控制

OBS 手工创建的来源保持原有行为：创建后立即启动接收器。FurryUP Studio 创建来源时应显式写入
`managed_by_fus=true`，并通过 `receiver_enabled` 控制接收器生命周期：

- OBS 新建或重载 `managed_by_fus=true` 的来源时，原生模块会无条件把
  `receiver_enabled` 复位为 `false`。即使场景集合曾持久化 `true`，也不会在 FurryUP Studio
  尚未启动插件时自行占用端口；本次 OBS 会话必须再收到一次 FurryUP Studio 的
  `SetInputSettings(receiver_enabled=true)` 才能启动。
- `managed_by_fus=true, receiver_enabled=false`：保留 OBS 来源，但不创建或继续运行
  RAOP/Bonjour 服务，不占用接收端口。
- `managed_by_fus=true, receiver_enabled=true`：启动接收器；从 `true` 改为 `false` 时
  完整停止协议线程并释放端口。
- `managed_by_fus=false` 或字段缺失：忽略 `receiver_enabled`，始终按手工来源方式启动，
  兼容旧场景集合和第三方调用方。

## 时间戳

AirPlay 提供墙上时钟时间戳，OBS 使用单调时钟。插件在每次连接时建立时间锚点，
把远端时间映射到 `os_gettime_ns()`；如果漂移超过三秒则重新锚定，避免异常时间戳
破坏音画同步。
