# SDUI 物理交互终端系统说明书 (Snapshot)

## 一、 项目背景与总体目标

本项目构建了一个基于“瘦客户端（Thin Client）”架构的分布式物联网交互系统。系统将 ESP32-S3 微控制器抽象为纯粹的物理 I/O 节点，负责环境感知、流媒体采集与 UI 渲染。

所有业务逻辑、状态同步、媒体解析及 AI 交互均由远端 Python 服务器接管。终端与服务器之间通过基于主题路由的 **SDUI (Server-Driven UI) 交互协议**及全双工 WebSocket 通道进行通信。

## 二、 系统总体架构

系统在逻辑与物理层面划分为两大核心部分：

1. **物理终端层 (ESP32-S3)**：依托 ESP-IDF 框架，实现硬件总线驱动、LVGL 渲染引擎、音频双工系统及消息总线路由。
2. **云端业务层 (Python Server)**：异步非阻塞网关，负责 STT (Speech-To-Text) 解析、音频持久化调试、UI 状态维护及媒体切片下发。

### 2.1 项目工程结构

物理终端采用模块化组件设计，各组件通过 CMake 构建系统进行依赖管理：

```text
02_lvgl_demo_v9/
├── components/
│   ├── sdui_bus/           # 核心枢纽：基于订阅/发布模式的消息路由总线
│   ├── websocket_manager/  # 通信底座：负责链路维护、断线重连与长载荷拼接
│   ├── audio_manager/      # 媒体引擎：ES8311 驱动、电源管理与 Base64 流式编解码
│   ├── imu_manager/        # 空间感知：QMI8658/QMA7981 传感器驱动
│   └── wifi_manager/       # 网络基础设施：Wi-Fi STA 模式状态机
├── main/
│   └── main.c              # 业务入口：UI 构建、自动息屏逻辑与总线初始化
├── server.py               # 云端网关：Python 异步服务与 STT 解析引擎
└── CMakeLists.txt          

```

## 三、 SDUI 交互协议 (Topic-Based)

系统通信统一采用基于“主题-载荷（Topic-Payload）”结构的 JSON 协议，实现业务逻辑的高度解耦。

### 3.1 上行链路 (终端 -> 云端)

| 主题 (Topic) | 载荷示例 (Payload) | 触发场景与说明 |
| --- | --- | --- |
| `ui/click` | `{"id": "btn_1"}` | 屏幕原子按钮触控事件。 |
| `audio/record` | `{"state": "stream", "data": "..."}` | 录音开启/停止信号及 PCM 转 Base64 音频切片。 |
| `motion` | `{"type": "shake"}` | IMU 识别到的物理姿态变化。 |

### 3.2 下行链路 (云端 -> 终端)

| 主题 (Topic) | 载荷示例 (Payload) | 执行动作与说明 |
| --- | --- | --- |
| `ui/update` | `"Count: 5"` | 触发终端渲染引擎更新特定文本标签。 |
| `audio/play` | `"UklG..."` | 终端接收 Base64 音频切片，即时解码并推入 I2S 扬声器。 |

## 四、 核心组件实现细节

### 4.1 消息枢纽 (sdui_bus)

* **路由机制**：采用订阅/发布（Pub/Sub）模式。各业务模块通过 `sdui_bus_subscribe` 注册关心的 Topic。
* **延迟解析策略**：总线仅对 JSON 外层进行浅解析以提取 Topic，内部 Payload 以纯文本形式传递给订阅者，最大限度降低网络任务的内存压力。

### 4.2 通信信使 (websocket_manager)

* **鲁棒性设计**：支持 `reconnect_timeout_ms` 自动重连。在断线期间，发送接口采取非阻塞拦截，直接丢弃上行数据以保护系统不卡顿。
* **帧拼接引擎**：利用 `payload_offset` 机制对 TCP 拆包后的长载荷进行动态内存拼接，确保大体积 Base64 数据帧的完整性。

### 4.3 音频全双工子系统 (audio_manager)

* **硬件驱动**：驱动 ES8311 芯片（22050Hz, 16-bit, 单声道），并通过 GPIO 46 进行硬件电源使能控制。
* **资源调度**：采集任务锚定于 Core 1 运行。核心缓冲区（PCM/Base64）强制通过 `MALLOC_CAP_INTERNAL` 分配在内部极速 SRAM，避免与屏幕刷新总线产生冲突。
* **本地监控**：集成电平计算逻辑，通过串口实时输出 `Mic Level` 以供硬件诊断。

### 4.4 终端显示与电源管理 (main.c)

* **自动息屏机制**：利用 LVGL 的 `lv_disp_get_inactive_time` 监测用户操作。当空闲超过 30 秒时，自动通过 PWM 调低或关闭背光；检测到触控或云端推送（`lv_disp_trig_activity`）时自动唤醒。

## 五、 云端业务逻辑 (server.py)

* **媒体流水线**：接收上行 PCM 切片并拼接，在录音结束时自动注入 RIFF-WAVE 文件头并保存为本地 `debug_recv.wav` 供调试。
* **STT 识别**：调用 `SpeechRecognition` 引擎（Google 引擎适配），集成环境噪音校准（`adjust_for_ambient_noise`）与电平阈值过滤。
* **下行流控**：针对终端硬件接收窗口，采取 1.0KB 小切片配合 `asyncio.sleep(0.02)` 延时的平滑下发策略，防止终端 OOM。

---

## 六、 终端资源调度规范

### 6.1 内存分布策略

* **内部 SRAM**：存放音频采样缓冲（1024 字节）、Base64 运算区及 DMA 描述符。
* **外部 PSRAM**：存放 LVGL 全屏双缓冲区、WebSocket 拼接缓冲区。

### 6.2 任务优先级矩阵

| 任务名称 | 优先级 | 核心 (Core) | 职责 |
| --- | --- | --- | --- |
| `lv_timer_handler` | 5 | Core 0 | UI 渲染与触控扫描。 |
| `ws_client_task` | 4 | Core 1 | 网络通信与帧拼接。 |
| `audio_record_task` | 2 | Core 1 | 音频采集与编码。 |

