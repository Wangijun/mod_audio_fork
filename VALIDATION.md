# 下行 PCM 验证与部署

## 复现方式

先按 [BUILD.md](BUILD.md) 对齐运行实例 SDK，构建并运行 CTest。两个测试目标覆盖：

- `downstream_pcm`：88 组格式/长度组合，包括 8k 直通、16k→8k、24k→8k、8k→16k、8k→64k、64k→8k、双声道、0～39 帧极短输入及奇数字节分包。比较整段与分包输出逐字节一致，并核对 `ceil(N × 输出率 / 源率)` 帧数。
- 尾部脉冲、重复 EOF、残余字节报错、sink 溢出、打断复位及滤波历史隔离。24k→8k 的 6.5kHz 阻带能量相对 1kHz 通带约为 -81.6dB（质量 2）。
- `downstream_drivers`：首批音频早于句柄、短于预缓冲的 EOF、部分读取补零、16384 采样预取限制、输出率/声道冲突、URL 在首包前补充源率、信令声明优先、代际切换、旧句柄延迟关闭、当前句柄清理、不完整采样和环形缓冲溢出。
- HTTP 读端对条件等待函数的包装计数为 0；看门狗使用实际单调时间，快速重复调用不会按调用次数触发。HTTP 网络线程只在生产者背压时等待条件变量。

黑盒脚本只依赖 Python 标准库，使用已有 FreeSWITCH 的本地 ESL（127.0.0.1:8021）和 external SIP profile。ESL 测试凭据在 `ESL` 类集中配置。`--host` 必须是 FreeSWITCH 可以访问的本机 IPv4 地址。脚本临时启动 HTTP Chunked/WS 端点与 UDP SIP/RTP 应答端，使用随机空闲端口，给每次呼叫分配独立 UUID，并在 `finally` 中终止自己的呼叫、关闭所有套接字和服务器。

```bash
python3 tests/integration.py --host <本机可达IPv4> \
  --output build/verification/default --verify

# 仅测试呼叫启用线性时间戳，隔离既有 soft timer 跳步；不改 profile 或其他通话。
python3 tests/integration.py --host <本机可达IPv4> \
  --output build/verification/linear --linear-timestamps --verify
```

每套矩阵 44 次通话，覆盖 PCMU、PCMA 8kHz，HTTP/WS 两种驱动，以及正常 16kHz 音频、20ms 短音频、非整 RTP 帧尾音、抖动、200ms 看门狗、残余字节、HTTP 503、信令/URL 冲突、24kHz 信令声明、URL 首包前补充源率、不带格式的兼容用法和别名冲突。WS 正常用例在打开播放句柄前完成推流；抖动用例以 997 字节碎片发送，在第 17 个碎片后暂停 1.1 秒。

产出 `results.json`、播放事件 JSON、解码 PCM、长度前缀 RTP 包（每包前为网络序 uint16 长度）及 PCAP。PCAP 的 IPv4/UDP 外层按接收端观测的地址、端口和时间重建，RTP 头与载荷保留接收原文。所有音频、抓包、日志和构建产物均在忽略目录中，不提交 Git。RTP 时长按包载荷与播放事件的采样数共同核对；80 采样窗口的能量检测允许一包量化误差，精确采样计数由转换器和驱动 CTest 验证。

## 已验证的运行环境与结果

验收环境为 2026-10-25 的 WSL `fs-app`。运行实例自报 `1.11.2-release+git~20260809T162310Z~3f13ad1b1d~64bit`，其挂载 SDK 的 pkg-config 版本为 `1.11.2-release`。使用 `freeswitch-builder:trixie` 与该安装树的头文件和共享库构建，未采用编译工程文档中另一版本的 SDK。运行共享库 SHA-256 为：

```text
b60b88ec7b08edacb12c15e6d31ef860400ad77fba6e56859f6be3a50dbb418e
```

普通构建、CTest 2/2、启用 `-Wall -Wextra -Werror` 的 ASan/UBSan CTest 2/2 均通过。使用运行服务的库搜索路径执行 `ldd -r`，没有缺失库或未解析符号。两套最终通话矩阵各 44/44 通过。

| 检查 | 基线 | Producer 重采样 |
|---|---|---|
| 一秒 16kHz 输入，PCMU/PCMA，HTTP/WS | 8000 个有效输出采样，普通播放未复现爆音推断 | 8000 个有效输出采样，尾部不同频率信号保留 |
| 20ms 输入与非整尾帧 | 160、8159 个播放采样 | 160、8159 个播放采样；正常 EOF 不按请求长度补长 |
| 一秒输入中注入 1.1 秒网络停顿 | 两条链路均 20160 个播放采样，约 2.52 秒 | HTTP 12739 个（约 1.592 秒），WS 11139 个（约 1.392 秒） |
| 200ms 看门狗 | 原实现按读取次数计算 | RTP 尾部连续欠载约 190～210ms；脚本总耗时还包含 broadcast 排队及起播 |
| HTTP 503 响应体 | 当作 PCM 播放 | 无音频输出，明确记录 HTTP 错误 |
| EOF 残余字节 | 先播出完整部分，再报告错误 | 停止流并丢弃尚未播放的缓冲；WS 收到 `pcm_partial_frame` 事件 |
| 核心重采样器 | 源/通道率不同会启用 | 两套共 66 条 close 计数均为 `core_resampler=0`，包括部分错误退出 |

抖动用例的 HTTP 与 WS 时长不同，是 WS 首包在 `uuid_broadcast` 排队前已到达；均保留约一秒有效音频。20ms 活跃读取限制用于处理核心的 16384 采样预取，否则会提前填入未来数秒静音。HTTP 网络读等待已移除。

RTP 序号没有丢包或乱序。默认 soft timer 的最终矩阵出现 4 个带 marker 的 320 tick 跳步，基线也存在同类跳步；该行为与 `switch_rtp.c` 中根据 `write_timer.samplecount` 生成时间戳的逻辑一致。脚本默认模式单独记录这种已知跳步，拒绝其他异常增量。仅对测试呼叫设置 `SEND_LINEAR_TIMESTAMPS` 后，44 例全部为连续 160 tick 增量。全局 SIP/RTP 配置没有修改；本次不声称消除了核心 soft timer 的跳步，也未进行主观听感验收。

## 部署产物与回滚

独立仓库：`/data/freeswitch-src-build/mod_audio_fork`。新模块已加载至 `fs-app` 的挂载安装树：`/data/docker_fs_server/freeswitch-install/lib/freeswitch/mod/mod_audio_fork.so`。每次替换前均检查无活跃通话、正常卸载模块后复制并重新加载；回归失败时已实际验证恢复旧模块再加载的路径，未强制卸载，也未重启既有 FreeSWITCH。

| 产物 | 仓库内位置 | SHA-256 |
|---|---|---|
| 当前 Release 模块（已 strip，131072 字节） | `build/package/mod_audio_fork.so` | `94f9b2ce5f082313dfcc42873e063bb452f816196f6a4dfa59c6f76d9c701525` |
| 包含调试信息的验收产物 | `build/producer/mod_audio_fork.so` | `3bb632e5fb5d2c61f9b64716dbd74c01b96c2d24766961ca382e46053eba6936` |
| 改造前模块备份 | `build/deploy-backup/mod_audio_fork.before-producer.so` | `0b92d6d10b73f078da24661c80d715f8be5c80cea3374fd61f4462e876dc9848` |

当前 Release 产物使用运行实例的 1.11.2 SDK 重新编译并执行 `strip --strip-unneeded`，CTest 2/2 通过，追加 PCMU/PCMA × HTTP/WS × 正常/尾音/抖动共 12 次通话回归全部通过。结果在 `build/verification/redeploy-release/`；重新部署前的模块备份在 `build/deploy-backup/release-lHmEYs/mod_audio_fork.so`，该备份保留调试信息。

当前交付校验清单为 `build/package/SHA256SUMS`；改造前及完整调试产物的校验清单为 `build/deploy-backup/SHA256SUMS`。基线及最终结果保存在 `build/verification/`：`baseline-normal`、`baseline-edges`、`baseline-chunked-linear`、`producer-linear`、`producer-default`，附 `summary.json`、`playback-counters.log`、`deployed-ldd.txt`、`runtime-version.txt` 和 CTest 日志。

回滚时先在 `show channels count` 为 0 后执行 `unload mod_audio_fork`，从上述备份恢复模块，再执行 `load mod_audio_fork`、`module_exists mod_audio_fork` 并重复通话检查。任务结束时模块存在、活跃通话为 0；临时测试服务、SIP 端与构建容器均已停止。源码未提交、未推送。
