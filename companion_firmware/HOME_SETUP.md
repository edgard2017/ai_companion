# AI Companion 回家部署与使用指南

本文说明把 Mac 和 ESP32 带回家以后，如何连接家庭网络、启动服务并使用聊天和听写功能。

## 当前准备状态

- Mac Server 代码已经准备完成。
- ESP Client 固件已经编译完成。
- 服务端自动化测试以及 ESP-IDF 编译、链接、合并 BIN 均已通过。
- 仍需要在家庭局域网中完成一次真实设备联调和 VAD 灵敏度校准。

ESP 可烧录的合并固件位于：

```text
build/ai-companion-chat-dictation-merged.bin
```

## 整体启动顺序

```text
Mac 连接家里 Wi-Fi
        ↓
Mac 运行 start_server.sh
        ↓
ESP 上电并进入配网模式
        ↓
手机给 ESP 填写家里 Wi-Fi
        ↓
ESP 和 Mac 在同一局域网通信
        ↓
开始聊天或听写
```

## 1. 启动 Mac Server

让 Mac 连接家里的 Wi-Fi，然后打开终端：

```sh
cd /Users/xiaolonz/works/ai_companion_server
./start_server.sh
```

这个终端窗口需要一直保持运行。脚本默认会避免 Mac 在服务运行期间自动睡眠。

再打开一个终端，检查服务是否正常：

```sh
curl http://127.0.0.1:8765/health
```

看到下面的字段表示服务已经启动：

```json
{
  "ok": true
}
```

还应关注以下状态：

- `asr_ready`：本地语音识别是否已经启动。
- `catalog_ready`：听写教材数据库是否可以使用。
- `llm_configured`：豆包聊天模型是否已经配置。

第一次调用 TTS 时需要加载本地 1.7B 模型，因此第一次回答会比后续回答慢。

## 2. 确认 Mac 的局域网地址

固件当前预填的 Mac 地址是：

```text
http://xiaolonzdeMacBook-Pro.local:8765
```

可以用下面的命令确认 Mac 的 Bonjour 名称：

```sh
scutil --get LocalHostName
```

当前应返回：

```text
xiaolonzdeMacBook-Pro
```

如果家里的路由器不支持 `.local` 地址，可以查询 Mac 的 Wi-Fi IP：

```sh
ipconfig getifaddr en0
```

假设返回 `192.168.1.23`，ESP 中填写的服务地址就是：

```text
http://192.168.1.23:8765
```

## 3. 确认 ESP 固件已经烧录

“固件已经下载到 Mac”和“固件已经烧录进 ESP”不是同一件事。

如果 ESP 还没有烧录，可以连接 USB 后执行：

```sh
cd /Users/xiaolonz/works/ai_companion/companion_firmware
source ~/esp/esp-env.sh
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

也可以使用烧录工具，把下面的合并固件写入地址 `0x0`：

```text
/Users/xiaolonz/works/ai_companion/companion_firmware/build/ai-companion-chat-dictation-merged.bin
```

上电或复位时不要一直按住 `BOOT`，否则 ESP32-S3 会进入下载模式。

## 4. 给 ESP 配置家里的 Wi-Fi

ESP 上电后会先尝试连接以前保存的 Wi-Fi。

如果以前保存的是公司 Wi-Fi，在家里连接不上时，ESP 等待约 20 秒后会自动创建热点：

```text
AI-Companion-Setup
```

使用手机完成配网：

1. 打开手机的 Wi-Fi 设置。
2. 连接 `AI-Companion-Setup`，该热点没有密码。
3. 打开手机浏览器。
4. 访问 `http://192.168.4.1`。
5. 填写家里的 Wi-Fi 名称和密码。
6. 确认 Mac 服务地址为：

   ```text
   http://xiaolonzdeMacBook-Pro.local:8765
   ```

7. 点击“保存并连接”。
8. ESP 自动重启并连接家里的 Wi-Fi。

服务地址只需要填写到端口 `8765`，不要添加 `/repeat`、`/voice-chat` 或其他 API 路径。

### Wi-Fi 频段说明

ESP32-S3 只支持 2.4GHz Wi-Fi。

Mac 可以连接同一路由器的 5GHz，只要路由器允许 2.4GHz 和 5GHz 设备互相访问即可。
如果路由器开启了“客户端隔离”或“AP 隔离”，需要将其关闭。

## 5. 使用聊天功能

设备启动并联网后，默认处于聊天模式。

操作方法：

```text
按住 BOOT
→ 对着设备说话
→ 松开 BOOT
→ 等待 Mac 处理
→ ESP 播放回答
```

处理链路为：

```text
ESP 录制 24kHz WAV
→ Mac 本地 ASR
→ 豆包大语言模型
→ Mac 本地 1.7B TTS
→ ESP 扬声器播放
```

聊天录音默认最长 8 秒。

## 6. 使用听写功能

在聊天模式下短按一次 `BOOT`，设备会开始默认听写：

```text
教材：三年级上册语文
单元：第一单元
模式：中文词语听写
数量：10 个
```

每道题播放完成后，可以通过以下三种方式进入下一题。

### 方式一：语音

可以说：

```text
好了
好啦
下一个
下一题
继续
```

### 方式二：按键

短按一次 `BOOT`。

### 方式三：自动超时

不进行任何操作，等待 10 秒后自动进入下一题。

如需提前结束，可以说：

```text
结束听写
不听了
退出听写
```

最后一道题完成后，设备会播放完成提示并自动回到聊天模式。

## 7. 常见问题

### 找不到 `AI-Companion-Setup`

先等待至少 20 秒。如果仍然没有出现：

1. 确认正式固件已经烧录进 ESP。
2. 确认上电时没有一直按住 `BOOT`。
3. 重新给设备断电再上电。

如果 ESP 已经连接了一个可用 Wi-Fi，但保存的 Mac 地址错误，它不会自动进入配网模式。
这种情况下可以擦除旧 NVS 配置并重新烧录：

```sh
cd /Users/xiaolonz/works/ai_companion/companion_firmware
source ~/esp/esp-env.sh
idf.py -p /dev/cu.usbmodemXXXX erase-flash flash monitor
```

`erase-flash` 会清除设备中保存的 Wi-Fi 配置，之后需要重新使用手机配网。

### 手机打不开 `192.168.4.1`

- 确认手机仍连接着 `AI-Companion-Setup`。
- 暂时关闭手机移动数据，防止手机自动切换网络。
- 在浏览器中完整输入 `http://192.168.4.1`，不要使用 HTTPS。

### ESP 已联网，但不能访问 Mac

依次检查：

1. Mac 和 ESP 是否连接同一个家庭路由器。
2. `start_server.sh` 的终端窗口是否仍在运行。
3. macOS 防火墙是否允许 Python 接收入站连接。
4. 路由器是否开启了客户端隔离。
5. `.local` 地址不通时，改用 Mac 的局域网 IP。

可以先用同一 Wi-Fi 下的手机访问：

```text
http://xiaolonzdeMacBook-Pro.local:8765/health
```

如果手机也访问不了，说明问题在 Mac 服务、防火墙或局域网，而不是 ESP 固件。

### 聊天返回错误

检查 `/health` 中：

```text
llm_configured = true
```

如果为 `false`，检查 Mac 的 `.env` 中豆包 API Key 和模型配置。

### 听写无法开始

检查 `/health` 中：

```text
catalog_ready = true
```

如果为 `false`，在 Mac 上执行：

```sh
cd /Users/xiaolonz/works/ai_companion_server
conda activate ai-companion-server
python ingest_materials.py
```

### 说“好了”没有反应

先查看 ESP 串口日志中的语音能量和 ASR 文字。

如果靠近设备说话也检测不到，可以降低 VAD 阈值；如果环境噪声经常误触发，则提高阈值：

```sh
cd /Users/xiaolonz/works/ai_companion/companion_firmware
source ~/esp/esp-env.sh
idf.py menuconfig
```

进入：

```text
AI Companion firmware
→ Voice activity detection
→ Average absolute PCM energy threshold
```

默认阈值为 `900`，需要根据家里的真实环境进行一次校准。

## 每次正常使用

第一次配网成功以后，日常不需要再用手机配置。每次只需要：

```text
1. Mac 连接家里 Wi-Fi
2. Mac 运行 /Users/xiaolonz/works/ai_companion_server/start_server.sh
3. 给 ESP 上电
4. 等待 ESP 自动联网
5. 按住 BOOT 聊天，或短按 BOOT 开始听写
```
