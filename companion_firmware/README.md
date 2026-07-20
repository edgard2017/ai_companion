# AI Companion 聊天 + 听写固件

这是征辰 EYE ESP32-S3 主板的正式配套固件。ESP 只负责录音、播放、按键、眼睛动画和
局域网通信；Mac 上的 `ai_companion_server` 负责本地 ASR/TTS、豆包聊天和听写状态。

把 Mac 和 ESP 带回家后的完整启动、配网及故障处理步骤见
[`HOME_SETUP.md`](HOME_SETUP.md)。

```text
聊天：按住 BOOT 说话 → /voice-chat → 本地 ASR → 豆包 → 本地 TTS → 播放

听写：短按 BOOT → 创建听写 → 播放第 1 题
                         ├─ 说“好了 / 下一个 / 下一题”
                         ├─ 短按 BOOT
                         └─ 10 秒超时
                              ↓
                         原子推进到下一题
```

原来的 `repeater_demo` 继续保留，用于单独检查麦克风、扬声器、Wi-Fi 和本地 ASR/TTS；
正式聊天和听写使用本目录。

## 已实现的交互

- 聊天模式是开机默认模式。按住 `BOOT` 超过 350 ms，对着设备说话，松开后发送。
- 在聊天模式短按一次 `BOOT`，开始默认听写单元。
- 每道听写提示音**播放完成以后**开始 10 秒窗口。以下任一种方式只推进一道题：
  - 说“好了”“好啦”“下一个”“下一题”或“继续”；
  - 按一下 `BOOT`；
  - 不操作，10 秒后自动推进。
- 说“结束听写”“不听了”或“退出听写”，会播放完成提示并回到聊天模式。
- 最后一题结束后自动播放完成提示并回到聊天模式。
- 网络重试会复用同一个 `request_id`，并使用服务端 `revision` 防止按钮、语音和超时
  同时发生时跳过两题。

默认听写配置为：

```text
学习者：judy
单元：pep-cn-g3-s1:u1
模式：chinese_word
题数：10
每题等待：10 秒
```

可以执行 `idf.py menuconfig`，在 `AI Companion firmware` 中修改默认单元、模式、题数、
音量、录音时长、VAD 阈值和按键时长。当前 demo 使用编译期默认单元，还没有在设备上
做语音选书界面。

## 硬件配置

本工程使用当前征辰 EYE 板载硬件，不使用早期 `audio_bringup` 的通用麦克风方案：

| 功能 | 硬件 / GPIO |
|---|---|
| 麦克风 ADC | ES7210，24 kHz、单声道 PCM16 |
| 扬声器 DAC | ES8311，24 kHz、单声道 PCM16 |
| I2C | SDA 1，SCL 2 |
| I2S | MCLK 38，WS 13，BCLK 14，DIN 12，DOUT 45 |
| BOOT 按键 | GPIO 0 |
| 双圆屏串联链 | MOSI 43，SCLK 44，RESET 46，DC 8，背光 42 |

两块圆屏在硬件上属于同一条串联显示链，不能分别传送左眼和右眼。固件只初始化一个
SPI3 面板，给两块屏发送同一幅左右对称的中性眼睛，因此不会再出现“两只左眼”的观感。
眼睛会用动画区分空闲、聆听、思考、说话、听写和错误状态。

## 1. 准备并启动 Mac 服务

在同一局域网中的 Mac 上：

```sh
cd /path/to/ai_companion_server
./start_server.sh --check
./start_server.sh
```

听写前要先按服务端 README 执行一次：

```sh
conda activate ai-companion-server
python ingest_materials.py
```

聊天还要求 `.env` 中的豆包配置有效。检查健康状态：

```sh
curl http://127.0.0.1:8765/health
```

至少确认 `asr_ready`、`catalog_ready` 为 `true`；聊天还要确认 `llm_configured` 为 `true`。
第一次 TTS 请求会加载 1.7B 模型，时间会明显长于后续请求。

## 2. 第一次开机配网

Wi-Fi 密码不会写入代码：

1. 给设备上电；上电或复位时不要一直按着 `BOOT`，否则 ESP32-S3 会进入下载模式。
2. 如果没有可用的旧配置，设备会创建开放热点 `AI-Companion-Setup`。
3. 用手机连接该热点，打开 `http://192.168.4.1`。
4. 填写家里 Wi-Fi 和 Mac 服务**根地址**，例如：

   ```text
   http://<Mac-LocalHostName>.local:8765
   ```

5. 保存后设备自动重启。以后会从 NVS 读取配置并自动联网。

Mac 的 Bonjour 名称可通过 `scutil --get LocalHostName` 查询。如果路由器不支持 mDNS，
使用 `ipconfig getifaddr en0` 查询的 Wi-Fi IP，例如 `http://192.168.1.23:8765`。
当前构建已预填这台开发 Mac 的 `xiaolonzdeMacBook-Pro.local`；换 Mac 时必须在这里改掉。
从旧复读机固件升级时，NVS 中保存的 `/repeat` 或 `/voice-chat` 完整地址会自动转换成
服务根地址，不必重新配网。连续 20 秒无法连接原 Wi-Fi 时，配置热点会再次出现。

## 3. 编译与烧录

已验证环境为 ESP-IDF 5.5.4：

```sh
cd ai_companion/companion_firmware
source ~/esp/esp-env.sh
idf.py build
idf.py merge-bin -o ai-companion-chat-dictation-merged.bin
```

用 ESP-IDF 直接烧录并查看日志：

```sh
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

也可以把下面的合并固件写到地址 `0x0`：

```text
build/ai-companion-chat-dictation-merged.bin
```

当前编译产物约 1.1 MiB，固件分区为 4 MiB。`build/`、合并 BIN、`sdkconfig`、下载的
组件和密钥均被 Git 忽略；源码中的 `sdkconfig.defaults` 和 `partitions.csv` 足以在新电脑
复现构建。

## VAD 调整

听写等待期间，ESP 使用平均 PCM 能量截取短语，再把 24 kHz WAV 发给 Mac `/asr`。
默认阈值为 900、结束静音为 650 ms，并保留 200 ms 语音前滚。若家里环境较吵、设备
总是误触发，可在 `menuconfig` 提高 `COMPANION_VAD_ENERGY_THRESHOLD`；如果靠近说话也
检测不到，则降低它。串口日志会打印检测能量和 ASR 文字，便于实机校准。

## 当前边界

- 录音、播放是半双工；设备说话时不会同时听命令。
- HTTP 只适用于可信家庭局域网。可选 Bearer token 能防止随手调用，但不等同于 TLS。
- 当前硬件的两块屏只能同步显示。若将来要独立左右眼，需要修改 PCB/排线连接方式。
- 本次构建验证了编译、链接、分区和合并 BIN；烧录与 VAD 阈值仍需在实机上验收。
