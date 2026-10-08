# ESP32-S31-Korvo-1

This profile ports the ESP32-S31-Korvo-1 v1.1 hardware configuration from the
xiaozhi-esp32 reference implementation into Muse Gadget SDK.

## Supported hardware

- ESP32-S31-WROOM-3, 16 MB flash and 16 MB octal PSRAM
- 800×480 ST7262E43 RGB LCD
- GT1151 capacitive touch
- ES8389 stereo codec, two analog microphones and two NS4150B amplifiers
- BOOT button on GPIO61 for push-to-talk and pairing confirmation
- Native USB Serial/JTAG and UART console

The WS2812 status LED, four ADC ladder buttons, camera, microSD and USB host are
not used by the initial Muse profile. Muse status is shown by the full on-screen
UI; volume and settings are available from the touch UI.

The LCD daughterboard has no controllable backlight. Brightness therefore acts
as an on/off display switch rather than true dimming. GPIO61 is not an RTC wake
pin, so the software power-off action cannot deep-sleep and wake from BOOT; use
RESET or remove power instead.

## 编译、烧录与日志监控

### 前置要求

本项目其他板卡通常使用 ESP-IDF 6.0.1，但 ESP32-S31-Korvo-1 必须使用
**ESP-IDF 6.1 或更高版本**，因为 6.0.1 尚未提供 `esp32s31` target。

以下命令中的 `/path/to/muse-gadget-sdk` 请替换为当前仓库路径。
命令假设：

- 项目位于 `/path/to/muse-gadget-sdk`；
- ESP-IDF 6.1 位于 `/Users/dinglei/.espressif/v6.1/esp-idf`；
- 命令从 macOS 终端执行。

### 1. 进入项目并加载 ESP-IDF 6.1

```sh
cd /path/to/muse-gadget-sdk/esp32
source /Users/dinglei/.espressif/v6.1/esp-idf/export.sh
idf.py --version
```

版本输出应为 `ESP-IDF v6.1` 或更高版本。也可以让板卡脚本通过
`IDF_EXPORT` 找到该工具链：

```sh
export IDF_EXPORT=/Users/dinglei/.espressif/v6.1/esp-idf/export.sh
```

如果当前终端已经加载了其他 ESP-IDF 版本，请新开一个终端，或重新执行
上面的 `source` 命令后再编译。

### 2. 首次生成板卡配置

```sh
tools/muse/board.sh build s31-korvo-1
```

首次执行会下载 managed components，并生成该板卡独立的配置和构建目录：

```text
build-muse-espressif-s31-korvo-1/
```

主要配置文件为：

```text
build-muse-espressif-s31-korvo-1/sdkconfig
```

构建日志写入：

```text
/tmp/muse_build_s31-korvo-1.log
```

### 3. 配置 Muse SDK token

每台 Gadget 都需要从 `gadgets.muse.ai` 获取的 `mgst_...` SDK token。打开
该板卡的配置界面：

```sh
idf.py \
  -B build-muse-espressif-s31-korvo-1 \
  -DIDF_TARGET=esp32s31 \
  -DSDKCONFIG=build-muse-espressif-s31-korvo-1/sdkconfig \
  -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-espressif-s31-korvo-1' \
  menuconfig
```

在菜单中进入：

```text
ESP32 Device SDK
  → Muse Gadgets SDK token
```

填写 token 后保存退出。生成的 `sdkconfig` 含有 token，不要将它提交到 Git，
也不要在日志或聊天中输出完整 token。

### 4. 正式编译

```sh
tools/muse/board.sh build s31-korvo-1
```

编译成功后，应用固件位于：

```text
build-muse-espressif-s31-korvo-1/muse-gadget.bin
```

如果编译失败，查看日志末尾：

```sh
tail -200 /tmp/muse_build_s31-korvo-1.log
```

如需从干净的构建目录重新开始：

```sh
rm -rf build-muse-espressif-s31-korvo-1
```

然后重新执行第 2～4 步。删除构建目录也会删除其中保存的 SDK token。

### 5. 查找串口

将开发板连接到原生 USB 端口，然后执行：

```sh
python3 tools/muse/ports.py --list
```

也可以直接列出 macOS 串口：

```sh
ls /dev/cu.usbmodem*
```

下文使用 `/dev/cu.usbmodemXXXX` 作为示例，请替换成实际端口。

### 6. 烧录固件

如果只连接了一块兼容的 Espressif USB 设备，可以让脚本自动查找：

```sh
tools/muse/board.sh flash s31-korvo-1
```

存在多个设备时，显式指定串口：

```sh
tools/muse/board.sh flash s31-korvo-1 /dev/cu.usbmodemXXXX
```

### 7. 查看启动日志

```sh
idf.py \
  -B build-muse-espressif-s31-korvo-1 \
  -p /dev/cu.usbmodemXXXX \
  monitor
```

按 `Ctrl-]` 退出 monitor。

### 8. 擦除设备状态后重新烧录

普通烧录会保留 Wi-Fi 和配对信息。如需完全清除 Flash：

```sh
idf.py \
  -B build-muse-espressif-s31-korvo-1 \
  -p /dev/cu.usbmodemXXXX \
  erase-flash
```

擦除后重新烧录：

```sh
tools/muse/board.sh flash s31-korvo-1 /dev/cu.usbmodemXXXX
```

### 常用命令汇总

```sh
cd /path/to/muse-gadget-sdk/esp32
source /Users/dinglei/.espressif/v6.1/esp-idf/export.sh

# 编译
tools/muse/board.sh build s31-korvo-1

# 查找串口
python3 tools/muse/ports.py --list

# 烧录
tools/muse/board.sh flash s31-korvo-1 /dev/cu.usbmodemXXXX

# 查看日志
idf.py -B build-muse-espressif-s31-korvo-1 \
  -p /dev/cu.usbmodemXXXX monitor
```

## Hardware verification

After flashing, verify these paths on the physical board:

1. LCD orientation, RGB565 colors and stable refresh without underruns.
2. Touch coordinates and touch-based settings navigation.
3. BOOT push-to-talk and physical pairing confirmation.
4. Both microphones capture intelligible 16 kHz audio.
5. Left and right speaker output and volume control.
6. BLE pairing, Wi-Fi join, Muse VM connection, text replies and images.
7. OTA only after the normal development build has been stable.

## Sources

- [Espressif user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-korvo-1/user_guide.html)
- [Espressif schematic](https://dl.espressif.com/schematics/esp32-s31-korvo-1-schematics.pdf)
- [xiaozhi-esp32 reference board](https://github.com/78/xiaozhi-esp32/tree/main/main/boards/espressif/esp32-s31-korvo-1), ported from local revision `c7241272f2d5fd140c77542f3cf12d09e717fc2f`
