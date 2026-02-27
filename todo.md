# SDUI 物理交互终端系统 演进功能设计规范

## 一、 增强型 UI 表现力原子组件

为支撑如“音乐播放器”等具有高表现力要求的业务场景，系统在 `sdui_parser` 布局引擎中扩展了以下原子组件与属性。所有组件的状态切换（如页面跳转、歌词同步）均由远端服务器通过全量 `ui/layout` 或增量 `ui/update` 驱动，终端保持无状态渲染。

### 1.1 新增原子组件库

| 组件类型 (`type`) | 关键属性 | 功能说明 |
| --- | --- | --- |
| `image` | `src` (Base64), `w`, `h`, `radius` | **流式图像组件**：支持服务器下发裁剪后的 RGB565 原始数据或 Base64 编码图像，用于展示封面或图标。 |
| `bar` | `value` (0-100), `bg_color`, `indic_color` | **进度指示组件**：用于展示音乐播放进度、下载进度或传感器量程。 |
| `slider` | `value`, `min`, `max`, `on_change` | **滑动控制组件**：支持音量调节、亮度调节，通过 Action URI 上报数值。 |

### 1.2 容器属性扩展 (Scroll & Transition)

* **滚动容器 (`scrollable`)**：`container` 组件支持 `scrollable: true` 属性。当子元素超出容器边界时，终端自动启用 LVGL 原生触摸滚动逻辑，无需服务器干预滚动动画。
* **全局过渡 (`transition`)**：在解析新的 `ui/layout` 载荷时，系统默认应用淡入淡出（Fade）或推屏（Slide）动画，以平滑覆盖旧有 UI 树，实现视觉上的“页面跳转”。

---

## 二、 生产级配网初始化机制 (Provisioning)

系统引入“双态启动”模型，解决终端在无网络凭证情况下的初始化与交互问题。

### 2.1 双态启动逻辑流转

系统在初始化阶段通过 NVS (Non-Volatile Storage) 状态机决定运行模式：

1. **引导探查**：上电后检查 Flash 特定分区中是否存在已加密存储的 SSID 与 Password。
2. **本地配网模式 (Local State)**：
* 若无凭证，系统启动 `Provisioning` 任务。
* **本地渲染**：渲染一段固化在固件中的极简 JSON 布局，显示配网二维码与设备 ID。
* **BLE 传输**：开启蓝牙低功耗 (BLE) 广播，建立安全通道接收手机端下发的 WiFi 凭证。


3. **SDUI 托管模式 (Cloud State)**：
* 若凭证有效，连接 WiFi 并建立 WebSocket 全双工通道。
* 从服务器拉取 `ui/layout` 首屏，将控制权完全移交云端。



### 2.2 配网协议与交互规范

| 步骤 | 交互媒介 | 动作描述 |
| --- | --- | --- |
| **1. 广播** | BLE (Protocomm) | 设备广播名称 `SDUI-Device-XXXX`，等待手机连接。 |
| **2. 传输** | Encrypted BLE | 手机通过微信小程序或 App 将 WiFi SSID/PWD 加密传输至设备。 |
| **3. 校验** | WiFi Station | 设备尝试连接目标 WiFi。 |
| **4. 固化** | NVS Flash | 连接成功后，将凭证写入 NVS 分区。 |

#### SoftAP + 手机端 Web 配置（推荐作为替代）
这是目前很多无键盘物联网设备（如打印机、智能插座）的做法：

终端显示一个简单的页面：“请连接 WiFi：SDUI-Setup，密码：12345678”。

用户用手机连接该热点后，手机会自动弹出（或扫码进入）一个配置网页。

手机浏览器里有完整的键盘，输入 SSID 和密码后发给 ESP32。

优点：终端不需要内置复杂的键盘逻辑，只需要跑一个极简的 Web Server。

---

## 三、 内存生命周期治理优化

针对 ESP32-S3 内部 SRAM 极其有限的特性，系统针对配网过程实施“阅后即焚”的内存管理策略。

### 3.1 配网任务栈回收机制

* **动态卸载**：一旦 WiFi 连接成功并校验通过，系统立即调用 `esp_bluedroid_disable()` 和 `esp_bt_controller_disable()` 彻底关闭蓝牙协议栈。
* **内存重归一化**：为彻底消除蓝牙协议栈运行后的内存碎片，系统在成功保存 WiFi 凭证后，主动触发 `esp_restart()` 软重启。
* **启动锚定**：重启后的系统不加载蓝牙驱动，将释放出的所有 SRAM 资源全量分配给 SPI DMA、I2S 音频缓冲及 WebSocket 长连接任务。

### 3.2 资源占用清单 (配网态 vs 托管态)

| 资源项 | 本地配网态 (BLE 开启) | 云端托管态 (SDUI 运行) |
| --- | --- | --- |
| **蓝牙协议栈** | 约 60KB - 100KB (Active) | 0KB (Disabled) |
| **LVGL Buffer** | 9.3KB (Internal SRAM) | 9.3KB (Internal SRAM) |
| **Audio Buffer** | 0KB (Idle) | 4.5KB+ (Active) |
| **WebSocket** | 0KB (Idle) | 32KB+ (Active/Buffer) |

