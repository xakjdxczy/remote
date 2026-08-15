# RemoteDesk

类似 ToDesk / 向日葵的**授权远程协助**工具：被控端显示 9 位识别码和临时密码，控制端在网页里输入后即可看画面、键鼠控制、传文件和聊天。

画面、键鼠、文件和聊天**只走 WebRTC P2P**。信令服务器只做识别码配对和交换 SDP，**不转发媒体**，也**不提供 TURN 中继**。打洞失败就连接失败。

> 只用于机主明确同意的远程协助。被控端必须由机主自己启动；没有识别码和密码无法连接。请勿用于未授权访问他人设备。

## 功能

- 9 位设备识别码 + 可刷新临时密码（连接前校验）
- 网页控制端：画面、鼠标、键盘、滚轮
- 文件拖拽发送到被控端 `~/RemoteDeskDownloads`
- 会话内文字消息、延迟 / FPS 显示
- 无显示器时自动使用可交互的**演示桌面**（可拖窗口、打字、点开始菜单）
- 有显示器时尝试真实截屏，并用 pynput 注入输入

## 架构

```
被控端 Host  <======== WebRTC DataChannel (P2P) ========>  网页控制端
     |                      画面 / 键鼠 / 文件 / 聊天              |
     +----- WebSocket 信令：识别码、密码、SDP / ICE ------+
                        信令 Server（不传媒体）
```

同一时间一台主机只接受一个控制端。密码在信令服务器上与主机登记时的临时密码做恒定时间比较。ICE 只用 STUN，不用 TURN。

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
| `python -m remote host` | 被控端，打印识别码/密码 |
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
