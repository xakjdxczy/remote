# 尘埃X (DUST-X)

本仓库是「尘埃X」品牌的产品单体仓库，包含官网、字母陨石打字游戏、RemoteDesk 远程协助工具（含网页控制端、信令服务、跨平台被控端与原生 Android App），以及最初用于搭建开发环境的 Tasks 起步应用。

## 目录结构

| 目录 | 内容 |
| --- | --- |
| `site/` | 尘埃X 官网首页 + 26 键「字母陨石」打字游戏（静态站点） |
| `src/` | RemoteDesk：信令服务、网页控制端、跨平台被控端（Python） |
| `android/` | RemoteDesk 原生 Android 被控端 App（Kotlin，MediaProjection + 无障碍注入 + 原生 WebRTC） |
| `tests/` | RemoteDesk 的 pytest 测试 |
| `client/` `server/` | 最初的全栈 Tasks 起步应用（用于验证开发环境） |
| `Dockerfile` `docker-compose.yml` | RemoteDesk 信令服务容器化 |

---

# RemoteDesk

类似 ToDesk / 向日葵的**授权远程协助**工具：被控端显示识别码和临时密码，控制端在网页里输入后即可看画面、键鼠控制、传文件和聊天。

画面、键鼠、文件和聊天默认**只走 WebRTC P2P**。信令服务器只做识别码配对和交换 SDP，**不转发媒体**。可**可选启用 TURN 中继**在严格网络下也连通（见下）。画面统一限制在 **720p** 以内以控制带宽。

> 只用于机主明确同意的远程协助。被控端必须由机主自己启动；没有识别码和密码无法连接。请勿用于未授权访问他人设备。

## 功能

- 设备识别码 + 可刷新临时密码（连接前校验）
- 网页控制端：画面、鼠标、键盘、滚轮、Android 导航栏
- 文件拖拽发送到被控端 `~/RemoteDeskDownloads`
- 会话内文字消息、实时网速/流量、延迟拆解（网络·缓冲·解码）/ FPS 显示
- 无显示器时自动使用可交互的**演示桌面**
- **跨平台被控端**：Windows / macOS / Linux 真机截屏 + 键鼠注入，Android 经 ADB 桥或**原生 App**远程控制
- **手机当摄像头**：同一套尘埃X。电脑打开**桌面程序**（双击 `尘埃X.app` / `尘埃X.exe`），窗口里选「手机摄像头」；手机 App 选「作为摄像头」。Wi‑Fi 局域网或 USB 二选一。会议软件选「尘埃X 摄像头」（自己的虚拟摄像头，不用 OBS）。麦克风仍用 BlackHole / VB-CABLE。
- **跨网互访**：两台都开着尘埃X，用互访识别码互加。可在设置里选 **应用层隧道**（本机 `127.0.0.1:端口` 转到对端，Mac 默认这项）或 **虚拟网卡**（Windows Wintun `100.x`；Mac 没有项目自己的 Network Extension 许可时选项不可用）。打洞失败走同一套 TURN。云端 / CLI 也可以走同一条打洞隧道，或走不占 P2P 的应用层协议（见下）。

## 跨平台被控端（backend）

被控端通过 `--backend` 选择画面/输入来源：

| backend | 平台 | 画面 | 输入 |
| --- | --- | --- | --- |
| `auto`（默认） | 全部 | 有显示器→desktop，否则→virtual | 同左 |
| `desktop` | Windows / macOS / Linux | `mss` 截屏 | `pynput` 键鼠 |
| `android` | 任意电脑 + 安卓设备 | `adb screencap` | `adb input` |
| `virtual` | 全部 | 内置演示桌面 | 内置演示桌面 |

### macOS

`desktop` 后端在 macOS 上用 `mss` + `pynput`，已处理 Retina 像素换算和多显示器偏移。首次运行需授权：

> 系统设置 → 隐私与安全性 → **屏幕录制** 和 **辅助功能**，勾选运行本程序的终端/应用，然后重启被控端。

```bash
pip install -e ".[host]"
python -m remote host --server ws://<服务器>/ws --backend desktop
```

### Android（ADB 桥）

```bash
adb devices                        # 确认设备已授权
python -m remote host --server ws://<服务器>/ws --backend android
# 多台设备时用 --adb-serial <序列号> 指定
```

### Android（原生 App）

`android/` 下是原生 Android 被控端（MediaProjection 采集 + 无障碍服务注入输入 + 原生 WebRTC 视频编码）。构建：

```bash
cd android
gradle :app:assembleDebug   # 产物 app/build/outputs/apk/debug/app-debug.apk
```

## 架构

```
被控端 Host  <======== WebRTC (P2P / 可选 TURN) ========>  网页控制端
     |                   视频 / 键鼠 / 文件 / 聊天                |
     +----- WebSocket 信令：识别码、密码、SDP / ICE ------+
                        信令 Server（不传媒体）
```

同一时间一台主机只接受一个控制端。密码在信令服务器上与主机登记时的临时密码做恒定时间比较。控制端会显示当前连接方式（**P2P 直连** 或 **TURN 中继**）。

### 可选：启用 TURN 中继

在信令服务器上设置环境变量即可让 `ice_servers`（`/api/config`）带上 TURN；不设置则保持纯 STUN/P2P：

```bash
export TURN_URLS="turn:your-ip:3478?transport=udp,turn:your-ip:3478?transport=tcp"
export TURN_USER="rd"
export TURN_PASS="<secret>"
python -m remote server
```

`docker-compose.yml` 里带了 coturn 服务。VPS 上把公网 IP 写进环境变量即可（和信令共用一套账号）：

```bash
export TURN_EXTERNAL_IP="<VPS公网IP>"
export TURN_URLS="turn:${TURN_EXTERNAL_IP}:3478?transport=udp,turn:${TURN_EXTERNAL_IP}:3478?transport=tcp"
export TURN_USER="dustx"
export TURN_PASS="<secret>"
docker compose up --build -d
```

桌面端「跨网互访」会拉 `/api/config` 的 `ice_servers`（信令已允许本机页面跨域读取）。不设 `TURN_URLS` 时仍是纯 STUN/P2P。

### 云端 / CLI 连 Windows（打洞 + 应用协议）

Windows 尘埃X 互访页先点「上线」。两种方式都支持，不要把家里的 SSH/RDP 暴露到公网。

**打洞**（和 Mac 互访同一条 WebRTC 数据通道；同时只能有一路 P2P）：

```bash
python -m remote mesh --device <识别码> --password <密码>
ssh -p 2222 对端用户名@127.0.0.1
```

Mac 已经连着时，云端会收到 `device busy`。打洞失败走 VPS 上的 TURN。

**纯应用协议**（经 VPS 信令转发 `list` / `read` / `write` / `exec`，不打洞、不占 P2P；文件默认限制在用户主目录）：

```bash
python -m remote agent --device <识别码> --password <密码> list
python -m remote agent --device <识别码> --password <密码> read --path Desktop
python -m remote agent --device <识别码> --password <密码> exec --command "whoami"
```

### 官网安装包（OSS）

编译好的安卓包 / macOS 桌面程序 / Windows 桌面程序放到对象存储后，官网下载按钮会先请求信令服务器 `/api/downloads/<android|macos|windows>`，由服务器签发短时下载链接，再跳转到 OSS 下载（不把 AK/SK 暴露给浏览器）。

```bash
export OSS_AK="<access-key>"
export OSS_SK="<secret-key>"
export OSS_ENDPOINT="s3.example-region.example-oss.com"
export OSS_BUCKET="<bucket>"
# 可选：OSS_REGION / OSS_APK_KEY / OSS_META_KEY

# 上传编译产物（需本机能直连 OSS；云端构建机若被墙可改用 --presign-put）
python -m remote upload-apk android/app/build/outputs/apk/debug/app-debug.apk \
  --version 1.8.1 --version-code 15
python -m remote upload-download macos desktop/mac/尘埃X.app
python -m remote upload-download windows desktop/windows/DustX

# 只签发限时 PUT 链接，拿到能访问 OSS 的电脑上再用 curl -T 上传
python -m remote upload-apk android/app/build/outputs/apk/debug/app-debug.apk \
  --version 1.8.1 --version-code 15 --presign-put

# 信令进程需带上同样的 OSS_* 环境变量
python -m remote server
```

官网静态页与信令同域时，Nginx 把 `/api/` 反代到信令即可。接口：

- `GET /api/downloads/android|macos|windows` → JSON（`url` / `filename` / `version`）
- `GET /api/downloads/<kind>?redirect=1` → 302 到签名 URL

## 快速开始

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -e ".[dev]"
```

### 电脑桌面程序（Mac / Windows，C++）

用户双击 **尘埃X.app**（macOS）或 **DustX.exe**（Windows）即可，不需要安装或运行 Python。窗口是系统原生的（macOS AppKit + WKWebView，Windows Win32 + WebView2）：远程控制走官网控制台，手机摄像头走本机配对端口，跨网互访用现有公网信令打 WebRTC 数据通道。

```bash
cd desktop/cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

macOS 产物：`desktop/cpp/build/尘埃X.app`。Windows 在 `windows-latest` 上编：Actions → Pack desktop → 下载 `DustX-windows`。首次若被拦：右键 → 打开。

远程控制页可用环境变量 `DUSTX_REMOTE_URL` 覆盖。

### 一键演示

```bash
python -m remote demo
```

浏览器打开 http://127.0.0.1:8080 ，页面会带上演示主机的识别码和密码，点「远程控制」即可操作虚拟桌面。

### 分开部署

```bash
# 公网 VPS（只做信令）
python -m remote server --host 0.0.0.0 --port 8080
# 被控电脑
python -m remote host --server ws://服务器IP:8080/ws
# 控制端：浏览器打开 http://服务器IP:8080 ，输入识别码和临时密码
```

局域网外请用 Nginx / Caddy 做 HTTPS，并把 `--server` 改成 `wss://你的域名/ws`。

## 命令

| 命令 | 作用 |
| --- | --- |
| `cd desktop/cpp && cmake -B build && cmake --build build` | 打出 C++ 尘埃X.app / DustX.exe（用户不跑 Python） |
| `python -m remote pack` | 旧的 Python 打包（已不作为桌面产品） |
| `python -m remote app` | 旧的 Python 窗口调试 |
| `python -m remote server` | 信令 + 网页 UI |
| `python -m remote host` | 被控端，打印识别码/密码（backend=auto） |
| `python -m remote host --backend desktop` | 强制真机截屏（Win/macOS/Linux） |
| `python -m remote host --backend android` | 经 ADB 控制安卓设备 |
| `python -m remote demo` | 本地同时拉起信令和演示主机 |
| `python -m remote upload-apk <apk>` | 把安卓包上传到 OSS，供官网下载 |
| `python -m remote upload-download macos|windows|android <path>` | 把桌面/安卓包装进 OSS |
| `python -m remote mesh --device --password` | 跨网互访打洞，本机 `2222` 转到对端 |
| `python -m remote agent --device --password list\|read\|write\|exec` | 应用层协议，不占 P2P |

## 测试

```bash
pytest -q
```

## Docker（信令 + 可选 TURN）

```bash
docker compose up --build
```

compose 会同时起信令和 coturn。客户端要真正走中继，还须给信令进程设置 `TURN_URLS` / `TURN_USER` / `TURN_PASS`（见上文「启用 TURN 中继」），否则 `ice_servers` 仍只有 STUN。

---

# Tasks 起步应用（client / server）

最初用于搭建并验证开发环境的全栈示例（npm workspaces 单体仓库）。

```bash
npm ci        # 安装依赖
npm run dev   # 同时启动 API(:3001) 与前端(:5173)
```

| 命令 | 说明 |
| --- | --- |
| `npm run dev` | 同时运行 server + client |
| `npm run build` | 类型检查并构建 |
| `npm run lint` | ESLint 检查 |
| `npm test` | 运行 API 测试（Vitest + Supertest） |
