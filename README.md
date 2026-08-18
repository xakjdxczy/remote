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
- **手机当摄像头**：同一套尘埃X。电脑打开**桌面程序**（双击 `尘埃X.app` / `尘埃X.exe`），窗口里选「手机摄像头」；手机 App 选「作为摄像头」。Wi‑Fi 局域网或 USB 二选一。会议软件要点窗口里的「输出到系统摄像头 / 麦克风」（需 OBS 虚拟摄像头或 BlackHole / VB-CABLE）。

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

### 官网安卓包（OSS）

编译好的 APK 放到对象存储后，官网下载按钮会先请求信令服务器 `/api/downloads/android`，由服务器签发短时下载链接，再跳转到 OSS 下载（不把 AK/SK 暴露给浏览器）。

```bash
export OSS_AK="<access-key>"
export OSS_SK="<secret-key>"
export OSS_ENDPOINT="s3.example-region.example-oss.com"
export OSS_BUCKET="<bucket>"
# 可选：OSS_REGION / OSS_APK_KEY / OSS_META_KEY

# 上传编译产物（需本机能直连 OSS；云端构建机若被墙可改用 --presign-put）
python -m remote upload-apk android/app/build/outputs/apk/debug/app-debug.apk \
  --version 1.8.1 --version-code 15

# 只签发限时 PUT 链接，拿到能访问 OSS 的电脑上再用 curl -T 上传
python -m remote upload-apk android/app/build/outputs/apk/debug/app-debug.apk \
  --version 1.8.1 --version-code 15 --presign-put

# 信令进程需带上同样的 OSS_* 环境变量
python -m remote server
```

官网静态页与信令同域时，Nginx 把 `/api/` 反代到信令即可。接口：

- `GET /api/downloads/android` → JSON（`url` / `filename` / `version`）
- `GET /api/downloads/android?redirect=1` → 302 到签名 URL

## 快速开始

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -e ".[dev]"
```

### 电脑桌面程序（Mac / Windows）

用户双击 **尘埃X.app**（macOS）或 **尘埃X.exe**（Windows）即可，不需要安装或运行 Python。窗口里是远程控制 + 手机摄像头 / 虚拟设备。官网上的网页控制台只支持远程控制。

开发机打一次包（源码不会进用户电脑）：

```bash
pip install -e ".[pack,host]"
python -m remote pack
```

生成 `desktop/mac/尘埃X.app` 或 `desktop/windows/DustX/DustX.exe`。首次若被拦：右键 → 打开。

Windows 包在 GitHub Actions 的 `windows-latest` 上打：Actions → Pack desktop → 下载 `DustX-windows` 产物。也可在仓库页手动 Run workflow。

源码调试仍可用 `python -m remote app`。

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
| `python -m remote pack` | 打出 尘埃X.app / 尘埃X.exe（用户不跑 Python） |
| `python -m remote app` | 源码调试：打开桌面窗口 |
| `python -m remote server` | 信令 + 网页 UI |
| `python -m remote host` | 被控端，打印识别码/密码（backend=auto） |
| `python -m remote host --backend desktop` | 强制真机截屏（Win/macOS/Linux） |
| `python -m remote host --backend android` | 经 ADB 控制安卓设备 |
| `python -m remote demo` | 本地同时拉起信令和演示主机 |
| `python -m remote upload-apk <apk>` | 把安卓包上传到 OSS，供官网下载 |

## 测试

```bash
pytest -q
```

## Docker（仅信令）

```bash
docker compose up --build
```

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
