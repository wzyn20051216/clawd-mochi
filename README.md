# Clawd Mochi ESP32-S3 ESP-IDF Port

本工程从开源项目 `yousifamanuel/clawd-mochi` 拉取代码，并将 Arduino `.ino` 版本移植为 ESP-IDF 工程，目标开发板为 ESP32-S3。

当前默认屏幕已切到 **ST7735 1.8 寸 128x160**，后续 1.54 寸 ST7789 240x240 到货后可在 `menuconfig` 中切回。

面向最终用户的安装、配置和使用说明见：[Clawd Mochi 用户使用说明书](docs/Clawd_Mochi_用户使用说明书.md)。

## 免责声明

这是一个独立的社区项目，并非 Anthropic、OpenAI、Claude Code 或 Codex 的官方产品，也不隶属于或受其赞助、认可。Claude、Claude Code、Clawd、Codex、OpenAI 等名称仅用于说明兼容或联动对象，相关商标归各自权利人所有。

本仓库的软件代码按 MIT License 发布。上游项目中的 3D 模型和媒体资源标注为 CC BY-NC-SA 4.0，包含非商业限制；如果你计划商业销售硬件，请重新设计外壳和视觉素材，并自行确认商标、外观和供应链合规。

## 当前功能

- ESP32-S3 AP+STA 双模式：保留热点 `ClaWD-Mochi`，也可连接你的正常 WiFi
- BLE 蓝牙桥接：电脑端桥接器默认用蓝牙发送 Claude/Codex 状态，语音切到 WiFi 后才走局域网
- 浏览器控制页面：热点模式 `http://192.168.4.1`，局域网模式优先使用 `http://clawd-mochi.local`
- ST7735 128x160 与 ST7789 240x240 双驱动保留
- normal eyes / squish eyes / Claude Code / canvas 绘图模式
- 背光开关、动画速度、背景色、画笔颜色控制

## 工程结构

```text
clawd-mochi/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild
│   ├── display.cpp
│   ├── display.hpp
│   ├── voice_module.cpp
│   ├── voice_module.hpp
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

## 离线语音模块

当前推荐使用亚博智能 AI 语音交互模块，作为离线语音命令入口和固定语音播报出口。ESP32-S3 通过 UART 接收模块识别到的命令，并在 WiFi、Claude/Codex hook、夜间模式、屏幕开关、任务完成/失败等状态变化时，反向发送协议让语音模块播报。

默认接线：

| 语音模块引脚 | ESP32-S3 GPIO |
| --- | --- |
| TX | GPIO18 |
| RX | GPIO21 |
| 5V | 5V |
| GND | GND |

串口参数：

```text
115200 / 8N1
协议格式：AA 55 XX YY FB
```

命令词表使用：

```text
docs\Clawd_Mochi_命令词播报词协议列表V4_中文.xlsx
```

V4 表只保留 Claude / Codex 桌宠需要的词条：表情、亮度、屏幕、夜间模式、活跃度、抱一下/松开/跳舞、桥接模式切换，以及 ESP32 主动触发的被动播报语。旧表里小车、巡线、垃圾分类、手动“开始思考/开始运行/任务完成”等调试词已经移除。

注意：语音模块出厂固件有自己的唤醒窗口。资料里说明，唤醒后约 20 秒没有识别到命令词，模块会自己进入休眠并播报休息语。这和 Mochi 固件里的 10 分钟自动休息不是同一个逻辑。ESP32 现在会忽略模块的自动休息帧 `0x0200` 和休息提示帧 `0x026F`，不会跟着切屏幕状态；但模块喇叭自己播出的声音只能通过重新制作语音模块固件来处理。

推荐在启英泰伦平台重新导入 V4 表并制作固件：把固定功能词条里的“休息语”播报改成极短的“嗯”，同时在固件配置页面里尽量把唤醒时长调大，或关闭自动休眠/休眠播报。如果平台不允许关闭，V4 里的极短休息语就是当前最稳妥的降噪方案。Mochi 真正的主动休息仍由 ESP32 的 10 分钟空闲逻辑触发，并会发送 `AA 55 FF 86 FB` 播报“莫奇先休息啦”。

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
cd "<project-dir>"
idf.py set-target esp32s3
idf.py build
```

如果当前终端开着 conda/base，推荐直接用项目自带包装脚本，它会临时加载 ESP-IDF 5.5.2 环境并同步桌宠状态：

```powershell
tools\idf_mochi.cmd build
```

如果普通 PowerShell 没加载 ESP-IDF 环境，可先运行：

```powershell
cd "<esp-idf-dir>"
.\export.ps1
cd "<project-dir>"
idf.py build
```

## 烧录与监视

把 `COMx` 换成你的串口号：

```powershell
idf.py -p COMx flash monitor
```

构建成功后固件位于：

```text
build/clawd_mochi_s3.bin
```

## 已知限制

- 当前 ESP-IDF 页面是移植后的轻量控制页，不是原 `.ino` 内嵌 HTML 的逐字节复刻。
- 显示层使用 240x240 RGB565 DMA 帧缓冲，约占 115KB RAM，适合 ESP32-S3。
- HTTP 请求处理期间会同步执行动画，动画过程中新的 Web 请求会短暂等待。
- 未接入物理按键、传感器或 OTA。

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

桥接器默认按桌宠当前桥接模式工作：

- `auto`：BLE 蓝牙优先，不主动连接保存的 WiFi
- `ble`：只走 BLE，电脑和 ESP32 不需要同一个 WiFi
- `wifi`：只走 WiFi HTTP，并开始连接保存的 WiFi；需要电脑和 ESP32 在同一个局域网

用户可以直接对语音模块说“自动桥接”“使用蓝牙”“使用 WiFi”来切换模式。切换后会保存到 ESP32 的 NVS，掉电不丢。只要没有明确切到“使用 WiFi”，固件不会后台连接保存的 WiFi，也不会因为 WiFi 失败打断蓝牙或播报“连接失败”。

开发调试时也可以临时禁用 BLE，只走 WiFi。执行前先对桌宠说“使用 WiFi”，并确认 ESP32 已拿到局域网 IP：

```powershell
$env:MOCHI_BLE="0"
py -3 tools\mochi_bridge.py --ping --host clawd-mochi.local
```

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

默认情况下，`mochi_bridge.py` 会优先使用本机常驻后台桥接器。后台只监听当前电脑自己的 `127.0.0.1:27665`，不是写死某个用户的局域网 IP；每个用户电脑上的 hook 都会打到自己的本机后台。后台会长期保持 BLE 连接，并在断线后自动重连：

```powershell
py -3 tools\mochi_bridge.py --daemon-status
py -3 tools\mochi_bridge.py --daemon-stop
py -3 tools\mochi_bridge.py --daemon
```

返回里看到 `transport: ble-daemon` 表示状态事件已通过常驻后台发送。ESP32 的睡眠只是屏幕和表情的假睡，后台仍然运行；一旦 Claude/Codex 或语音事件触发，桌宠会自动醒来。

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

当前 LCD 字体仅支持 ASCII，`text` 建议使用短英文或数字。接 Claude/Codex 时，由电脑端桥接器通过 BLE 或 `/pet` 推送状态，API Key 不会写入 ESP32-S3 固件。

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

推荐使用一键安装脚本。安装器会尝试安装 Python BLE 依赖 `bleak`，用于蓝牙优先桥接；安装失败也不会影响 WiFi 模式。

Windows：

```powershell
powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -MochiHost clawd-mochi.local
```

macOS / Linux：

```bash
python3 tools/install_mochi_bridge.py --host clawd-mochi.local
# 或者
sh tools/install_mochi_bridge.sh --host clawd-mochi.local
```

安装器会自动完成：

```text
复制桥接器到用户目录下的 .codex/mochi-bridge
尽量安装 BLE 依赖 bleak
保存 ESP32 Host
启动本机常驻桥接器
写入 Codex 全局 hook
写入 Claude Code 全局 hook
发送测试事件
```

常用维护命令：

Windows：

```powershell
powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -Action status
powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -Action test
powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -Action uninstall
```

macOS / Linux：

```bash
python3 tools/install_mochi_bridge.py --action status
python3 tools/install_mochi_bridge.py --action test
python3 tools/install_mochi_bridge.py --action uninstall
```

安装后正常打开 Codex / Claude Code 即可。首次加载 hook 时可能会要求在 `/hooks` 中确认信任。确认后，正常交互时会自动把 `SessionStart`、`UserPromptSubmit`、`PreToolUse`、`PostToolUse`、`Stop` 等事件同步到 ESP32。

也可以用包装脚本运行一次性真实 agent：

```powershell
tools\codex_mochi.cmd "检查当前项目状态，只回复一句话"
tools\claude_mochi.cmd "检查当前项目状态，只回复一句话"
```

如果希望任意项目都自动同步，可把同样的 hooks 写到用户级配置：

```text
Windows: C:\Users\<你>\.codex\hooks.json
Windows: C:\Users\<你>\.claude\settings.json
macOS/Linux: ~/.codex/hooks.json
macOS/Linux: ~/.claude/settings.json
```

全局 hook 命令必须使用 `mochi_event.py` 的绝对路径，例如：

```text
Windows: py -3 "C:/Users/<你>/.codex/mochi-bridge/mochi_event.py"
macOS/Linux: python3 ~/.codex/mochi-bridge/mochi_event.py
```

为避免误删项目后全局桥接失效，建议把全局桥接器固定放在：

```text
Windows: C:\Users\<你>\.codex\mochi-bridge
macOS/Linux: ~/.codex/mochi-bridge
```

生成面向用户分发的桥接器安装包：

```powershell
py -3 tools\package_mochi_bridge.py --version v1.0.0
```

生成结果在 `dist/` 目录，包含 Windows、macOS、Linux 的安装、测试、状态检查和卸载脚本，以及 SHA-256 校验文件。

## 上游来源

原始 Arduino 工程已保留在：

```text
upstream/
```
