# 尘埃X 桌面程序（C++）

双击运行，不需要安装 Python。窗口里是：

- **远程控制**：本机页面注册到官网信令（默认 `https://loessx.com`），看屏幕 / 键鼠
- **跨网互访**：另一套识别码，打开即上线。应用层隧道可同时连多台（每路一个本地端口，默认从 `2222` 起）。Windows 可选 Wintun 虚拟网卡（同时只能一路）。Mac 没有项目自己的 Network Extension 许可时，「虚拟网卡」不可用
- **手机摄像头**：本机 HTTP/WebSocket 配对，只走局域网或 USB

三块面板常驻，切页不会把另外两边踢下线。远程控制和互访可以同时连同一台电脑（两套码，两条 P2P）。云端可用 `python -m remote mesh` 走同一条打洞隧道，或 `python -m remote agent` 走不占 P2P 的应用协议（主机上线后，信令 `POST /api/agent` → 本机 `POST /api/agent/run`）。

手机 App 协议不变：`dustcam://ip:port/token`，`/cam/ws` 的 `hello` / `signal` / `ready`。

虚拟摄像头和麦克风都是尘埃X 自己的设备，不用 BlackHole / VB-CABLE。Windows 用 DirectShow（`DustXCam.dll` / `DustXMic.dll`），当前用户注册即可，不要代码签名。macOS 麦克风是 HAL 驱动 `DustXMic.driver`；Mac 系统摄像头仍受 Apple 扩展限制。会议 / OBS 请选「尘埃X 摄像头」和「尘埃X 麦克风」。

## 编译

macOS：

```bash
cd desktop/cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
open build/尘埃X.app
```

Windows（在 Windows 上，或 GitHub Actions `windows-latest`）：

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物是 `build/Release/DustX.exe`，旁边必须有 `DustXCam.dll`、`DustXMic.dll`、`wintun.dll` 和 `web\`。打开程序会按当前用户注册 DirectShow 设备，不用管理员、也不用代码签名。OBS 选「视频采集设备 → 尘埃X 摄像头」，音频选「尘埃X 麦克风」。编译时会下载 Google platform-tools，把 `adb` 打进应用（Mac 在 `Contents/Resources/platform-tools/`，Windows 在可执行文件旁的 `platform-tools/`）。「准备 USB」不再依赖系统 PATH 里有 adb。虚拟网卡需要管理员 + 官方 Wintun；应用层隧道不用管理员。

环境变量：

- `DUSTX_REMOTE_URL` 远程控制页
- `DUSTX_AGENT_ROOT` 应用协议 list/read/write/exec 的根目录（默认用户主目录）
- `DUSTX_CAM_PORT` 本机配对端口（默认 18790）
- `DUSTX_SIGN_IDENTITY` macOS 正式签名身份。不设则 ad-hoc，不会去钥匙串里抓公司的 Developer ID。

macOS 上「尘埃X 麦克风」是 HAL 插件，开源发行可以装。要进 OBS「视频采集设备」的虚拟摄像头必须走 Apple Camera Extension，需要项目自己的付费开发者账号签名；没有账号时 Mac 请用窗口采集预览，Windows 的 DirectShow 摄像头不受此限制。
