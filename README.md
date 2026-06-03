# Clawd Mochi ESP32-S3 ESP-IDF 移植版

本工程从开源项目 `yousifamanuel/clawd-mochi` 拉取代码，并将 Arduino `.ino` 版本移植为 ESP-IDF 工程，目标开发板为 ESP32-S3。

当前默认屏幕已切到 **ST7735 1.8 寸 128x160**，后续 1.54 寸 ST7789 240x240 到货后可在 `menuconfig` 中切回。

## 当前功能

- ESP32-S3 AP+STA 双模式：保留热点 `ClaWD-Mochi`，也可连接你的正常 WiFi
- 浏览器控制页面：热点模式 `http://192.168.4.1`，局域网模式优先使用 `http://clawd-mochi.local`
- ST7735 128x160 与 ST7789 240x240 双驱动保留
- normal eyes / squish eyes / Claude Code / canvas 绘图模式
- 背光开关、动画速度、背景色、画笔颜色控制

## 工程结构

```text
E:\desktop\clawd mochi
├── CMakeLists.txt
├── sdkconfig.defaults
├── main
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild
│   ├── display.cpp
│   ├── display.hpp
│   └── main.cpp
└── upstream
    └── clawd_mochi.ino
```

## 默认接线

默认沿用原项目 SPI 接线，但 ESP32-S3 不同开发板的可用排针可能不同，建议先对照你的开发板原理图确认。

| LCD 引脚 | ESP32-S3 GPIO |
| --- | --- |
| SDA / MOSI | GPIO10 |
| SCL / SCLK | GPIO8 |
| RES / RST | GPIO2 |
| DC | GPIO1 |
| CS | GPIO4 |
| BL | GPIO3 |
| VCC | 3V3 |
| GND | GND |

## I2S 语音输出

第一阶段已加入 I2S 扬声器输出，推荐使用 `MAX98357A` I2S 数字功放和一个小喇叭。

默认接线：

| MAX98357A 引脚 | ESP32-S3 GPIO |
| --- | --- |
| BCLK / BCK | GPIO15 |
| LRC / WS | GPIO16 |
| DIN | GPIO17 |
| VIN | 5V 或 3V3 |
| GND | GND |

音频格式固定为：

```text
16000 Hz / 16-bit little-endian / mono PCM
```

网页里可以点击“测试声音”，也可以用电脑端脚本发送测试音：

```powershell
py -3 tools\mochi_audio.py
```

如果 GPIO 不方便，在 `idf.py menuconfig` 的 `Clawd Mochi -> Audio` 里修改 `BCLK / WS / DOUT`。

## 屏幕驱动切换

当前默认：

```text
ST7735 1.8 inch 128x160
```

如需切到未来的 ST7789 1.54 寸 240x240，执行：

```powershell
idf.py menuconfig
```

进入：

```text
Clawd Mochi
```

修改 `LCD driver`，可选：

```text
ST7735 1.8 inch 128x160
ST7789 1.54 inch 240x240
```

同一菜单里也可以修改 `LCD MOSI/SCLK/CS/DC/RST/backlight GPIO`、`LCD backlight is active high`、`LCD SPI clock Hz`、`LCD MADCTL rotation value`、`LCD X/Y offset`。

## 连接正常 WiFi

为了让电脑保持外网，同时还能把 Claude/Codex 状态推送到小屏，建议让 ESP32-S3 连接你的正常 WiFi。

首次配网：

```text
1. 电脑或手机连接热点 ClaWD-Mochi，密码 clawd1234
2. 打开 http://192.168.4.1
3. 点击“扫描”
4. 选择你的正常 WiFi，输入密码，点击“连接”
5. 等待小屏显示 LAN 地址
```

连接成功后，WiFi 名和密码会保存到 NVS，掉电不丢。之后上电会自动连接这个 WiFi，同时仍保留 `ClaWD-Mochi` 备用热点。

如果目标 WiFi 暂时连不上，固件会先快速自动重连 3 次；仍失败时不删除 NVS 里的 WiFi 密码，转为每 10 秒后台重试一次。此时可以连接备用热点 `ClaWD-Mochi`，打开 `http://192.168.4.1` 继续控制或重新配网。

如果只是想临时断开当前 WiFi，在网页里点击：

```text
临时断开WiFi
```

这只会断开 STA 连接，保存的 WiFi 仍然保留。需要再次连接时点击：

```text
重连已保存WiFi
```

如果要真正清掉保存的 WiFi 和其他设置，使用网页里的“恢复出厂”。

连接成功后屏幕会显示固定局域网域名和备用 IP：

```text
clawd-mochi.local
xxx.xxx.xxx.xxx
```

电脑保持连接同一个正常 WiFi，然后优先打开：

```text
http://clawd-mochi.local
```

如果你的 Windows 或路由器没有正确处理 mDNS，再使用屏幕下方显示的备用 IP。

高级用法：也可以在 `idf.py menuconfig` 的 `Clawd Mochi` 菜单里预填 `WiFi station SSID` 和 `WiFi station password`，但日常使用推荐网页配网。

如果 ST7735 出现颜色反、方向不对、显示偏移，优先调：

```text
LCD backlight is active high
LCD MADCTL rotation value
LCD X offset
LCD Y offset
```

如果屏幕完全黑屏，先试着取消勾选 `LCD backlight is active high` 重新烧录；如果还是黑，再检查 `BL/LED` 是否需要直接接 3V3。

## 构建

推荐在 ESP-IDF PowerShell 终端中执行：

```powershell
cd "E:\desktop\clawd mochi"
idf.py set-target esp32s3
idf.py build
```

如果当前终端开着 conda/base，推荐直接用项目自带包装脚本，它会临时加载 ESP-IDF 5.5.2 环境并同步桌宠状态：

```powershell
tools\idf_mochi.cmd build
```

如果普通 PowerShell 没加载 ESP-IDF 环境，可先运行：

```powershell
cd "E:\Espressif\frameworks\esp-idf-v5.5.2"
.\export.ps1
cd "E:\desktop\clawd mochi"
idf.py build
```

## 烧录与监视

把 `COMx` 换成你的串口号：

```powershell
idf.py -p COMx flash monitor
```

本次已验证生成固件：

```text
E:\desktop\clawd mochi\build\clawd_mochi_s3.bin
```

## 已知限制

- 当前 ESP-IDF 页面是移植后的轻量控制页，不是原 `.ino` 内嵌 HTML 的逐字节复刻。
- 显示层使用 240x240 RGB565 DMA 帧缓冲，约占 115KB RAM，适合 ESP32-S3。
- HTTP 请求处理期间会同步执行动画，动画过程中新的 Web 请求会短暂等待。
- 未接入物理按键、传感器、音频或 OTA。

## Claude / Codex 桌宠桥接

固件提供轻量桌宠接口：

```text
GET http://192.168.4.1/pet?mood=happy&text=build%20ok
```

如果小屏已经连上正常 WiFi，优先直接使用固定 mDNS 地址：

```powershell
py -3 tools\mochi_bridge.py happy "build ok" --host clawd-mochi.local
py -3 tools\mochi_bridge.py thinking "coding..." --host clawd-mochi.local
py -3 tools\mochi_bridge.py error "build failed" --host clawd-mochi.local
```

桥接器也会自动尝试：显式 host、环境变量、mDNS 自动发现、保存的 host、`clawd-mochi.local`、`192.168.4.1`，所以多数情况下可以不再手动输入 IP。

也可以先设置环境变量，后续省略 `--host`：

```powershell
$env:MOCHI_HOST="clawd-mochi.local"
py -3 tools\mochi_bridge.py thinking "coding..."
```

更推荐把当前 LAN 地址保存一次：

```powershell
py -3 tools\mochi_bridge.py --set-host clawd-mochi.local
py -3 tools\mochi_bridge.py --ping
py -3 tools\mochi_bridge.py --demo
```

保存后常用状态可以直接发送：

```powershell
py -3 tools\mochi_bridge.py thinking "coding..."
py -3 tools\mochi_bridge.py happy "done"
py -3 tools\mochi_bridge.py error "build failed"
```

也可以让它包住本地命令，自动显示“运行中/成功/失败”：

```powershell
tools\idf_mochi.cmd build
py -3 tools\mochi_task.py --name test -- py -3 -m py_compile tools\mochi_bridge.py
```

`mood` 可用：`normal`、`happy`、`thinking`、`error`、`surprise`、`sleepy`、`love`、`wink`、`look`。

当前 LCD 字体仅支持 ASCII，`text` 建议使用短英文或数字。后续接 Claude/Codex 时，推荐由电脑端桥接器保管 API Key，再通过 `/pet` 把状态推送到 ESP32-S3，避免把密钥写进固件。

### 真实 Agent 状态桥接

项目已提供 Codex / Claude Code 的事件桥接器。它会读取官方 JSON 事件流或 hook 事件，并把状态映射到 ESP32：

| Agent 状态 | ESP32 表情 |
| --- | --- |
| 会话开始 | 开心 |
| 收到用户任务 / 正在思考 | 思考 |
| 调用工具 / 执行命令 | 左右看 |
| 工具成功 / 任务完成 | 开心 |
| 工具失败 / 任务失败 | 生气 |
| 等待下一步 | 普通眨眼 |

推荐使用一键安装脚本。现在优先使用固定 mDNS 地址：

```powershell
powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -MochiHost clawd-mochi.local
```

安装器会自动完成：

```text
复制桥接器到 C:\Users\<你>\.codex\mochi-bridge
保存 ESP32 Host
写入 Codex 全局 hook
写入 Claude Code 全局 hook
发送测试事件
```

常用维护命令：

```powershell
powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -Action status
powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -Action test
powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -Action uninstall
```

安装后正常打开 Codex / Claude Code 即可。首次加载 hook 时可能会要求在 `/hooks` 中确认信任。确认后，正常交互时会自动把 `SessionStart`、`UserPromptSubmit`、`PreToolUse`、`PostToolUse`、`Stop` 等事件同步到 ESP32。

也可以用包装脚本运行一次性真实 agent：

```powershell
tools\codex_mochi.cmd "检查当前项目状态，只回复一句话"
tools\claude_mochi.cmd "检查当前项目状态，只回复一句话"
```

如果希望任意项目都自动同步，可把同样的 hooks 写到用户级配置：

```text
C:\Users\23201\.codex\hooks.json
C:\Users\23201\.claude\settings.json
```

全局 hook 命令必须使用 `mochi_event.py` 的绝对路径，例如：

```text
py -3 "C:/Users/23201/.codex/mochi-bridge/mochi_event.py"
```

为避免误删项目后全局桥接失效，建议把全局桥接器固定放在：

```text
C:\Users\23201\.codex\mochi-bridge
```

## 上游来源

原始 Arduino 工程已保留在：

```text
E:\desktop\clawd mochi\upstream
```
