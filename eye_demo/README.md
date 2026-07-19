# Zhengchen EYE demo

Standalone offline animation for the 0.71-inch, 160x160 Zhengchen EYE board.
The two displays use independent SPI buses, so the left and right eye assets can
be rendered separately. Both modules are expected to be mounted upright in the
same direction, matching the official L.gif/R.gif asset convention.

The animation displays separate, enlarged blue anime left/right eyes and blinks
periodically.
It does not initialize Wi-Fi, audio, touch input, or the cloud application.

Build with ESP-IDF v5.5.4:

```sh
source "$HOME/esp/esp-env.sh"
idf.py build
idf.py merge-bin -o zhengchen-eye-demo-merged.bin
```

Flash the merged image at address `0x0` with the ESP32-S3 Flash Download Tool.
