# SDUI 物理交互终端系统 (ESP32-S3) 开发指南

## 一、 项目背景与总体目标

本项目构建了一个基于“瘦客户端（Thin Client）”架构的分布式物联网交互系统。系统将 ESP32-S3 微控制器抽象为纯粹的物理 I/O 节点，负责环境感知、流媒体采集与 UI 渲染。

所有业务逻辑、状态同步、媒体解析及 AI 交互均由远端 Python 服务器接管。终端与服务器之间通过基于主题路由的 **SDUI (Server-Driven UI) 交互协议**及全双工 WebSocket 通道进行高速通信。

---

## 二、 系统架构与工程结构

系统在逻辑与物理层面划分为两大核心部分：

1. **物理终端层 (ESP32-S3)**：依托 ESP-IDF (v5.5) 框架，实现硬件总线驱动、LVGL 渲染引擎、音频双工系统及消息总线路由。
2. **云端业务层 (Python Server)**：异步非阻塞网关，负责 STT (Speech-To-Text) 解析、音频持久化调试、UI 状态维护及媒体切片下发。

### 目录结构

系统采用模块化组件设计，通过 CMake 进行依赖管理：

```text
02_lvgl_demo_v9/
├── components/
│   ├── sdui_bus/           # 核心枢纽：基于 Pub/Sub 模式的消息路由总线 (上行 + 本地事件)
│   ├── sdui_parser/        # 布局引擎：JSON → LVGL 递归渲染、Action URI 事件绑定
│   ├── websocket_manager/  # 通信底座：负责链路维护、断线重连与长载荷分块拼接
│   ├── audio_manager/      # 媒体引擎：音频驱动 (ES8311/ES7210)、电源管理与 Base64 编解码
│   ├── imu_manager/        # 空间感知：QMI8658/QMA7981 传感器驱动及姿态算法
│   ├── wifi_manager/       # 网络设施：Wi-Fi STA 状态管理
│   └── esp32_s3_touch_amoled_1_75c/ # BSP：屏幕及触摸驱动底层支持
├── main/
│   └── main.c              # 业务入口：初始化调度、动态 SDUI 布局入口与息屏管理
├── sdkconfig.defaults      # 系统核心配置（内存分布、频率、外设宏等）
└── CMakeLists.txt          
```

---

## 三、 SDUI 交互协议 (Topic-Based)

系统通信统一采用基于“主题-载荷（Topic-Payload）”结构的 JSON 协议，实现业务逻辑的高度解耦。

### 3.1 上行链路 (终端 -> 云端)

| 主题 (Topic) | 载荷示例 (Payload) | 触发场景与说明 |
| --- | --- | --- |
| `ui/click` | `{"id": "btn_1"}` | 屏幕原子按钮触控事件（默认 Action URI）。 |
| `audio/record` | `{"state": "stream", "data": "..."}` | 录音开启/停止信号及 PCM 转 Base64 音频数据流。 |
| `motion` | `{"type": "shake", "magnitude": 15.3}` | IMU 识别到的物理姿态变化（如摇一摇）。 |

### 3.2 下行链路 (云端 -> 终端)

| 主题 (Topic) | 载荷示例 (Payload) | 执行动作与说明 |
| --- | --- | --- |
| `ui/layout` | `{"flex":"column", "children":[...]}` | **全量布局渲染**：清除现有 UI 并根据 JSON 树递归构建新界面。 |
| `ui/update` | `{"id":"label_1","text":"Count: 5"}` | **增量属性更新**：按 ID 查找组件并更新其属性。 |
| `audio/play` | `"UklG..."` | 终端接收 Base64 音频切片，即时解码并推入 I2S 扬声器。 |

### 3.3 容器化布局 JSON 协议 (ui/layout)

Server 下发的 `ui/layout` 载荷为树状 JSON 结构，支持以下原子组件与属性：

| 属性 | 类型 | 说明 | 示例 |
| --- | --- | --- | --- |
| `type` | string | 组件类型: `container` / `label` / `button` | `"type": "button"` |
| `id` | string | 组件唯一 ID，用于 `ui/update` 增量更新 | `"id": "btn_rec"` |
| `text` | string | 文本内容 (label/button) | `"text": "Hold to Talk"` |
| `flex` | string | Flex 布局方向: `row` / `column` / `row_wrap` / `column_wrap` | `"flex": "column"` |
| `justify` | string | 主轴对齐: `start` / `center` / `end` / `space_between` | `"justify": "center"` |
| `align_items` | string | 交叉轴对齐 | `"align_items": "center"` |
| `w` / `h` | number/string | 尺寸：像素 `120`、百分比 `"50%"`、`"full"` / `"content"` | `"w": "80%"` |
| `align` | string | 绝对定位: `center` / `top_mid` / `bottom_mid` 等 | `"align": "center"` |
| `bg_color` | string | 背景色 (Hex) | `"bg_color": "#2ecc71"` |
| `text_color` | string | 文字颜色 (Hex) | `"text_color": "#FFFFFF"` |
| `font_size` | number | 字体大小，映射到 Montserrat 14/16/20/24/28 | `"font_size": 24` |
| `gap` | number | Flex 子元素间距 | `"gap": 10` |
| `pad` / `radius` | number | 内边距 / 圆角 | `"pad": 8` |
| `hidden` | boolean | 隐藏/显示 | `"hidden": true` |
| `children` | array | 子组件数组 | `"children": [...]` |

### 3.4 Action URI 事件绑定协议

每个交互组件可通过 `on_click` / `on_press` / `on_release` 字段绑定动作：

| URI 前缀 | 路由方式 | 示例 |
| --- | --- | --- |
| `local://` | 本地总线分发，不经 WebSocket | `"on_press": "local://audio/cmd/record_start"` |
| `server://` | 通过 WebSocket 上报云端 | `"on_click": "server://ui/action"` |
| 无前缀 | 默认上报 `ui/click` | `"on_click": ""` (发送 `{"id":"xxx"}`) |

**示例：Server 下发“按住说话”按钮**
```json
{
  "topic": "ui/layout",
  "payload": {
    "flex": "column", "justify": "center", "align_items": "center", "gap": 15,
    "children": [
      {"type": "label", "id": "status", "text": "Ready", "font_size": 20},
      {
        "type": "button", "id": "btn_rec", "text": "Hold to Talk",
        "w": 160, "h": 50, "bg_color": "#2ecc71",
        "on_press": "local://audio/cmd/record_start",
        "on_release": "local://audio/cmd/record_stop"
      }
    ]
  }
}
```
终端渲染按钮后，按下时自动触发本地 `audio_manager` 开始录音，松手停止，无需经过云端。

---

## 四、 核心组件机制

1. **消息枢纽 (sdui_bus)**：采用订阅/发布（Pub/Sub）模式，实现各业务解耦。支持三种路由方式：下行 (`route_down`)、上行 (`publish_up`)、本地 (`publish_local`)。
2. **布局引擎 (sdui_parser)**：递归解析 JSON UI 树并映射为 LVGL 对象。支持 Flex 布局、Action URI 事件绑定、圆屏安全边距(40px)。
3. **通信信使 (websocket_manager)**：支持断线被动重连。在弱网断线时主动拦截上行发布，避免数据堆积导致 OOM。
4. **音频全双工 (audio_manager)**：支持双通道麦克风读取与基于 I2S 的 DAC 音频播放。通过总线事件订阅驱动（`audio/cmd/*`）。
5. **空间感知 (imu_manager)**：通过 `sdui_bus` 上行发布姿态事件（如 `motion` 主题），与 WebSocket 完全解耦。
---

## 五、 🚧 ESP32-S3 内部 SRAM 内存治理手册 (重要)

ESP32-S3 拥有 ~512KB 内部 SRAM 和 8MB PSRAM，但**两者并不等价**：SPI DMA、FreeRTOS 任务栈等硬件级操作严格依赖内部 SRAM 的**连续块**。WiFi 协议栈运行后会将内部 SRAM 碎片化，使之前看似充裕的空间无法满足大块连续分配。

本章记录了 `SPI DMA Failed to allocate priv TX buffer` + `ESP_ERR_NO_MEM` 的完整排查过程，涉及三个相互关联的子问题。

### 问题现象

```
E spi_master: setup_dma_priv_buffer(1206): Failed to allocate priv TX buffer
E lcd_panel.io.spi: panel_io_spi_tx_color(395): spi transmit (queue) color failed
E co5300_spi: panel_co5300_draw_bitmap(292): send color data failed
E esp_lvgl:bridge_v9: Draw bitmap failed: ESP_ERR_NO_MEM
E AUDIO_MANAGER: Failed to allocate internal memory! System halted.
```

### 根因模型

```
┌─────────────────── 内部 SRAM (~512KB) ──────────────────┐
│  WiFi 协议栈 (60-100KB, 碎片化)                          │
│  I2S DMA 缓冲 (~10KB)                                   │
│  FreeRTOS 系统栈 + 各任务栈                               │
│  ★ LVGL Draw Buffer (原 PSRAM → 改为内部 9.3KB)          │
│  ★ SPI DMA bounce buffer (每次 flush 动态申请,大小=传输块) │
│  ★ 音频录音 buffer (原强制 INTERNAL → 改走 DEFAULT)       │
│  …剩余碎片…                                              │
└──────────────────────────────────────────────────────────┘
```

核心矛盾：多个组件竞争有限且碎片化的内部 SRAM 连续块。

---

### 修复 1 — 消除 SPI DMA Bounce Buffer (`esp32_s3_touch_amoled_1_75c.c`)

**原理**：LVGL 渲染缓冲在 PSRAM 时，SPI DMA 无法直接访问，底层驱动会在**每次 flush** 时从内部 SRAM 动态 `malloc` 一块与传输数据等大的 bounce buffer 做跳板。WiFi 碎片化后找不到连续块即 OOM。

**解法**：让 LVGL 缓冲直接常驻内部 SRAM（一次性安排，不再每帧动态申请）。

```c
// bsp_display_lcd_init() 中的关键配置
#define LVGL_TRANSFER_BUF_LINES  10  // 每块 466×10×2 ≈ 9.3KB

// SPI bus 参数
.max_transfer_sz = BSP_LCD_H_RES * LVGL_TRANSFER_BUF_LINES * BSP_LCD_BITS_PER_PIXEL / 8,
io_config.trans_queue_depth = 4;  // 降低队列深度

// LVGL adapter 配置
.buffer_height = LVGL_TRANSFER_BUF_LINES,
.use_psram     = false,           // ← 关键：缓冲在内部 SRAM，SPI 直接 DMA 访问
.require_double_buffer = false,   // 单缓冲，节省内部 SRAM
```

> **代价**：刷新率略降（每帧需 ~47 次 SPI 传输）。对 UI 文本/按钮类场景影响微乎其微。

---

### 修复 2 — 音频缓冲释放内部 SRAM (`audio_manager.c`)

**原理**：`audio_record_task` 和 `audio_play_callback` 中的 PCM / Base64 / JSON 缓冲原本使用 `MALLOC_CAP_INTERNAL` 强制分配在内部 SRAM（总计 ~4.5KB），与 SPI DMA 争抢资源。

**解法**：这些是应用层编解码缓冲，不需要 DMA 直接访问，改用 `MALLOC_CAP_DEFAULT`，系统会自动分配到最合适的区域（PSRAM 优先）。

```c
// 修改前（争抢内部 SRAM）
heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

// 修改后（放宽到默认内存池，可分配至 PSRAM）
heap_caps_malloc(size, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
```

---

### 修复 3 — 音频任务栈迁移至 PSRAM (`audio_manager.c` + `sdkconfig.defaults`)

**原理**：修复 1 占用了 9.3KB 内部 SRAM 用于 LVGL 缓冲后，`xTaskCreatePinnedToCore` 为录音任务分配 4KB 栈时因内部 SRAM 不足而**静默失败**（原代码未检查返回值），导致录音任务从未启动。

**解法**：使用 ESP-IDF v5.4+ 的 `xTaskCreatePinnedToCoreWithCaps` API，将任务栈分配到 PSRAM。

```c
#include "freertos/idf_additions.h"

BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
    audio_record_task, "audio_record_task",
    4096, NULL, 2, NULL, 1,
    MALLOC_CAP_SPIRAM);        // ← 任务栈分配到 PSRAM
if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create audio_record_task!");
}
```

同时在 `sdkconfig.defaults` 中启用：
```
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

---

### 防抢占的初始化顺序 (`main.c`)

```
1. bsp_display_start()    ← 最先：SRAM 充裕时锚定 SPI DMA + LVGL 缓冲
2. LVGL UI 构建            ← 紧随：显示就绪后立即渲染
3. wifi_init_sta()         ← 后置：此后 SRAM 被碎片化
4. websocket / imu / audio ← 最后：大内存负载组件走 PSRAM
```

### 排查检查清单

遇到类似 OOM 时，按以下顺序逐项排查：

| # | 检查项 | 命令/方法 |
|---|--------|----------|
| 1 | 启动时内部 SRAM 余量 | `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` |
| 2 | 最大连续可用块 | `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` |
| 3 | LVGL buffer 是否在 PSRAM | 检查 `use_psram` 设置 |
| 4 | 音频/WebSocket buffer 是否强制 INTERNAL | grep `MALLOC_CAP_INTERNAL` |
| 5 | 任务栈是否创建成功 | 检查 `xTaskCreate` 返回值 |

---

## 六、 快速上手与编译烧录

由于本项目重构过底层 `sdkconfig.defaults` 内存配比映射，**首次编译/清理后重新编译时，必须删除原生成的配置文件**。

### 命令行编译

```bash
# 进入工程目录
cd 02_lvgl_demo_v9

# 1. 删除历史配置文件（强制重新应用 defaults）
rm sdkconfig (Linux/macOS) 或 del sdkconfig (Windows)
# 或者执行全量清理
idf.py fullclean

# 2. 编译并烧录监控
idf.py build flash monitor
```

### VS Code 插件开发

1. 点击底部状态栏的 `ESP-IDF: Full Clean` 垃圾桶图标。
2. 点击齿轮图标 `ESP-IDF: SDK Configuration editor (menuconfig)` 点一下 Save 生成新的。
3. 点击火苗图标 `ESP-IDF: Build, Flash and Monitor` 进行一键构建烧录。

> **提示**：烧录完成启动后，若遇到屏幕无法正常亮起或花屏，请检查 SPI 引脚定义及上述 `esp32_s3_touch_amoled_1_75c.c` 驱动组件配置是否被意外回退。

---

## 七、 云端业务层 (Python Server) MVP 说明

配套的 `server.py` 是用于调试与验证 SDUI 原型的 MVP (Minimum Viable Product) 设计实现，旨在演示端云交互的基本链路，尚未进行高可用与工程化封装。

### 核心功能 (MVP 阶段)
1. **连接即下发**：终端 WebSocket 连接建立后，主动下发预设的容器化首屏 `ui/layout`（包含布局信息、组件状态）。
2. **主题事件路由**：
   - 监听并处理来自终端的 `ui/click` 上行事件，例如处理按钮的计数累加，并返回 `ui/update` 更新组件展示。
   - 监听并处理 `audio/record` 的音频流，执行本地存储 `debug_recv.wav` 以及基础的 Speech-To-Text (STT) 识别。
   - 处理终端传感器事件如 `motion` (摇一摇)，触发 UI 或逻辑的变动。
3. **Debug 调试控制台**：在命令行提供了一套极简的手动操作台，方便开发者在联机运行时下发 `layout` (重置布局) / `update` (更新组件) / `raw` (原始指令) 以辅助硬件侧界面的调试。

### 后续演进方向
目前的 Server 仅用于单实例验证，要支持复杂的 Agent 业务需要后续扩展：
- 引入 FastAPI / 异步框架并结合多实例 Session 管理（而非现有的全局单一连接变量）。
- 将 STT 模块及音频下发（TTS）对接到真正的大语言模型（LLM）流式接口。
- 分离状态同步（加入 version/nonce 控制机制）及资源下发逻辑。
