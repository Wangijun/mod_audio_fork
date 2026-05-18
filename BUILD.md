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
sudo apt-get install -y cmake build-essential libwebsockets-dev libboost-all-dev
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

### 2. 重启 FreeSWITCH

```bash
sudo systemctl restart freeswitch
```

或从 fs_cli 重新加载：
```bash
fs_cli -x "reload mod_audio_fork"
```

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
