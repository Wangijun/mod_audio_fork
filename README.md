# mod_audio_fork

一个 FreeSWITCH 模块，用于在通道上挂载媒体监听器，并通过 WebSocket 将 L16 音频流传输到远程服务器。同时支持**原生下行流式播放** — 注册 `SWITCH_FILE_INTERFACE` 文件驱动（`audio_fork://`），可直接通过 HTTP Chunked 模式拉取并播放裸线性 PCM16 音频流，作为 `mod_shout` 的现代高性能零编解码替代方案。

## 功能特性

- **上行流式分流** — 将音频流通过 WebSocket 实时传输到远程 ASR 识别服务器
- **下行流式播放** — 原生 `SWITCH_FILE_INTERFACE` 文件驱动，直接拉取 HTTP Chunked PCM16 流播放
- **零编解码延迟** — 下行裸 PCM16 直接送入信道，彻底消除 MP3 编解码延迟与 CPU 消耗
- **严格整采样对齐** — 读写缓冲均按整采样边界对齐，彻底杜绝奇数字节截断白噪音
- **起播预缓冲与时钟防抖** — HTTP 与 WebSocket 分别配置，均默认 200ms（允许 1..5000ms），欠载自动补静音维持 20ms RTP 时钟
- **严格 PCM 契约** — 下行只接受交错 S16LE（16-bit little-endian、无 WAV/MP3 头）；WebSocket 分片可在任意字节边界切分
- **秒级极速打断** — 关闭播放时主动 shutdown 套接字，瞬间解除 curl 阻塞极速释放
- **多种混音类型** — 单声道（仅主叫）、混合（主叫 + 被叫）或立体声（独立声道）
- **灵活的采样率** — 上行 8000..64000 Hz 且必须是 8000 的整数倍；HTTP 下行 8000..48000 Hz 可使用任意整数
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
uuid_audio_fork <uuid> start <ws-url> <mix-type> <sampling-rate> [bugname] [metadata]
```

挂载媒体监听器并开始将音频流传输到 WebSocket 服务器。

| 参数 | 描述 |
|---|---|
| `uuid` | FreeSWITCH 通道 UUID |
| `ws-url` | WebSocket URL（`ws://`、`wss://`；兼容 `http://`、`https://` 别名；host < 512 字节、path < 4096 字节） |
| `mix-type` | `mono`（仅主叫）、`mixed`（主叫 + 被叫）或 `stereo`（独立声道） |
| `sampling-rate` | `8k`、`16k` 或 8000 的整数倍，范围 `8000..64000`（如 `24000`、`32000`、`64000`） |
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

注册 FreeSWITCH 原生 `SWITCH_FILE_INTERFACE` 文件驱动（协议头 `audio_fork://`），支持**双模**统一流式播放：
- **模式 A（WebSocket 全双工内存桥）**：直接读取当前通话绑定的 WebSocket 远端推流数据，极低延迟播放；
- **模式 B（HTTP Chunked 异步拉流）**：通过 HTTP/HTTPS 异步拉取裸线性 PCM16 流并播放。

---

#### 模式 A：WebSocket 全双工内存桥驱动 (`audio_fork://<uuid>`)

将 FreeSWITCH 通道播放引擎挂接到当前会话所属的 WebSocket 内存环形缓冲，直接播放远端 ASR/TTS 引擎推送的下行二进制 PCM 音频。

##### 语法格式

```
playback(audio_fork://<uuid>[:<bugname>][?query_parameters])
```

##### 支持的 URL 查询参数

| 参数名 | 描述 | 默认值 | 取值范围与说明 |
|---|---|---|---|
| `rate` / `sampling` | 下行音频采样率 (Hz) | `16000` | 必须与会话 `start` 时的目标采样率一致（如 `8000`、`16000`） |
| `channels` | 音频声道数 | `1` | `1`（单声道 Mono）或 `2`（立体声 Stereo），须与会话一致 |
| `watchdog` | 静音看门狗超时时间 (ms) | `3000` | 缓冲读空持续此时长后安全退出播放；设为 `0` 可关闭看门狗 |
| `playback` | 业务自定义透传播放标识 | — | 驱动忽略该参数，FreeSWITCH 的 `PLAYBACK_STOP` 路径仍保留供业务匹配 |

##### 调用示例

```bash
# 在 fs_cli 中直接播放当前会话的 WebSocket 下行音频流
fs_cli -x "uuid_broadcast <uuid> audio_fork://<uuid>:audio_fork?rate=16000&channels=1 aleg"

# 在 Outbound ESL (Node/Bun) 中下发播放指令
await session.execute("playback", `audio_fork://${uuid}?rate=16000&channels=1`);

# 收到 kill_audio 信令或业务打断时
fs_cli -x "uuid_break <uuid> all"
```

---

#### 模式 B：HTTP Chunked 异步拉流驱动 (`audio_fork://http[s]://`)

通过后台异步线程（libcurl）拉取远端 HTTP TTS 服务的 chunked 裸线性 PCM16 流并实时混音播放。

##### 语法格式

```
playback(audio_fork://http[s]://<host>:<port>/<path>[?query_parameters])
```

##### 支持的 URL 查询参数

| 参数名 | 描述 | 默认值 | 取值范围与说明 |
|---|---|---|---|
| `rate` / `sampling` | HTTP 音频采样率 (Hz) | `16000` | `8000` ~ `48000`，允许任意整数 |
| `channels` | 音频声道数 | `1` | `1`（单声道 Mono）或 `2`（立体声 Stereo） |
| `prebuffer` | 已弃用；模组忽略并记录提示，HTTP 原始查询串仍传给上游 | — | 预缓冲仅由 `audio_fork.conf.xml` 中的独立配置项控制 |
| `watchdog` | 静音看门狗超时时间 (ms) | `3000` | 连续无数据输入达此时长后安全退出播放；设为 `0` 可关闭看门狗 |

##### HTTP 与 WebSocket 播放预缓冲配置

模块加载时读取 FreeSWITCH `autoload_configs/audio_fork.conf.xml`，配置方式与 `mod_shout` 的 XML 配置一致：

```xml
<configuration name="audio_fork.conf" description="mod_audio_fork 播放配置">
  <settings>
    <param name="ws-prebuffer-ms" value="200"/>
    <param name="http-prebuffer-ms" value="200"/>
  </settings>
</configuration>
```

配置文件缺失或某项缺失时，该路径使用默认 `200` ms。两项均必须是 `1`～`5000` 的整数；无效值会阻止模块加载并指出字段。HTTP 和 WebSocket URL 的 `?prebuffer=` 均不会覆盖配置，模组会记录忽略提示。HTTP 原始查询串仍会传给上游服务；慢流在等待约 1 秒后达到 500 ms 数据量时仍可降级起播。修改 XML 后先执行 `reloadxml`，再在无活跃通话时重启容器加载新模块。加载日志会输出两项配置值，每次播放日志会输出所采用的预缓冲值。

##### 调用示例

```bash
# 在 fs_cli 中直接向通道播放实时 HTTP PCM 流
fs_cli -x "uuid_broadcast <uuid> audio_fork://http://127.0.0.1:9080/tts-stream/140581e86ae8eff9.pcm?rate=16000&channels=1 aleg"

# 在 Outbound ESL (Node/Bun) 中下发播放指令
await session.execute("playback", "audio_fork://http://127.0.0.1:9080/tts-stream/xxx.pcm?rate=16000&channels=1");

# 配合 uuid_break 毫秒级打断播放
fs_cli -x "uuid_break <uuid> all"
```

---

## WebSocket 通信协议规范 (Protocol Specification)

mod_audio_fork 与远端 WebSocket 服务端采用全双工 WebSocket 长连接（RFC 6455）进行通信，支持上下行音频传输与业务控制信令双向收发。

### 1. 握手与连接建立
- **子协议 (Sec-WebSocket-Protocol)**: 默认发送 `Sec-WebSocket-Protocol: audio.drachtio.org`（可通过环境变量 `MOD_AUDIO_FORK_SUBPROTOCOL_NAME` 自定义）。
- **Basic Auth 鉴权 (可选)**: 设置通道变量 `MOD_AUDIO_BASIC_AUTH_USERNAME` 与 `MOD_AUDIO_BASIC_AUTH_PASSWORD`，模块在握手阶段自动注入 `Authorization: Basic <base64>` 请求头。
- **初始元数据 (Metadata)**: 在调用 `uuid_audio_fork start <ws-url> <mix-type> <sampling> [bugname] [metadata]` 时，若提供了 `metadata`（JSON 字符串），模块在建连成功后会将其作为**首个文本帧**自动发送给服务端。

### 2. 上行音频流 (FreeSWITCH -> WebSocket Server)
- **帧类型**: WebSocket Binary Frame（二进制帧）。
- **音频格式**: 裸线性 PCM（S16LE, 16-bit little-endian 有符号整型，无 WAV 头/元数据头）。
- **声道与混音**: 由 `start` 命令的 `mix-type` 参数指定：
  - `mono`: 仅主叫音频流（单声道，16kHz 每 20ms 包长 640 字节）；
  - `mixed`: 主叫与被叫线性混音后单声道推送；
  - `stereo`: 左右双声道独立分流（每 20ms 包长 1280 字节）。
- **打包周期**: 严格按照 FreeSWITCH 读时钟（默认 20ms 一包）持续推送。

### 3. 下行音频流 (WebSocket Server -> FreeSWITCH)
用于全双工双向实时交互（AI 话务员/语音大模型实时下发 TTS 音频）：
- **帧类型**: WebSocket Binary Frame（二进制帧）。
- **音频格式**: 裸线性 PCM（S16LE），采样率与声道须与通道会话一致。
- **分片对齐**: 支持任意长度分片传输；模块内置跨帧字节对齐暂存器，自动修复单字节碎片，保证按整采样对齐。
- **定稿结束标记 (EOF)**: 服务端播完一句语音后，可通过以下两种方式之一告知模块流结束：
  1. 发送一个**长度为 0 字节的二进制帧**；
  2. 发送 `{"type":"speak_done"}` 文本帧。

### 4. 下行控制信令 (WebSocket Server -> FreeSWITCH, 文本 JSON 帧)
服务端下发的控制指令必须为合法 JSON 文本帧，顶层包含 `"type"` 字段。支持的信令规范如下：

| 信令 `type` | 格式示例 | 业务语义与 FreeSWITCH 动作 |
|---|---|---|
| `speak_start` | `{"type":"speak_start"}` | **开启新播报段**：重置下行代际计数器（`generation++`），清空缓冲区上一句残留的尾音，开始接收新一轮 PCM 数据。旧播放句柄因代际失效自动退出。 |
| `speak_done` | `{"type":"speak_done"}` | **句子定稿标记**：标记当前下行语音结束（标记 EOF），下游读缓冲区排干后文件句柄正常退出。 |
| `kill_audio` | `{"type":"kill_audio","data":{"speakId":"s_1001"}}` | **即时打断 (Barge-In)**：立即丢弃下行环形缓冲中所有积压音频，将 `interrupted` 置 1 强制中断播放，并向 FreeSWITCH 事件总线抛出 `mod_audio_fork::kill_audio` 事件（携带 `speakId`）。 |
| `transcription` | `{"type":"transcription","data":{"text":"你好","is_final":true}}` | **ASR 实时转写结果**：将 `data` 节点载荷向 FreeSWITCH 事件总线抛出 `mod_audio_fork::transcription` 事件，供业务层/ESL 订阅消费。 |
| `error` | `{"type":"error","data":{"code":"500","message":"ASR quota exceeded"}}` | **服务端错误通知**：将错误载荷向 FreeSWITCH 事件总线抛出 `mod_audio_fork::error` 事件。 |
| 通用/业务自定义 | `{"type":"custom_event", ...}` 或任意合法 JSON | **通用透传 JSON**：非流媒体控制/ASR 的业务信令（如业务打点、意图识别等），统一原样作为 `mod_audio_fork::json` 事件抛至 FreeSWITCH 事件总线，由业务层消费处理。 |

### 5. 上行控制信令 (FreeSWITCH -> WebSocket Server, 文本 JSON 帧)
业务层可通过 FreeSWITCH API 命令向活动的 WebSocket 连接发送任意 JSON 控制文本帧：

```bash
# 发送 DTMF 事件或自定义控制信令
uuid_audio_fork <uuid> send_text [bugname] {"event":"dtmf","digit":"1"}
```

## 事件

HTTP 和 WebSocket 下行流均为无头裸 PCM：采样率、声道数和位宽由 URL 或当前会话固定，服务端不得在一个流中改变格式。WebSocket 发送顺序为 `speak_start` 文本帧、一个或多个 binary PCM 帧、`speak_done` 文本帧或零长度 binary EOF 帧。binary 分片不要求与采样帧对齐；EOF 时仍有 1～3 个残余字节会报告 `pcm_partial_frame` 错误并终止。文件驱动不会返回 `SWITCH_STATUS_SUCCESS` 且长度为 0；欠载时返回请求帧数的静音，EOF 或停止时返回 `FALSE/0`。

`speak_start` 会推进播放代际并清空旧 PCM；旧句柄按代际不匹配退出，新句首帧即使在旧句柄关闭前到达也会进入缓冲。业务可在 WebSocket 播放 URL 上附加唯一 `?playback=<id>` 标识，文件驱动忽略该参数，FreeSWITCH 的 `PLAYBACK_STOP` 路径仍保留它供业务匹配。带 `data.speakId` 的 `kill_audio` 会把该标识透传至 `mod_audio_fork::kill_audio` 事件体，便于业务忽略迟到的旧句打断事件。

`uuid_audio_fork stop`、`uuid_break` 和通道挂机会停止接收下行音频、取消连接并等待后台线程收尾；模块卸载会拒绝新连接并清理待处理连接。缓冲区写入失败或 HTTP 背压超过 250ms 会记录错误、发出一次 `buffer_overrun`/`error` 事件并终止流。

本模块生成以下 FreeSWITCH 自定义事件：

| 事件 | 描述 |
|---|---|
| `mod_audio_fork::connect` | WebSocket 连接成功建立 |
| `mod_audio_fork::connect_failed` | WebSocket 连接失败（消息体包含原因） |
| `mod_audio_fork::disconnect` | WebSocket 连接关闭或服务器发送断开请求 |
| `mod_audio_fork::buffer_overrun` | 音频缓冲区写入不足或溢出，流被停止 |
| `mod_audio_fork::transcription` | 服务器发送了转录消息 |
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
