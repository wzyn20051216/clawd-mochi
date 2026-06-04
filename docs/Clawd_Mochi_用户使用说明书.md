# Clawd Mochi 用户使用说明书

本文档面向第一次拿到 Clawd Mochi 的用户。照着做，你可以完成硬件接线、固件烧录、网页配网、语音模块配置，以及 Claude / Codex 桌宠状态联动。

## 0. 最快上手流程

如果你已经拿到了接好线并烧录好的成品，只需要做这几步：

1. 给 Clawd Mochi 上电。
2. 手机或电脑连接热点 `ClaWD-Mochi`，密码 `clawd1234`。
3. 浏览器打开 `http://192.168.4.1`。
4. 扫描并连接你的正常 WiFi。
5. 电脑切回正常 WiFi，打开 `http://clawd-mochi.local`。
6. 如果要联动 Codex / Claude Code，在电脑上运行安装桥接器命令：

```powershell
powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -MochiHost clawd-mochi.local
```

如果你是从零开始制作，请继续往下看硬件接线和烧录步骤。

## 1. 这是一个什么项目

Clawd Mochi 是一个 ESP32-S3 桌面小宠物。它有一块小屏幕，可以显示眨眼、开心、困困、生气、惊讶、爱心等表情；也可以连接 WiFi，用网页控制；还可以和 Codex / Claude Code 联动，在 AI 思考、运行工具、完成任务或报错时做出对应表情和语音播报。

当前主线使用：

- ESP32-S3 开发板
- ST7735 1.8 寸 128x160 屏幕，默认启用
- ST7789 1.54 寸 240x240 屏幕，已保留配置，后续可切换
- 亚博智能 / CI1302 离线 AI 语音交互模块
- ESP-IDF 固件工程
- 电脑端 Python 桥接器

## 2. 你需要准备什么

### 硬件

| 名称 | 说明 |
| --- | --- |
| ESP32-S3 开发板 | 任意常见 ESP32-S3 开发板均可，注意 GPIO 是否可用 |
| ST7735 屏幕 | 1.8 寸 128x160，SPI 接口，当前默认屏幕 |
| 亚博智能 AI 语音交互模块 | CI1302 / 类似 UART 协议模块 |
| 杜邦线 | 用于连接屏幕和语音模块 |
| USB 数据线 | 用于烧录 ESP32-S3 |
| 3D 打印外壳 | 可选，上游模型在 `upstream\models` 目录 |

### 软件

| 名称 | 用途 |
| --- | --- |
| Windows 10/11 | 当前项目脚本按 Windows PowerShell 写好 |
| ESP-IDF 5.5.x | 编译和烧录 ESP32-S3 固件 |
| Python 3 | 运行电脑端桥接器 |
| VS Code + ESP-IDF 插件 | 推荐开发环境 |
| Codex / Claude Code | 可选，用于 AI 桌宠联动 |

## 3. 硬件接线

### 3.1 屏幕接线

默认屏幕是 ST7735 1.8 寸 128x160。

| LCD 引脚 | ESP32-S3 GPIO |
| --- | --- |
| SDA / MOSI | GPIO10 |
| SCL / SCLK | GPIO8 |
| RES / RST | GPIO2 |
| DC | GPIO1 |
| CS | GPIO4 |
| BL / LED | GPIO3 |
| VCC | 3V3 |
| GND | GND |

注意：

- 不同开发板的 GPIO 位置不同，请对照你的开发板丝印。
- 如果屏幕完全不亮，先检查 `VCC/GND/BL`。
- 有些屏幕的 `BL/LED` 可以直接接 3V3；如果用 GPIO3 控制异常，可以在 `menuconfig` 里调整背光极性。

### 3.2 语音模块接线

| 语音模块引脚 | ESP32-S3 GPIO |
| --- | --- |
| TX | GPIO18 |
| RX | GPIO21 |
| 5V | 5V |
| GND | GND |

注意：

- 语音模块 TX 接 ESP32 RX。
- 语音模块 RX 接 ESP32 TX。
- 默认串口参数是 `115200 / 8N1`。
- 默认协议格式是 `AA 55 XX YY FB`。

## 4. 编译和烧录固件

### 4.0 获取项目

如果项目已经在你的电脑上，直接进入项目目录即可。

如果未来项目发布到 GitHub，可以用类似下面的方式下载：

```powershell
git clone <你的项目仓库地址>
cd <项目目录>
```

下文里的 `<项目目录>` 指的就是这个工程文件夹，也就是里面能看到 `CMakeLists.txt`、`main`、`tools`、`docs` 的目录。

### 4.1 打开 ESP-IDF 终端

推荐使用 VS Code 的 ESP-IDF 插件终端，或者打开 ESP-IDF PowerShell。

进入项目目录：

```powershell
cd "<项目目录>"
```

首次使用时设置目标芯片：

```powershell
idf.py set-target esp32s3
```

编译：

```powershell
idf.py build
```

如果你的 PowerShell 里有 conda/base，或者普通终端找不到 `idf.py`，可以直接用项目自带脚本：

```powershell
tools\idf_mochi.cmd build
```

看到 `Project build complete` 就说明编译成功。

### 4.2 烧录

先确认 ESP32-S3 的串口号，比如 `COM5`。然后执行：

```powershell
idf.py -p COM5 flash monitor
```

把 `COM5` 换成你电脑上的实际端口。

烧录完成后，屏幕会先显示彩条自检，然后显示 Clawd Mochi 启动画面。

## 5. 第一次配网

Clawd Mochi 上电后会一直保留一个备用热点：

```text
热点名：ClaWD-Mochi
密码：clawd1234
网页： http://192.168.4.1
```

首次配网步骤：

1. 用手机或电脑连接 WiFi 热点 `ClaWD-Mochi`。
2. 打开浏览器，访问 `http://192.168.4.1`。
3. 点击网页里的“扫描”。
4. 选择你的正常 WiFi。
5. 输入 WiFi 密码。
6. 点击“连接”。
7. 页面会显示连接成功和跳转地址。

连接成功后，页面会提示类似：

```text
连接成功！
切换到目标WiFi后请跳转到：http://clawd-mochi.local
备用 IP：http://xxx.xxx.xxx.xxx
桥接 host：clawd-mochi.local
```

这时把电脑或手机切回你的正常 WiFi，然后访问：

```text
http://clawd-mochi.local
```

如果打不开，就看屏幕或网页提示的备用 IP，例如：

```text
http://172.20.10.2
```

## 6. 网页控制说明

网页可以控制桌宠的常用功能。

| 功能 | 说明 |
| --- | --- |
| 普通眼睛 | 切回默认眨眼表情 |
| 眯眯眼 | 显示 squish eyes |
| Claude Code | 显示 Claude Code 风格界面 |
| 画板 | 手机或电脑在网页上画，屏幕同步显示 |
| 开心/困困/生气/惊讶/爱心 | 切换不同表情 |
| 背光 | 打开或关闭屏幕背光 |
| 亮度 | 调整屏幕亮度 |
| 速度 | 调整动画速度 |
| 活跃度 | 调整自动眨眼和小动作频率 |
| 夜间模式 | 暗背景、低亮度、安静动作 |
| 白天模式 | 恢复默认橙色背景和亮度 |
| 扫描 WiFi | 扫描附近 WiFi |
| 临时断开 WiFi | 断开当前 WiFi，但保留密码 |
| 重连已保存 WiFi | 使用保存的 WiFi 重新连接 |
| 恢复出厂 | 清空保存设置，恢复默认状态 |

### 画板使用

1. 打开网页。
2. 点击“画板”。
3. 用手指或鼠标在画布上画。
4. ESP32 屏幕会同步显示。
5. 点击“完成”退出画板。

如果画板没有显示：

- 确认手机和 ESP32 在同一个 WiFi。
- 刷新网页后重新进入画板。
- 如果正在 AI 联动或语音命令频繁触发，稍等几秒再画。

## 7. 语音模块配置

项目提供了已经整理好的语音词条表：

```text
docs\Clawd_Mochi_命令词播报词协议列表V3_中文.xlsx
```

推荐使用 V3 表。它只保留桌宠需要的词条，包括：

- 唤醒语
- 表情控制
- 亮度控制
- 屏幕开关
- 夜间模式 / 白天模式
- 活跃度调整
- 抱一下 / 松开 / 跳舞
- AI 思考、运行、完成、失败等固定播报

### 语音模块使用逻辑

用户说话后，语音模块识别命令词，然后通过 UART 发给 ESP32。ESP32 会改变屏幕状态，必要时再让语音模块播报固定语音。

例如：

| 你说 | 桌宠反应 |
| --- | --- |
| 你好莫奇 | 唤醒 |
| 开心一点 | 开心表情 |
| 困困 | 睡觉表情 |
| 亮一点 | 提高亮度 |
| 暗一点 | 降低亮度 |
| 夜间模式 | 变暗并降低活跃度 |
| 白天模式 | 恢复默认亮度和背景 |
| 抱一下 | 爱心表情 |
| 跳舞 | 提高活跃度 |

### 关于语音模块自动休眠

语音模块本身可能有一个出厂唤醒窗口。唤醒后如果一段时间没有识别到命令，它可能会自己播报休息语。

这和 ESP32 固件里的 10 分钟自动休息不是同一个东西。

当前 ESP32 会忽略语音模块的自动休息帧，不会让屏幕跟着乱休眠。但模块自己的喇叭如果播报休息语，需要在语音模块平台重新制作固件时调整。

推荐做法：

- 使用 V3 词条表。
- 把语音模块的休息语改短，例如“嗯”。
- 如果平台允许，尽量调长唤醒时长或关闭自动休眠播报。

## 8. Claude / Codex 桌宠联动

这个功能可以让桌宠跟随 AI 工作状态变化。

比如：

| AI 状态 | 桌宠表现 |
| --- | --- |
| 会话开始 | 开心 |
| 用户发出任务 | 思考 |
| 调用工具 / 执行命令 | 左右看 |
| 任务完成 | 开心并播报完成 |
| 任务失败 | 生气并播报失败 |
| 等待下一次任务 | 普通眨眼 |

### 8.1 确认 ESP32 已在局域网

先确保 ESP32 已经连接你的正常 WiFi，并且电脑也在同一个 WiFi。

推荐测试：

```powershell
py -3 tools\mochi_bridge.py --ping --host clawd-mochi.local
```

如果成功，屏幕会有反应。

如果失败，可以使用屏幕上显示的 IP：

```powershell
py -3 tools\mochi_bridge.py --ping --host 172.20.10.2
```

### 8.2 安装全局桥接器

执行：

```powershell
powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -MochiHost clawd-mochi.local
```

安装脚本会自动完成：

- 复制桥接器到 `C:\Users\<用户名>\.codex\mochi-bridge`
- 保存 ESP32 地址
- 写入 Codex 全局 hook
- 写入 Claude Code 全局 hook
- 发送测试事件

安装后，正常打开 Codex 或 Claude Code 即可。之后 AI 工作时，桌宠会自动同步状态。

### 8.3 常用维护命令

查看安装状态：

```powershell
powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -Action status
```

发送测试事件：

```powershell
powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -Action test
```

卸载 hook：

```powershell
powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -Action uninstall
```

卸载时只移除全局 hook，桥接文件会保留在：

```text
C:\Users\<用户名>\.codex\mochi-bridge
```

## 9. 常见问题

### 屏幕是白屏

检查：

1. `VCC` 是否接 3V3。
2. `GND` 是否共地。
3. `MOSI/SCLK/DC/CS/RST/BL` 是否按表连接。
4. 屏幕驱动是否选对，默认是 ST7735。
5. 背光极性是否正确。

可尝试：

```powershell
idf.py menuconfig
```

进入：

```text
Clawd Mochi
```

调整：

- `LCD backlight is active high`
- `LCD MADCTL rotation value`
- `LCD X offset`
- `LCD Y offset`

### 颜色反了或方向不对

进入 `menuconfig`，调整：

```text
Clawd Mochi -> LCD MADCTL rotation value
```

ST7735 默认值是：

```text
0xC0
```

### 连接 WiFi 失败

网页会提示原因。常见情况：

| 提示 | 可能原因 |
| --- | --- |
| 密码可能错误 | WiFi 密码输入错 |
| 没有找到这个 WiFi | 路由器太远或 SSID 选错 |
| 信号较弱或路由器拒绝连接 | 信号弱、路由器限制、热点兼容问题 |

处理方法：

1. 连接备用热点 `ClaWD-Mochi`。
2. 打开 `http://192.168.4.1`。
3. 重新扫描并输入密码。

### 电脑连 ESP32 热点后没有外网

这是正常现象。热点 `ClaWD-Mochi` 只是配网和备用控制用。完成配网后，请让 ESP32 和电脑都连接同一个正常 WiFi，这样电脑就有外网，桌宠也能接收 AI 状态。

### 打不开 `clawd-mochi.local`

有些 Windows 或路由器不稳定支持 mDNS。可以使用屏幕显示的 IP 地址。

例如：

```text
http://172.20.10.2
```

### 语音模块说话没反应

检查：

1. 语音模块是否已经烧录 V3 词条固件。
2. TX/RX 是否交叉连接。
3. 语音模块是否供电 5V。
4. 串口波特率是否是 115200。
5. 是否先说了唤醒词。

### AI 联动没有反应

按顺序检查：

1. ESP32 和电脑是否在同一个 WiFi。
2. 浏览器能否打开 `http://clawd-mochi.local`。
3. 手动 ping 是否成功：

```powershell
py -3 tools\mochi_bridge.py --ping --host clawd-mochi.local
```

4. 查看 hook 安装状态：

```powershell
powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -Action status
```

5. 重新安装桥接器：

```powershell
powershell -ExecutionPolicy Bypass -File tools\install_mochi_bridge.ps1 -MochiHost clawd-mochi.local
```

### Codex / Claude 第一次提示 hook 需要确认

如果工具提示是否信任 hook，请确认允许。桥接器只会把 AI 的工作状态发送到 ESP32，不会把 API Key 写进 ESP32 固件。

## 10. 给开发者的配置入口

如果你要换屏幕、改 GPIO 或调整串口参数，执行：

```powershell
idf.py menuconfig
```

进入：

```text
Clawd Mochi
```

可以配置：

| 配置项 | 说明 |
| --- | --- |
| LCD driver | ST7735 或 ST7789 |
| LCD MOSI/SCLK/CS/DC/RST/backlight GPIO | 屏幕接线 |
| LCD backlight is active high | 背光极性 |
| LCD SPI clock Hz | SPI 速度 |
| LCD MADCTL rotation value | 屏幕方向和颜色顺序 |
| LCD X/Y offset | 屏幕偏移 |
| WiFi AP SSID/password | 备用热点名称和密码 |
| WiFi station SSID/password | 预填要连接的 WiFi |
| Voice Module UART port | 语音模块串口号 |
| Voice Module RX/TX GPIO | 语音模块接线 |
| Voice Module baudrate | 语音模块波特率 |

## 11. 日常使用建议

推荐日常流程：

1. 上电。
2. 等待屏幕显示启动动画。
3. ESP32 自动连接保存的 WiFi。
4. 电脑打开 Codex 或 Claude Code。
5. 桌宠自动显示 AI 工作状态。
6. 需要控制时，打开 `http://clawd-mochi.local`。
7. 需要语音控制时，说唤醒词后说命令。

如果送给普通用户，建议提前完成：

- 烧录 ESP32 固件
- 烧录语音模块 V3 固件
- 接好屏幕和语音模块
- 测试网页配网
- 测试至少一个语音命令
- 测试一次 `mochi_bridge.py --ping`

这样用户拿到后只需要配 WiFi，就可以开始使用。

## 12. 版本说明

本文档对应当前 ESP-IDF 移植版主线：

- 默认屏幕：ST7735 1.8 寸 128x160
- 保留屏幕：ST7789 1.54 寸 240x240
- 默认语音：亚博智能 / CI1302 UART 离线语音模块
- 音频路线：已移除旧 I2S 功放和 Windows TTS，统一使用离线语音模块固定播报
- AI 联动：通过电脑端桥接器和全局 hook 实现
