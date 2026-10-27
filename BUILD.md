# 构建 mod_audio_fork

## 前提条件

### 1. FreeSWITCH

需要安装可运行的 FreeSWITCH 及其开发头文件。

**通过软件包安装（Debian/Ubuntu）：**
```bash
sudo apt-get install -y freeswitch freeswitch-dev
```

**从源码构建：**
```bash
git clone https://github.com/signalwire/freeswitch.git
cd freeswitch
./bootstrap.sh
./configure
make
sudo make install
```

### 2. 构建依赖

```bash
sudo apt-get update
sudo apt-get install -y cmake build-essential pkg-config libwebsockets-dev libspeex-dev libspeexdsp-dev libcurl4-openssl-dev
```

## 构建

### 使用 build.sh（推荐）

`build.sh` 脚本可处理依赖安装、构建和安装：

```bash
chmod +x build.sh

# 一键执行：安装依赖、构建并安装
sudo ./build.sh all

# 或分步执行：
sudo ./build.sh deps      # 仅安装构建依赖
./build.sh build           # 仅配置和构建
sudo ./build.sh install    # 仅将 .so 安装到 FreeSWITCH 模块目录
./build.sh --help          # 显示用法和选项
```

#### 环境变量

可以通过环境变量覆盖默认路径：

| 变量 | 默认值 | 描述 |
|---|---|---|
| `FREESWITCH_INCLUDE_DIR` | `/usr/local/freeswitch/include/freeswitch` | FreeSWITCH 头文件路径 |
| `FREESWITCH_LIBRARY` | `/usr/local/freeswitch/lib/libfreeswitch.so` | FreeSWITCH 共享库路径 |
| `FREESWITCH_MOD_DIR` | `/usr/local/freeswitch/mod` | 模块安装目录 |
| `BUILD_TYPE` | `Release` | CMake 构建类型（`Release` 或 `Debug`） |

自定义路径示例：
```bash
FREESWITCH_INCLUDE_DIR=/usr/include/freeswitch \
FREESWITCH_LIBRARY=/usr/lib/libfreeswitch.so \
FREESWITCH_MOD_DIR=/usr/lib/freeswitch/mod \
./build.sh all
```

### 手动构建

```bash
mkdir build
cd build

cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DFREESWITCH_INCLUDE_DIR="/usr/local/freeswitch/include/freeswitch" \
  -DFREESWITCH_LIBRARY="/usr/local/freeswitch/lib/libfreeswitch.so"

make -j$(nproc)
```

然后安装：
```bash
sudo cp mod_audio_fork.so /usr/local/freeswitch/mod/
sudo chown freeswitch:freeswitch /usr/local/freeswitch/mod/mod_audio_fork.so
```

## 安装与配置

### 1. 加载模块

在 FreeSWITCH 的 `modules.conf.xml` 中添加：
```xml
<load module="mod_audio_fork"/>
```

### 2. 无活跃通话时重新加载模块

```bash
fs_cli -x "show channels count"
# 必须为 0；先备份现有模块并记录 sha256sum，再卸载、替换与加载。
fs_cli -x "unload mod_audio_fork"
# 安装至 fs_cli -x "global_getvar mod_dir" 返回的目录
fs_cli -x "load mod_audio_fork"
```

不要强制卸载使用中的模块。加载或通话回归失败时，在无活跃通话后卸载候选模块、恢复备份并重新加载验证。既有 FreeSWITCH 服务无需重启。

### 3. 验证

```bash
fs_cli -x "module_exists mod_audio_fork"
```

## 故障排除

### 常见问题

| 问题 | 解决方案 |
|---|---|
| 找不到模块 | 确认 `mod_audio_fork.so` 在 FreeSWITCH 模块目录中 |
| 权限被拒绝 | 确保文件所有者为 `freeswitch:freeswitch` |
| 缺少依赖 | 运行 `ldd mod_audio_fork.so` 检查未解析的符号 |
| 构建错误 | 确保 FreeSWITCH 头文件和所有依赖已安装 |

### 调试日志

查看 FreeSWITCH 日志：
```bash
tail -f /var/log/freeswitch/freeswitch.log
```

或在 fs_cli 中设置调试级别：
```bash
fs_cli -x "console loglevel debug"
```

### 验证库依赖

```bash
ldd /usr/local/freeswitch/mod/mod_audio_fork.so
```

## 对齐运行实例的 Trixie 构建与测试

构建工程文档版本不代表运行版本。先查询运行实例 `version`、`global_getvar mod_dir`，核对其挂载的 SDK `lib/pkgconfig/freeswitch.pc`、头文件及 `libfreeswitch.so` 校验值。使用同一安装树的头文件和共享库，禁止混用其他 FreeSWITCH 安装树。

以下路径由当前部署提供：`SDK_ROOT` 指运行容器实际挂载的安装树，`DEPS_ROOT` 指运行依赖目录，`RUNTIME_LIBS` 放置构建镜像缺少但运行容器使用的依赖库（例如 `libtpl.so.0`，须复制实际文件及对应 SONAME）。这些目录以只读方式挂载。

```bash
docker run --rm \
  -v "$PWD:/src" -v "$SDK_ROOT:/sdk:ro" \
  -v "$DEPS_ROOT:/deps:ro" -v "$RUNTIME_LIBS:/runtime-libs:ro" \
  -e LD_LIBRARY_PATH=/sdk/lib:/deps:/runtime-libs \
  freeswitch-builder:trixie bash -lc '
    cmake -S /src -B /src/build/producer \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DFREESWITCH_INCLUDE_DIR=/sdk/include/freeswitch \
      -DFREESWITCH_LIBRARY=/sdk/lib/libfreeswitch.so &&
    cmake --build /src/build/producer -j8 &&
    ctest --test-dir /src/build/producer --output-on-failure &&
    ldd -r /src/build/producer/mod_audio_fork.so
  '
```

`ldd -r` 必须没有 `not found` 或 `undefined symbol`。CTest 包含 `downstream_pcm`、`downstream_drivers`；驱动测试只初始化 APR 内存池，不启动 FreeSWITCH 服务，通过链接器包装计数确保 HTTP 读端没有条件等待。测试断言在 Release 构建中也保持有效。检查内存与未定义行为时，在单独的 `build/sanitized` 中为 C 编译器及可执行文件链接器加入 `-fsanitize=address,undefined`，仅构建两个测试目标后运行 CTest。

本地 SIP/RTP 回归脚本、验收边界与回滚位置见 [VALIDATION.md](VALIDATION.md)。
