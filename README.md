# mod_audio_fork

一个 FreeSWITCH 模块，用于在通道上挂载媒体监听器，并通过 WebSocket 将 L16 音频流传输到远程服务器。本模块支持**双向音频** — 可以从服务器接收音频并实时回放给通话方，实现完整的 IVR、对话和语音机器人应用。

## 功能特性

- **双向音频** — 将音频流传输到 WebSocket 服务器，并接收音频进行实时回放
- **二进制音频流** — 从服务器接收原始二进制音频帧（除 base64 编码的 JSON 外）
- **音频标记** — 通过命名标记同步音频播放（`mark` / `clearMarks`）
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

### 命令语法

```
uuid_audio_fork <uuid> <command> [arguments...]
```

### 命令

#### start

```
uuid_audio_fork <uuid> start <wss-url> <mix-type> <sampling-rate> [bugname] [metadata] [bidirectionalAudio_enabled] [bidirectionalAudio_stream_enabled] [bidirectionalAudio_stream_samplerate]
```

挂载媒体监听器并开始将音频流传输到 WebSocket 服务器。

| 参数 | 描述 |
|---|---|
| `uuid` | FreeSWITCH 通道 UUID |
| `wss-url` | WebSocket URL（`ws://`、`wss://`、`http://` 或 `https://`） |
| `mix-type` | `mono`（仅主叫）、`mixed`（主叫 + 被叫）或 `stereo`（独立声道） |
| `sampling-rate` | `8k`、`16k` 或 8000 的任意整数倍（如 `24000`、`32000`、`64000`） |
| `bugname` | 可选的监听器名称，用于多个并发分流（默认：`audio_fork`） |
| `metadata` | 可选的 JSON 元数据，连接建立后立即作为文本帧发送 |
| `bidirectionalAudio_enabled` | `true` 或 `false` — 启用从服务器接收音频（默认：`true`） |
| `bidirectionalAudio_stream_enabled` | `true` 或 `false` — 启用来自服务器的二进制音频流 |
| `bidirectionalAudio_stream_samplerate` | 来自服务器的输入音频采样率（如 `8000`、`16000`） |

#### stop

```
uuid_audio_fork <uuid> stop [bugname] [metadata]
```

关闭 WebSocket 连接并卸载媒体监听器。可选在关闭前发送最终的文本帧。

#### send_text

```
uuid_audio_fork <uuid> send_text [bugname] <text>
```

向远程服务器发送文本帧（如 DTMF 事件、控制消息）。

#### pause

```
uuid_audio_fork <uuid> pause [bugname]
```

暂停音频流传输（帧将被丢弃）。

#### resume

```
uuid_audio_fork <uuid> resume [bugname]
```

在暂停后恢复音频流传输。

#### graceful-shutdown

```
uuid_audio_fork <uuid> graceful-shutdown [bugname]
```

启动优雅关闭 — 停止发送新音频，但允许缓冲的音频在关闭前排空。

#### stop_play

```
uuid_audio_fork <uuid> stop_play [bugname]
```

通过清空播放缓冲区来停止当前音频播放。

### 事件

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

### 服务器到模块的消息

服务器可以发送 JSON 文本帧来控制模块：

#### playAudio
向通话方回放音频（使用 base64 编码的 JSON 模式时）：
```json
{
  "type": "playAudio",
  "data": {
    "audioContentType": "raw",
    "sampleRate": 8000,
    "audioContent": "<base64编码的原始音频>"
  }
}
```

#### killAudio
停止当前音频播放并清空缓冲区：
```json
{
  "type": "killAudio"
}
```

#### mark
添加命名标记用于音频同步：
```json
{
  "type": "mark",
  "data": {
    "name": "marker-name"
  }
}
```
当播放到达标记时，模块会向服务器发送标记事件。最多可排队 30 个标记。

#### clearMarks
清除所有待处理的标记：
```json
{
  "type": "clearMarks"
}
```

#### transcription
```json
{
  "type": "transcription",
  "data": { ... }
}
```

#### transfer
```json
{
  "type": "transfer",
  "data": { ... }
}
```

#### disconnect
```json
{
  "type": "disconnect",
  "data": { ... }
}
```

#### error
```json
{
  "type": "error",
  "data": { ... }
}
```

#### 二进制音频流

当 `bidirectionalAudio_stream_enabled` 设置为 `true` 时，服务器可以直接通过 WebSocket 发送原始二进制音频帧（而非 base64 编码的 JSON）。这对实时音频流更高效。模块会处理：

- 如果服务器的采样率与通道采样率不同，自动重采样
- 预缓冲以平滑网络抖动
- 音频标记交错以实现同步

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
# 启动双向音频流传输
fs_cli -x "uuid_audio_fork <uuid> start wss://your-server.com/audio mixed 16k mybug {} true true 16000"

# 发送文本消息
fs_cli -x "uuid_audio_fork <uuid> send_text mybug {\"event\":\"dtmf\",\"digit\":\"1\"}"

# 暂停流传输
fs_cli -x "uuid_audio_fork <uuid> pause mybug"

# 恢复流传输
fs_cli -x "uuid_audio_fork <uuid> resume mybug"

# 停止并发送最终消息
fs_cli -x "uuid_audio_fork <uuid> stop mybug {\"reason\":\"complete\"}"
```

## 许可证

详见 [LICENSE](LICENSE)。
