# ESP-IDF 顶层工程文件
# 用法：
#   source /mnt/scatch/xiaolonz/esp/esp-env.sh
#   cd firmware/audio_bringup
#   idf.py set-target esp32s3
#   idf.py build            # 无需插板即可验证编译
#   idf.py -p /dev/ttyUSB0 flash monitor
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(audio_bringup)
