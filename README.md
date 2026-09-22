# mod_audio_fork

一个 FreeSWITCH 模块，用于在通道上挂载媒体监听器，并通过 WebSocket 将 L16 音频流传输到远程服务器。同时支持**原生下行流式播放** — 注册 `SWITCH_FILE_INTERFACE` 文件驱动（`audio_fork://`），可直接通过 HTTP Chunked 模式拉取并播放裸线性 PCM16 音频流，作为 `mod_shout` 的现代高性能零编解码替代方案。

## 功能特性

- **上行流式分流** — 将音频流通过 WebSocket 实时传输到远程 ASR 识别服务器
- **下行流式播放** — 原生 `SWITCH_FILE_INTERFACE` 文件驱动，直接拉取 HTTP Chunked PCM16 流播放
- **零编解码延迟** — 下行裸 PCM16 直接送入信道，彻底消除 MP3 编解码延迟与 CPU 消耗
- **严格整采样对齐** — 读写缓冲均按整采样边界对齐，彻底杜绝奇数字节截断白噪音
- **起播预缓冲与时钟防抖** — 默认 200ms 起播预缓冲，欠载自动补静音维持 20ms RTP 时钟
- **尾音排空确认** — 传输结束且缓冲区读空后持续 3 帧确认排空，杜绝吞尾字
- **秒级极速打断** — 关闭播放时主动 shutdown 套接字，瞬间解除 curl 阻塞极速释放
- **多种混音类型** — 单声道（仅主叫）、混合（主叫 + 被叫）或立体声（独立声道）
- **灵活的采样率** — 8000、16000、24000、32000、48000、64000 Hz（8000 的任意整数倍）
- **自动重采样** — 内置 Speex 重采样器，用于采样率转换
- **TLS 支持** — 安全 WebSocket 连接（wss://）
- **SIMD 优化** — AVX2/SSE2 向量数学运算用于音频处理
- **优雅关闭** — 关闭连接前排空音频缓冲区

## 环境变量

| 变量 | 描述 | 默认值 |
|---|---|---|
| `MOD_AUDIO_FORK_SUBPROTOCOL_NAME` | WebSocket [子协议](https://tools.ietf.org/html/rfc6455#section-1.9)名称 | `audio.drachtio.org` |
| `MOD_AUDIO_FORK_SERVICE_THREADS` | libwebsocket 服务线程数（1–5） | `1` |
| `MOD_AUDIO_FORK_BUFFER_SECS` | 音频缓冲区大小（秒）（1–5） | `2` |

## 通道变量

| 变量 | 描述 |
|---|---|
| `MOD_AUDIO_BASIC_AUTH_USERNAME` | WebSocket 连接的 HTTP Basic Auth 用户名 |
| `MOD_AUDIO_BASIC_AUTH_PASSWORD` | WebSocket 连接的 HTTP Basic Auth 密码 |
| `MOD_AUDIO_FORK_ALLOW_SELFSIGNED` | 允许自签名 TLS 证书（`true`/`false`） |
| `MOD_AUDIO_FORK_SKIP_SERVER_CERT_HOSTNAME_CHECK` | 跳过 TLS 主机名验证（`true`/`false`） |
| `MOD_AUDIO_FORK_ALLOW_EXPIRED` | 允许过期的 TLS 证书（`true`/`false`） |

## API

### 1. 上行分流命令 (`uuid_audio_fork`)

#### 命令语法

```
uuid_audio_fork <uuid> <command> [arguments...]
```

#### 命令

##### start

```
uuid_audio_fork <uuid> start <wss-url> <mix-type> <sampling-rate> [bugname] [metadata]
```

挂载媒体监听器并开始将音频流传输到 WebSocket 服务器。

| 参数 | 描述 |
|---|---|
| `uuid` | FreeSWITCH 通道 UUID |
| `wss-url` | WebSocket URL（`ws://` 或 `wss://`） |
| `mix-type` | `mono`（仅主叫）、`mixed`（主叫 + 被叫）或 `stereo`（独立声道） |
| `sampling-rate` | `8k`、`16k` 或 8000 的任意整数倍（如 `24000`、`32000`、`64000`） |
| `bugname` | 可选的监听器名称，用于多个并发分流（默认：`audio_fork`） |
| `metadata` | 可选的 JSON 元数据，连接建立后立即作为文本帧发送 |

##### stop

```
uuid_audio_fork <uuid> stop [bugname] [metadata]
```

关闭 WebSocket 连接并卸载媒体监听器。可选在关闭前发送最终的文本帧。

##### send_text

```
uuid_audio_fork <uuid> send_text [bugname] <text>
```

向远程服务器发送文本帧（如 DTMF 事件、控制消息）。

##### pause

```
uuid_audio_fork <uuid> pause [bugname]
```

暂停音频流传输（帧将被丢弃）。

##### resume

```
uuid_audio_fork <uuid> resume [bugname]
```

在暂停后恢复音频流传输。

##### graceful-shutdown

```
uuid_audio_fork <uuid> graceful-shutdown [bugname]
```

启动优雅关闭 — 停止发送新音频，但允许缓冲的音频在关闭前排空。

---

### 2. 下行原生流式播放驱动 (`audio_fork://`)

注册 FreeSWITCH 原生 `SWITCH_FILE_INTERFACE` 文件驱动（协议头 `audio_fork://`），通过 HTTP Chunked 异步拉取裸线性 PCM16 流并播放。

#### 语法格式

```
playback(audio_fork://http[s]://<host>:<port>/<path>[?query_parameters])
```

#### 支持的 URL 查询参数

| 参数名 | 描述 | 默认值 | 取值范围与说明 |
|---|---|---|---|
| `rate` / `sampling` | 音频采样率 (Hz) | `16000` | `8000` ~ `48000`（FreeSWITCH 核心按此采样率与通道自动重采样） |
| `channels` | 音频声道数 | `1` | `1`（单声道 Mono）或 `2`（立体声 Stereo） |
| `prebuffer` | 起播预缓冲时长 (ms) | `200` | `1` ~ `2000` 毫秒。积攒指定毫秒数据后即刻秒起播，杜绝欠载斩波 |
| `watchdog` | 静音看门狗超时时间 (ms) | `3000` | 连续无数据输入达此时长后安全退出播放；设为 `0` 可关闭看门狗 |

#### 调用示例

```bash
# 在 fs_cli 中直接向通道播放实时 HTTP PCM 流
fs_cli -x "uuid_broadcast <uuid> audio_fork://http://127.0.0.1:9080/tts-stream/140581e86ae8eff9.pcm?rate=16000&channels=1 aleg"

# 在 Outbound ESL (Node/Bun) 中下发播放指令
await session.execute("playback", "audio_fork://http://127.0.0.1:9080/tts-stream/xxx.pcm?rate=16000&channels=1");

# 配合 uuid_break 毫秒级打断播放
fs_cli -x "uuid_break <uuid> all"
```

## 事件

本模块生成以下 FreeSWITCH 自定义事件：

| 事件 | 描述 |
|---|---|
| `mod_audio_fork::connect` | WebSocket 连接成功建立 |
| `mod_audio_fork::connect_failed` | WebSocket 连接失败（消息体包含原因） |
| `mod_audio_fork::disconnect` | WebSocket 连接关闭或服务器发送断开请求 |
| `mod_audio_fork::buffer_overrun` | 音频缓冲区溢出 — 帧正在被丢弃 |
| `mod_audio_fork::transcription` | 服务器发送了转录消息 |
| `mod_audio_fork::transfer` | 服务器发送了转接请求 |
| `mod_audio_fork::play_audio` | 服务器发送了用于播放的音频 |
| `mod_audio_fork::kill_audio` | 服务器请求停止当前音频播放 |
| `mod_audio_fork::error` | 服务器报告了错误 |
| `mod_audio_fork::json` | 服务器发送了通用 JSON 消息 |

## 构建

详细构建说明请参见 [BUILD.md](BUILD.md)。

### 快速开始

```bash
# 安装依赖、构建并安装
chmod +x build.sh
sudo ./build.sh all

# 或逐步执行：
sudo ./build.sh deps      # 安装构建依赖
./build.sh build           # 构建模块
sudo ./build.sh install    # 安装到 FreeSWITCH
```

## 使用示例

```bash
# 启动上行音频流传输 (ASR)
fs_cli -x "uuid_audio_fork <uuid> start ws://127.0.0.1:10096 mono 16k mybug {}"

# 发送文本消息
fs_cli -x "uuid_audio_fork <uuid> send_text mybug {\"event\":\"dtmf\",\"digit\":\"1\"}"

# 暂停流传输
fs_cli -x "uuid_audio_fork <uuid> pause mybug"

# 恢复流传输
fs_cli -x "uuid_audio_fork <uuid> resume mybug"

# 停止上行流传输
fs_cli -x "uuid_audio_fork <uuid> stop mybug {\"reason\":\"complete\"}"

# 下行流式播放 (TTS)
fs_cli -x "uuid_broadcast <uuid> audio_fork://http://127.0.0.1:9080/tts-stream/xxx.pcm?rate=16000&channels=1 aleg"
```

## 许可证

详见 [LICENSE](LICENSE)。
