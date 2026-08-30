# ClaudeWatch — Claude Code 状态表 · Waveshare ESP32-S3-Touch-AMOLED-1.75C

把微雪 1.75 寸圆形 AMOLED 开发板做成一块桌面小表：**表盘时钟** + **Claude Code 实时状态**。
Claude 在思考 / 跑工具 / 等你批准权限时，屏幕上的状态环会变色、转动或脉冲，一眼就能看出要不要过去点确认。

```
esp32/
├── build.sh                    编译 / 烧录 / 串口监视（arduino-cli）
├── firmware/
│   ├── ClaudeWatch/            固件源码（Arduino 工程）
│   │   ├── ClaudeWatch.ino     硬件初始化、串口命令、主循环
│   │   ├── ui.cpp / ui.h       LVGL 界面：表盘 + 状态页
│   │   ├── net.cpp / net.h     Wi-Fi、mDNS、NTP、HTTP 接口
│   │   ├── status.cpp/.h       状态模型 + 极简 JSON 解析
│   │   ├── config.h            时区、NTP、mDNS 名、亮度、触摸方向
│   │   └── font_mont_*.c       生成的 Montserrat 120px/64px 字体
│   └── libraries/              微雪随板提供的驱动库（GFX/LVGL 8.4/SensorLib/XPowersLib…）
└── host/
    ├── claude_watch_hook.py    Claude Code hook：把事件汇总成状态推给板子
    ├── install_hooks.py        把 hook 写进 ~/.claude/settings.json（可 --remove）
    └── watchctl.py             Mac 侧工具：配 Wi-Fi、对时、串口监视、手动推状态
```

## 两个固件版本

| 目录 | 形态 | 工具链 |
|---|---|---|
| `firmware/` | **独立固件**：开机即表盘 + Claude 状态，两页循环滑动 | Arduino core 3.3.10，`./build.sh all` |
| `brookesia/` | **出厂桌面系统（ESP-Brookesia）+ ClaudeWatch App**：桌面上点 "Claude" 图标进入，同样的两页界面，底边上滑退出 | ESP-IDF 5.5，`brookesia/build.sh all` |

两者共用同一套 Mac 侧脚本和 hooks（HTTP 接口、串口命令完全一致）。Brookesia 版的服务在后台常驻（Wi‑Fi/HTTP/mDNS/NTP），
桌面状态栏的时钟也因此走 NTP。Brookesia 版界面是 LVGL 9 移植（`brookesia/components/brookesia_app_claude_watch/ui/cw_ui.cpp`），
网络/状态服务是纯 C 组件（`brookesia/components/claude_watch_service/`）。

### Brookesia 版构建说明

```bash
brookesia/build.sh all        # 首次会下载托管组件（BSP、LVGL 9.5、mdns），全量编译约 5 分钟
brookesia/build.sh monitor    # idf.py monitor（Ctrl+] 退出）；控制台走 USB Serial/JTAG
```
- ESP-IDF 装在 `~/esp/esp-idf`（v5.5.1），工具链在 `~/.espressif`；`export.sh` 在这台 Mac 上会 SIGABRT，`build.sh` 改用 `idf_tools.py export` 自己拼环境变量，并依赖 Homebrew 的 cmake/ninja。
- App 组件 `brookesia_app_claude_watch`（`WHOLE_ARCHIVE`，靠 `ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR` 注册，桌面自动出图标 "Claude"）；服务组件 `claude_watch_service` 在 `app_main` 里 `cw_service_start()` 常驻。
- 分区表 `partitions.csv`：nvs 0x9000（与 Arduino 版同偏移，所以 Wi‑Fi 凭据两边通用）、factory app 4MB。
- 切回独立固件：`./build.sh all`（Arduino）；切回桌面版：`brookesia/build.sh all`。

## 硬件

实测这块板是 **1.75C** 版本：ESP32-S3R8（8MB PSRAM）、**32MB Flash**、CO5300 QSPI AMOLED 466×466、
CST9217 触摸、AXP2101 电源、QMI8658 IMU、ES8311/ES7210 音频。**没有 PCF85063 RTC、没有 TCA9554、没有 SD 卡**
（1.75 标准版才有）。时间靠 NTP 或 USB 命令设置，ESP32 内部时钟在不断电时保持。

**引脚以 1.75C 为准**（`firmware/libraries/Mylibrary/pin_config.h` 已改为 1.75C 值）：LCD 复位 GPIO1、触摸复位 GPIO2、**I2S MCLK GPIO16**（1.75 标准版是 39/40/42）。
用错 MCLK 引脚时 ES8311 初始化仍会"成功"，但没有主时钟就完全无声——这是最初提示音不响的原因。

I2C 总线（SDA=15，SCL=14）实测设备：`0x18` ES8311、`0x34` AXP2101、`0x40` ES7210、`0x5A` CST9217、`0x6B` QMI8658。

## 快速开始

```bash
# 依赖（已装好）：brew install arduino-cli；arduino-cli core install esp32:esp32@3.3.10；pip3 install esptool pypdf
./build.sh all                         # 编译 + 烧录（自动找 /dev/cu.usbmodem*）
python3 host/watchctl.py wifi <SSID> <密码>   # 配 Wi-Fi（存 NVS，重刷固件也保留）
python3 host/watchctl.py time          # 没联网时先把 Mac 时间推过去
python3 host/watchctl.py monitor       # 看串口日志
python3 host/install_hooks.py          # 安装 Claude Code hooks（已安装）
```

联网后板子会以 `http://claude-watch.local/` 出现在局域网里，自动 NTP 对时（时区 `CST-8`，见 `config.h`）。

## 界面

**滑动是循环的**：往右滑，下一页从右侧滑入；往左滑，从左侧滑入，怎么滑都能切到另一页（页数可在 `ui.cpp` 的 `PAGES` 扩展）。
Claude 需要你时自动跳到状态页，空闲 90 秒后自动回到表盘。

**表盘**：纯黑底（AMOLED 省电），Montserrat 120px 时间，外圈 60 个刻度点走秒（走过的变 Claude 橙、当前秒高亮），下方日期，
顶部 Wi-Fi/电池图标，底部一行 Claude 迷你状态（小圆点 + 文字）。23:00–07:00 自动降低亮度。

**状态页**：大状态环 + 状态词 + 当前工具 + 已持续时间 + 一句说明 + 项目名/会话数。

| 状态 | 颜色 | 动效 | 触发 |
|---|---|---|---|
| IDLE | 绿 | 静止 | Stop / SessionStart / 空闲提醒 |
| WORKING | 橙 | 环转动、小点呼吸 | UserPromptSubmit / PreToolUse / PostToolUse |
| WAITING | 蓝 | 环脉冲，自动跳到此页 | PermissionRequest / 权限通知 / AskUserQuestion / ExitPlanMode |
| ERROR | 红 | 静止 | 手动推送 |
| OFFLINE | 灰 | 静止 | 10 分钟没收到消息 / 所有会话结束 |

多个 Claude Code 会话同时开着时，取优先级最高的（waiting > error > working > idle），并显示会话数。

**设置页**（第 3 页，不在滑动循环里）：侧键 **双击**弹出、**单击**回表盘（任何页面单击都回表盘）。BOOT 键和 PWR 键都可以（PWR 走 AXP2101 短按中断轮询）。
内容：亮度滑块、夜间自动调暗、音量滑块（松手试音）、提示音开关、Wi‑Fi/IP/音频/内存信息、重启。设置页内禁用滑动手势，滑块不会误切页。

**网页后台** `http://claude-watch.local/`（板子自己提供，无需安装）：Claude 连接状态、自动跳转/回退/离线超时、亮度、夜间调暗、
**表盘背景色 / 主题色**、秒刻度开关、12/24 小时制、提示音/音量、切换 Wi‑Fi、对时、重启。改动即时生效并存入 NVS。
接口：`GET /api/config`（settings + status + info）、`POST /api/config {部分字段}`、`POST /api/wifi {ssid,pass}`、`POST /api/beep`、`POST /api/reboot`；旧的 `/status`、`/time` 不变，`/help` 是纯文本说明。

**图片：表盘背景 + 相册页**（滑动循环：表盘 → Claude → 相册 → 表盘）
- 网页后台「图片」卡片上传（多选）：浏览器把图片居中裁切成 466×466、编码为 RGB565 大端原始数据（434,312 字节/张），`multipart` POST 到 `/api/img`
- 存在板子的 FAT 分区（`ffat`，约 9.9MB，首次启动自动格式化），目录 `/img`，约放 22 张；上传一张约 5 秒
- 表盘背景：关闭 / 固定一张 / 每 N 分钟轮换；「背景压暗」在加载时按像素衰减，保证时间可读
- 相册页：点屏切下一张（滑动不会误触发），可设自动翻页秒数；图片数据放 PSRAM（每张 434KB，两份：壁纸 + 相册当前图）
- 接口：`GET /api/img` 列表、`POST /api/img`（multipart，文件名即图片名 `[A-Za-z0-9_-]{1,24}.bin`）、`DELETE /api/img?name=`、`GET /api/img/raw?name=`（网页缩略图用）

**Claude 回复内容**：hook 在 `Stop` 事件读取 `transcript_path`（JSONL）里最后一段 assistant 文本（去掉 `**`/反引号，截 240 字），作为 `output` 字段推送；
板子在 Claude 页中下部用 3 行显示（超出以 … 截断）。字体 `font_cjk_16.c`：Noto Sans SC 16px 4bpp，ASCII + GB2312 一级 3755 汉字 + 常用标点/全角（约 480KB Flash）。
JSON 解析已支持 UTF‑8 直传和 `\uXXXX`（含代理对）解码。网页后台「Claude 连接」里也显示最新输出。
多会话时按优先级显示（waiting > error > working > idle），所以别的会话在工作时会盖住空闲会话的回复。

**语音对话页**（滑动循环第 4 页：表盘 → Claude → 相册 → 语音）：按住圆形麦克风按钮说话、松开发送。
```
板子 ES7210 双麦录音(16k/16bit, 最长 15s, 存 PSRAM) ──POST /voice──▶ Mac host/voice_server.py
   whisper-cli(ggml-base) → claude -p --model haiku --continue → say -v "Eddy (中文（中国大陆）)"
板子 ◀── JSON{text, reply, audio} ── 再 GET /audio/<id> 流式播放(ES8311)
```
- Mac 端：`brew install whisper-cpp`，模型在 `~/.cache/whisper/ggml-base.bin`（148MB）；启动 `python3 host/voice_server.py`（端口 8765）。
  服务器每 60 秒向板子 `POST /api/voice_server {"url":...}` 自报地址，板子存 NVS，无需手工配置；识别到 VPN 的 198.18.x 假地址会跳过。
- 一轮耗时约 14 秒（whisper 0.5 s + claude ~13 s）；`--claude-model sonnet` 更聪明但更慢；`--model ~/.cache/whisper/ggml-small.bin` 识别更准。
- 语音对话在独立会话目录 `~/.claude/claude-watch/voice/session` 里 `--continue`，不会串到你的项目会话。
- 板子端：`voice.cpp` 里的 ES7210 初始化序列移植自 esp_codec_dev（从属模式、I2S 16bit、MIC1+MIC2、30dB），录音取左声道（MIC1），去直流并做简单归一化。

**内置壁纸**：`python3 host/default_images.py` 用纯 Python 程序化生成 6 张（日照金山、落日海面、黄昏群山、星空、银河、极光；含抖动避免 RGB565 色带）并上传、设为 30 分钟轮换；`--out DIR` 只导出文件。

**提示音**：ES8311 + NS4150B 功放，Claude 进入 WAITING 响两声、ERROR 三声（需要在 MX1.25 焊盘接喇叭；未接时只是静默）。

**WORKING 页词汇**：没有具体工具时轮播 Claude Code 终端那套词（Bootstrapping、Sketching、Cogitating、Percolating……），每 3.5 秒换一个。

**自动熄屏 / 自动深睡**（设置页两个滑块，网页后台两个下拉）：无触摸/按键超过 N 秒 → 熄屏（Wi‑Fi 和状态接收照常，触屏/按键点亮，Claude 进入 WAITING/ERROR 也会自动点亮）；
再超过 M 秒 → 深度睡眠（时钟保持，状态推送暂停，BOOT/触屏唤醒）。两者默认「从不」。
熄屏 = 低功耗待机：面板 Sleep‑In、Wi‑Fi 调制解调器休眠（HTTP 仍可达，延迟约 100 ms）、CPU 降到 80 MHz、LVGL 暂停渲染（触摸中断直接唤醒）。
电池续航建议：用电池时设「熄屏 1 分钟 + 深睡 30 分钟」；接 USB 常亮时两者设「从不」。板载电池较小，满速常亮大约只能撑几个小时。

**时间保持（无 RTC 芯片的对策）**
- **长按侧键 ≥1.5 s = 深度睡眠**（屏幕/Wi‑Fi 关，ESP32 内部时钟继续走，电池能撑很久）；**按 BOOT 或触屏唤醒**，时间不丢。电源芯片的硬件断电改为需按住 10 秒。
- 真断电/重刷：每 5 分钟把时间写入 NVS，开机先恢复并显示 `≈ 未校准`，校准后消失。
- 校准来源：NTP；**Mac hook 每次推送附带 `epoch`**（同一局域网即可，不依赖外网）；USB `time`；网页「同步时间」。
- USB 串口残留：hook 走串口兜底时若被打断，板子缓冲里会留半截 JSON，下一条命令会被吞掉——`watchctl.py` 现在发命令前先发一个空行冲掉。

**OTA**：`./build.sh ota`（`HOST=192.168.31.220 ./build.sh ota` 指定 IP）把 `firmware/build/ClaudeWatch.ino.bin` 通过 `POST /api/ota` 推给板子，双 app 槽（app0/app1 各 3MB），刷完自动重启。首次需要 USB 刷入带 OTA 的版本。

## 数据流

```
Claude Code ──hook 事件(stdin JSON)──▶ host/claude_watch_hook.py
   ├─ 更新 ~/.claude/claude-watch/state.json（每会话状态，3 小时过期）
   ├─ 聚合成 {"state","tool","project","msg","sessions"}
   └─ fork 子进程发送（hook 本身立刻返回，不拖慢 Claude）
        ├─ HTTP POST http://claude-watch.local/status   （IP 缓存 10 分钟）
        └─ 失败则 USB 串口写一行 JSON（DTR=1/RTS=0 打开，不会复位板子；CLAUDE_WATCH_SERIAL=0 可关）
```

板子 HTTP 接口：`GET /` 帮助、`GET /status` 当前状态、`POST /status` JSON、`POST /time?epoch=N`。
串口命令：`wifi <ssid> <pass>`、`time <epoch>`、`{json}`、`status`、`info`、`i2cscan`、`bright <0-255>`、`reboot`。

手动测试：
```bash
python3 host/watchctl.py send waiting --tool Bash --msg "approve: rm -rf build"
python3 host/watchctl.py send idle
```

## 编译参数

`esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc`（微雪验证过的 16MB 布局，在 32MB 芯片上照常工作）。
`CDCOnBoot=cdc` 必开，否则 `Serial` 走 UART0，USB 上看不到输出。`lv_conf.h` 已调：内存池 64KB、只保留 14–28px 内置字体、关闭 demo。

## 渲染性能（实测，`perf` 串口命令做全屏压测）

| 阶段 | 表盘整帧 | 说明 |
|---|---|---|
| 初版：40MHz QSPI + `writePixels` 同步 + lv_arc 秒针 | 57ms / 17fps | 传输 27ms 与渲染 30ms 串行 |
| 80MHz、自建 SPI 设备异步 DMA 双缓冲、`LV_COLOR_16_SWAP=1` | 41ms / 24fps | 传输 13ms 并与渲染并行 |
| 秒针弧 → 60 刻度点，去掉半透明底盘，状态环 → alpha 图片着色 | **27ms / 37fps** | 空页面下限 15ms（DMA 极限） |

结论：**LVGL 8 的 `lv_arc` / 大圆角半透明对象在这块 MCU 上每个要 15–20ms**（对整个包围盒做逐像素遮罩），能用图片或小对象替代就替代。
`img_ring.c` 是用 Python 生成的 392×392 抗锯齿 alpha 环（`ui.cpp` 顶部有说明），`lv_obj_set_style_img_recolor` 着色。
像素数据不走 Arduino_GFX 的 `writePixels`，而是在同一 SPI 总线上加了第二个设备句柄（80MHz、队列 + 完成中断），见 `ClaudeWatch.ino` 的 `pix_*`。

调试命令：`perf`（3 秒全屏压测出 fps）、`dbg ticks|time|date|top|mini|ring|ringimg|state`（隐藏元素看耗时）、`page 0|1`（切页）。

## 已知注意点

- 打开 USB 串口时必须 DTR=1、RTS=0（`watchctl.py`/hook 已处理）。其他组合会触发 ESP32-S3 USB-JTAG 复位，甚至进下载模式（表现为串口无响应）。
- **烧录完成后串口经常"装死"**（esptool 复位后 USB-JTAG 状态不对），屏幕正常但串口没回应：`python3 host/watchctl.py reset` 发一个复位脉冲即可，不用拔线。
- 界面为英文（LVGL 内置字体无中文）；要中文需用 `lv_font_conv` 生成含 CJK 字形的字体。
- 触摸坐标**必须** `TOUCH_MIRROR_X/Y = true`（CST9217 原始坐标相对屏幕旋转 180°，微雪例程和 BSP 都做了镜像）。不开镜像时滑动看似正常，但点击/拖动会落到对角位置——排查方法：串口 `touchlog` 对比按压坐标和控件位置。
- `firmware/build/` 是编译产物，可删。

## 许可

本项目代码采用 [MIT License](LICENSE)。仓库内随附的第三方库保留各自许可：`firmware/libraries/`（微雪例程附带的 Arduino_GFX、LVGL 8.4、SensorLib、XPowersLib 等，MIT/BSD）、
`brookesia/components/brookesia_core`（Espressif ESP-Brookesia，Apache-2.0）、`firmware/ClaudeWatch/es8311*.c/h` 与 `es7210_reg.h`（Espressif，Apache-2.0）、
中文字体 `font_cjk_16.c` 由 Noto Sans SC（SIL OFL 1.1）生成。
