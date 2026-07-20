# AI Companion · 陪读小精灵

给小朋友使用的 ESP32-S3 语音伙伴。当前 demo 已经完成两个核心功能：

- 聊天：按住 BOOT 说话，由 Mac 本地 ASR、豆包大模型和本地 1.7B TTS 生成回答。
- 听写：短按 BOOT 开始；说“好了/下一个”、按一下 BOOT 或等待 10 秒，都能进入下一题。

ESP 负责麦克风、扬声器、按键、双圆眼屏和局域网通信；模型和教材状态放在 Mac 的独立
仓库 [`edgard2017/ai_companion_server`](https://github.com/edgard2017/ai_companion_server)。

## 当前硬件

- ESP32-S3，16 MB Flash，Octal PSRAM。
- ES7210 麦克风 ADC，ES8311 扬声器 DAC，24 kHz PCM16。
- 两块 0.71 英寸圆形 LCD。两屏属于同一串联链，只能同步显示；正式固件使用对称眼睛素材。
- 一个 GPIO0 / BOOT 按键。

早期文档里的 INMP441、MAX98357A 和水墨屏只是立项阶段方案，不是目前实机配置。

## 软件结构

```text
ai_companion/
├── companion_firmware/  # 正式聊天 + 听写固件
├── repeater_demo/       # 只验证本地 ASR/TTS 和音频链路
├── eye_demo/            # 只验证圆屏和眼睛素材
├── eye_assets/          # 眼睛原始与生成素材
├── firmware/            # 早期通用音频 bring-up，非当前板载音频
└── docs/                # 决策与硬件记录
```

正式固件的配网、编译、烧录、交互和 VAD 调整说明见
[`companion_firmware/README.md`](companion_firmware/README.md)。

## 当前架构

```text
ESP32-S3 ──24k WAV / HTTP──▶ Mac ai_companion_server
   ▲                            ├─ whisper.cpp 本地 ASR
   │                            ├─ 豆包 LLM
   └────────24k WAV─────────────└─ Qwen3-TTS 1.7B 本地 TTS
```

家庭 demo 默认使用同一局域网的 HTTP。Wi-Fi 和 Mac 地址通过手机配网页保存到 ESP NVS；
模型密钥只放在 Mac `.env`，不写入固件或 Git。
