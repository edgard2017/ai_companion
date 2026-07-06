// bsp_pins.h —— 板级引脚定义（按你的实际接线修改这里，其余代码不用动）
//
// ⚠️ ESP32-S3 选脚注意：避开这些脚
//   - Strapping: GPIO0 / GPIO3 / GPIO45 / GPIO46
//   - USB-JTAG:  GPIO19 / GPIO20（用原生 USB 烧录/调试时占用）
//   - Flash/PSRAM: GPIO26~32（通常被占，勿用）
//   下面给的是一组安全默认值，接线不同就改成你的。
#pragma once

// ---- 麦克风 INMP441（I2S RX，输入）----
// INMP441 接线：VDD=3V3, GND=GND, L/R=GND(选左声道), SCK=BCK, WS=WS, SD=DIN
#define I2S_MIC_BCK   GPIO_NUM_4    // INMP441 SCK
#define I2S_MIC_WS    GPIO_NUM_5    // INMP441 WS  (LRCLK)
#define I2S_MIC_DIN   GPIO_NUM_6    // INMP441 SD  (数据输入到 ESP32)

// ---- 功放 MAX98357A（I2S TX，输出）----
// MAX98357A 接线：VIN=5V, GND=GND, DIN=DOUT, BCLK=BCK, LRC=WS,
//   SD 脚悬空/拉高=开启（也用于选声道，见规格书）
#define I2S_AMP_BCK   GPIO_NUM_15   // MAX98357A BCLK
#define I2S_AMP_WS    GPIO_NUM_16   // MAX98357A LRC (WS)
#define I2S_AMP_DOUT  GPIO_NUM_7    // MAX98357A DIN (ESP32 数据输出)
