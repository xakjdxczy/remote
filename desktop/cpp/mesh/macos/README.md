# macOS Packet Tunnel（第二层，源码骨架）

尘埃X 的 **应用层隧道**不需要这段扩展。这里只是 Packet Tunnel Provider 的骨架，方便以后用**项目自己的**付费 Apple 开发者账号开通 `com.apple.developer.networking.networkextension` 后再编进去。

## 为什么默认不编进 App

- 正式激活要 Network Extension 能力，和系统摄像头扩展同一类门槛。
- 开源 / ad-hoc 签名没有该能力。把扩展打进包再要系统扩展许可，会把宿主应用签死或无法启动。
- **不要**用公司 Developer ID 给这个开源仓库签名。

因此桌面端设置里「虚拟网卡」在 Mac 上默认不可用，并提示改用 `127.0.0.1` 端口转发。

## 以后若要用项目自己的账号

1. 在 Apple Developer 给 `com.dustx.remotedesk` 打开 Network Extension → Packet Tunnel。
2. 用 `DUSTX_SIGN_IDENTITY` 指向**这个项目自己的**证书（不要用别人的公司证）。
3. 把本目录编成 `appex`，嵌进 `尘埃X.app/Contents/PlugIns/`。
4. 宿主 entitlement 再加 `com.apple.developer.networking.networkextension` = `packet-tunnel-provider`。

在那之前，请用第一层应用隧道。
