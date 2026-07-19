# 征辰 EYE 局域网复读机 demo

目标链路：

```text
按住 BOOT 说话 → ESP32 录制 24kHz PCM → Mac /repeat
→ Whisper ASR → Qwen3-TTS 复述同一句 → ESP32 扬声器播放
```

这里没有接大模型。它专门验证麦克风、扬声器、Wi-Fi、Mac 本地 ASR/TTS
以及端到端延迟。链路跑通后，只需在 ASR 和 TTS 之间加入对话模型。

## 1. 配置并启动 Mac

Mac 服务端已经迁移到独立仓库。第一次安装：

```sh
git clone https://github.com/edgard2017/ai_companion_server.git
cd ai_companion_server
bash setup_local.sh
```

以后使用 Conda 环境启动，服务默认监听局域网地址 `0.0.0.0:8765`：

```sh
cd ai_companion_server
conda activate ai-companion-server
python run_server.py
```

查询 Mac 的 Bonjour 名称和当前 Wi-Fi 地址：

```sh
scutil --get LocalHostName
ipconfig getifaddr en0
```

先在另一台同网设备访问下面任一地址：

```text
http://<Mac-LocalHostName>.local:8765/health
http://<Mac-WiFi-IP>:8765/health
```

如果 macOS 弹出防火墙提示，允许 Python 接收入站连接。

## 2. 第一次开机配网

Wi-Fi 名称和密码不写死在固件里，也不需要为了换网络重新编译：

1. 第一次开机时，ESP 会创建开放热点 `AI-Companion-Setup`。
2. 用手机连接这个热点，不需要密码。
3. 手机浏览器打开 `http://192.168.4.1`。
4. 填写家里的 Wi-Fi 名称和密码，并把 Mac 地址改为上一步查询到的 Bonjour
   名称或 Wi-Fi IP，然后点击“保存并连接”。
5. ESP 保存配置并重启，以后每次开机会自动连接家里网络。

固件预填的是一个通用示例 URL：

```text
http://ai-companion-mac.local:8765/repeat
```

配网时应把 `ai-companion-mac` 换成 `scutil --get LocalHostName` 查询到的名称。
`.local` 名称在家用路由器和手机热点上通常最方便，Mac IP 变化时不需要修改 ESP。
如果局域网拦截 mDNS，配网时可以填成：

```text
http://<Mac-WiFi-IP>:8765/repeat
```

Wi-Fi 和 Mac 地址保存在 ESP 的 NVS 中，不会写进代码或提交到仓库。ESP 如果连续
20 秒连不上已保存的网络，会自动重新出现 `AI-Companion-Setup` 热点。因此从公司
拿回家时，只需按上述步骤用手机重新配一次，无需重新烧录。

## 3. 构建、烧录和使用

```sh
cd ai_companion/repeater_demo
source /path/to/esp-idf/export.sh
idf.py build
idf.py merge-bin -o zhengchen-repeater-merged.bin
```

在 Windows 烧录合并固件时，地址使用 `0x0`。运行后：

1. 按住 `BOOT` 键。
2. 对着设备说一句话。
3. 松开 `BOOT` 键。
4. 等待 Mac 完成 ASR 和 TTS，设备扬声器会复述同一句话。

默认最长录音 8 秒、音量 75，都可在 menuconfig 中调整。串口会打印录音时长、
峰值、ASR/TTS 耗时和错误原因。

## 网络注意事项

ESP 和 Mac 需要位于同一个可互访的局域网。公司 Wi-Fi 常见客户端隔离、端口限制
或 mDNS 屏蔽；如果同网仍访问不了 `/health`，最快的 demo 方法是让 Mac 和 ESP
同时连接同一个手机热点。回家后通过 `AI-Companion-Setup` 页面保存家里 Wi-Fi；
默认的 `.local` Mac 地址可保持不变。

本 demo 没有 token，也没有 TLS，只适合可信局域网。这里的 HTTP token 若以后启用，
只是每个请求携带的一段共享口令；它不同于 SSH key 的非对称密钥免密登录。
