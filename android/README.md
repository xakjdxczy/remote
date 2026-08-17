# 尘埃X 远程被控 · Android Host (RemoteDeskHost)

原生 Android **被控端**：手机安装后即可被远程控制，无需连接电脑（区别于 ADB 桥方案）。

- **画面**：MediaProjection 截屏 → JPEG → 通过 WebRTC DataChannel P2P 直发（与 Python 被控端、网页控制端同一套协议）。
- **输入**：AccessibilityService 注入点击 / 滑动 / 滚动。
- **信令**：OkHttp WebSocket，`wss://<server>/ws`，注册为 host 并交换 SDP/ICE（非 trickle）。
- **P2P**：仅 STUN、无 TURN，符合项目「纯 P2P」设计。

## 使用

1. 安装 APK，打开「尘埃X 远程被控」。
2. 填写信令服务器（默认 `wss://117.72.108.246/ws`）。
3. 点「开始被控（授权录屏）」，同意系统录屏授权 → 显示**识别码 + 临时密码**。
4. 点「开启无障碍」，在系统设置里启用本应用的无障碍服务（否则只能看画面、无法远程点击）。
5. 在任意浏览器打开控制端网页，输入识别码 + 密码即可远程查看/操作本机。

## 构建

需要 Android SDK（platform 34、build-tools 34）与 JDK 17+。

```bash
cd android
# 配置 SDK 路径（或设置 ANDROID_HOME）
echo "sdk.dir=/path/to/android-sdk" > local.properties
gradle :app:assembleDebug        # 或 ./gradlew（先 gradle wrapper 生成 wrapper）
# 产物：app/build/outputs/apk/debug/app-debug.apk
```

## 权限说明

- 录屏（MediaProjection）：用于捕获屏幕画面。
- 无障碍（AccessibilityService，`canPerformGestures`）：Android 仅允许无障碍服务合成触摸事件，因此远程点击/滑动需要用户手动启用一次。
- 前台服务（mediaProjection 类型）+ 通知：保证后台持续捕获。

## 版本号

- 版本在 `app/build.gradle.kts` 的 `versionName`(展示,如 `1.1.0`)与 `versionCode`(整数,每次发布 +1)。
- App 首页会显示 `版本 v<versionName> (<versionCode>)`,方便确认装的是哪一版。
- **约定:每次发布 APK 前都递增 `versionCode`(+1)并更新 `versionName`。**
- 当前:`1.2.0 (8)`。

## 说明

- 文本/物理按键注入在移动端受限，当前实现聚焦触摸（点击/滑动/滚动）。
- 这是可安装、可联网注册的实现；实际远程控制效果请在真机上验收。
