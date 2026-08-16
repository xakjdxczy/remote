# RemoteDesk

类似 ToDesk / 向日葵的**授权远程协助**工具：被控端显示 9 位识别码和临时密码，控制端在网页里输入后即可看画面、键鼠控制、传文件和聊天。

画面、键鼠、文件和聊天默认**只走 WebRTC P2P**。信令服务器只做识别码配对和交换 SDP，**不转发媒体**。默认无 TURN（打洞失败即连接失败）；如需在严格网络下也能连通，可**可选启用 TURN 中继**（见下）。画面统一限制在 **720p** 以内以控制带宽。

> 只用于机主明确同意的远程协助。被控端必须由机主自己启动；没有识别码和密码无法连接。请勿用于未授权访问他人设备。

## 功能

- 9 位设备识别码 + 可刷新临时密码（连接前校验）
- 网页控制端：画面、鼠标、键盘、滚轮
- 文件拖拽发送到被控端 `~/RemoteDeskDownloads`
- 会话内文字消息、延迟 / FPS 显示
- 无显示器时自动使用可交互的**演示桌面**（可拖窗口、打字、点开始菜单）
- **跨平台被控端**：Windows / macOS / Linux 真机截屏 + 键鼠注入，Android 经 ADB 桥远程控制
- 有显示器时尝试真实截屏，并用 pynput 注入输入

## 跨平台被控端（backend）

被控端通过 `--backend` 选择画面/输入来源：

| backend | 平台 | 画面 | 输入 |
| --- | --- | --- | --- |
| `auto`（默认） | 全部 | 有显示器→desktop，否则→virtual | 同左 |
| `desktop` | Windows / macOS / Linux | `mss` 截屏 | `pynput` 键鼠 |
| `android` | 任意电脑 + 安卓设备 | `adb screencap` | `adb input`（点按/滑动/文本/按键） |
| `virtual` | 全部 | 内置演示桌面 | 内置演示桌面 |

### macOS

`desktop` 后端在 macOS 上用 `mss` + `pynput`，已处理 Retina 物理/逻辑像素换算和多显示器偏移。首次运行需授权：

> 系统设置 → 隐私与安全性 → **屏幕录制** 和 **辅助功能**，勾选运行本程序的终端/应用，然后重启被控端。

```bash
pip install -e ".[host]"
python -m remote host --server ws://<服务器>/ws --backend desktop
```

### Android（ADB 桥）

在一台电脑上运行被控端，用 USB 或 `adb connect` 接入安卓设备，即可把**该安卓设备**变成被控端。需要先装好 [platform-tools(adb)](https://developer.android.com/tools/releases/platform-tools) 并在手机上开启「USB 调试」。

```bash
adb devices                        # 确认设备已授权
python -m remote host --server ws://<服务器>/ws --backend android
# 多台设备时用 --adb-serial <序列号> 指定
```

> 说明：这是**贴合现有 Python 架构、可落地可测试**的方案，把安卓设备作为被控端。若需要「手机上装 App 即可被控」的**原生 Android 应用**（MediaProjection + 无障碍注入 + 原生 WebRTC），那是另一套更大的工程，不在本仓库范围内。

## 架构

```
被控端 Host  <======== WebRTC DataChannel (P2P) ========>  网页控制端
     |                      画面 / 键鼠 / 文件 / 聊天              |
     +----- WebSocket 信令：识别码、密码、SDP / ICE ------+
                        信令 Server（不传媒体）
```

同一时间一台主机只接受一个控制端。密码在信令服务器上与主机登记时的临时密码做恒定时间比较。默认 ICE 只用 STUN；控制端会显示当前连接方式（**P2P 直连** 或 **TURN 中继**）。

### 可选：启用 TURN 中继

在信令服务器上设置环境变量即可让 `ice_servers`（`/api/config`）带上 TURN；不设置则保持纯 STUN/P2P：

```bash
export TURN_URLS="turn:your-ip:3478?transport=udp,turn:your-ip:3478?transport=tcp"
export TURN_USER="rd"
export TURN_PASS="<secret>"
python -m remote server
```

启用 TURN 后，直连失败的会话会自动经中转连通（会消耗服务器带宽）。控制端与被控端都会标明当前是直连还是中继。

## 快速开始

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -e ".[dev]"
```

### 一键演示（推荐先看效果）

```bash
python -m remote demo
```

浏览器打开 http://127.0.0.1:8080 ，页面会带上演示主机的识别码和密码，点「远程控制」即可操作虚拟桌面。

### 分开放

```bash
# 机器 A 或一台公网 VPS（只做信令）
python -m remote server --host 0.0.0.0 --port 8080

# 被控电脑
python -m remote host --server ws://服务器IP:8080/ws

# 控制端：浏览器打开 http://服务器IP:8080
# 输入被控端控制台里的识别码和临时密码
```

双方需要能互相打通 UDP（同一局域网，或经过 STUN 的锥形 NAT）。对称 NAT / 严格防火墙下会失败，因为没有中继回退。

局域网外请用 Nginx / Caddy 做 HTTPS，并把 `--server` 改成 `wss://你的域名/ws`。网页本身仍只用于信令。

## 命令

| 命令 | 作用 |
| --- | --- |
| `python -m remote server` | 信令 + 网页 UI |
| `python -m remote host` | 被控端，打印识别码/密码（backend=auto） |
| `python -m remote host --backend desktop` | 强制真机截屏（Win/macOS/Linux） |
| `python -m remote host --backend android` | 经 ADB 控制安卓设备 |
| `python -m remote host --virtual` | 强制使用演示桌面 |
| `python -m remote demo` | 本地同时拉起信令和演示主机 |

识别码会写到 `~/.remotedesk/device.json`，重启后尽量保持不变。

## 测试

```bash
pytest -q
```

## Docker（仅信令）

```bash
docker compose up --build
```

被控端仍在真实电脑上运行 `python -m remote host --server ws://<host>:8080/ws`。
