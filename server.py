import asyncio
import websockets
import json
import logging
import base64
import wave
import io
import os
import speech_recognition as sr

# ============================================================
#  SDUI Gateway Server — 支持容器化布局协议 & Action URI 事件
# ============================================================
# 日志配置：DEBUG 级别便于调试协议交互
logging.basicConfig(
    level=logging.DEBUG,
    format='%(asctime)s [%(levelname)s] %(message)s'
)
# 降低 websockets 库自身的日志噪音
logging.getLogger("websockets").setLevel(logging.WARNING)

# ---- 全局状态 ----
global_count = 0
audio_buffer = bytearray()
recognizer = sr.Recognizer()

# ---- 多终端设备注册表 ----
# key: device_id (MAC 地址字符串)
# value: { "ws": websocket对象, "addr": 连接地址, "telemetry": 最新遥测数据字典, "connected_at": 时间戳 }
import time
devices: dict = {}


# ============================================================
#  首屏布局定义 (Server 驱动 UI - 高表现力增强版)
# ============================================================
def build_home_layout():
    """
    构建包含动画特效和增强组件的首屏 UI 布局 JSON。
    """
    return {
        "flex": "column",
        "justify": "center",
        "align_items": "center",
        "gap": 15,
        "children": [
            # ---- 粒子背景层 (默认隐藏，通过控制台 'particle on' 命令开启) ----
            {
                "type": "particle",
                "id": "bg_particle",
                "canvas_w": 160,
                "canvas_h": 160,
                "count": 15,
                "color": "#e94560",
                "particle_size": 3,
                "duration": 40,
                "hidden": True   # Python True = JSON true, 默认隐藏
            },
            # ---- 进度条 (带 value) ----
            {
                "type": "bar",
                "id": "progress",
                "w": 300,
                "h": 8,
                "value": 45,
                "bg_color": "#2a2a2a",
                "indic_color": "#1db954",
                "radius": 4
            },
            # ---- 计数标签 (带跑马灯) ----
            {
                "type": "label",
                "id": "count_label",
                "text": "欢迎使用 SDUI 增强版系统 · 当前计数：0",
                "font_size": 20,
                "w": 250,
                "long_mode": "marquee"
            },
            # ---- 音量滑块 ----
            {
                "type": "slider",
                "id": "vol_slider",
                "w": 250,
                "value": 70,
                "min": 0,
                "max": 100,
                "on_change": "server://ui/volume"
            },
            # ---- 按钮组容器 (横向排列) ----
            {
                "type": "container",
                "flex": "row",
                "gap": 20,
                "children": [
                    {
                        "type": "button",
                        "id": "btn_add",
                        "text": "Add +1",
                        "w": 120, "h": 50,
                        "bg_color": "#3498db",
                        "radius": 25,
                        "on_click": "server://ui/click",
                        # 呼吸动画
                        "anim": {"type": "breathe", "min_opa": 120, "max_opa": 255, "duration": 1500}
                    },
                    {
                        "type": "button",
                        "id": "btn_rec",
                        "text": "Hold to Talk",
                        "w": 140, "h": 50,
                        "bg_color": "#2ecc71",
                        "radius": 25,
                        "on_press": "local://audio/cmd/record_start",
                        "on_release": "local://audio/cmd/record_stop",
                        # 颜色脉冲动画 (按下说话时更醒目)
                        "anim": {"type": "color_pulse", "color_a": "#2ecc71", "color_b": "#27ae60", "duration": 800, "repeat": -1}
                    }
                ]
            },
            # ---- STT 结果标签 ----
            {
                "type": "label",
                "id": "stt_label",
                "text": "Ready",
                "font_size": 16,
                "text_color": "#888888"
            }
        ]
    }

def build_test_scroll_layout():
    """测试 scrollable 容器布局"""
    items = []
    for i in range(1, 11):
        items.append({
            "type": "label",
            "text": f"Scrollable Item No.{i}",
            "font_size": 24,
            "pad": 10,
            "bg_color": "#333333",
            "w": "full",
            "anim": {"type": "slide_in", "from": "right", "duration": 300 + i * 50}  # 级联滑入效果
        })
    
    return {
        "flex": "column", "justify": "center", "align_items": "center", "gap": 15,
        "children": [
            {"type": "label", "text": "Scroll Container Test", "font_size": 20},
            {
                "type": "container",
                "id": "scroll_box",
                "scrollable": True,
                "w": 380, "h": 280,
                "flex": "column", "gap": 10,
                "bg_color": "#111111", "pad": 15, "radius": 10,
                "children": items
            },
            {
                "type": "button", "id": "btn_back", "text": "Back",
                "w": 120, "h": 40, "bg_color": "#e74c3c", "radius": 20,
                "on_click": "server://ui/action"
            }
        ]
    }



# ============================================================
#  辅助函数
# ============================================================
async def send_topic(ws, topic: str, payload):
    """封装并发送一条 SDUI 协议消息"""
    msg = json.dumps({"topic": topic, "payload": payload}, ensure_ascii=False)
    logging.debug(f"↓ SEND [{topic}] payload_len={len(msg)}")
    await ws.send(msg)


async def send_layout(ws, layout: dict):
    """发送全量布局指令"""
    logging.info("↓ Sending ui/layout (full render)")
    await send_topic(ws, "ui/layout", layout)


async def send_update(ws, widget_id: str, **props):
    """
    发送增量更新指令。
    示例: send_update(ws, "count_label", text="Count: 5")
    """
    update = {"id": widget_id, **props}
    logging.info(f"↓ Sending ui/update → {update}")
    await send_topic(ws, "ui/update", update)


# ============================================================
#  主处理函数
# ============================================================
async def sdui_handler(websocket):
    global global_count, audio_buffer
    remote = websocket.remote_address
    logging.info(f"✦ Terminal connected: {remote}")

    # 该连接关联的 device_id（在收到第一条 telemetry 心跳后确定）
    connection_device_id = None

    # ---- 连接建立后立即下发首屏布局 ----
    home_layout = build_home_layout()
    logging.info(f"  Home layout: {len(home_layout['children'])} widgets")
    await send_layout(websocket, home_layout)

    try:
        async for message in websocket:
            # ---- 解析消息 ----
            try:
                data = json.loads(message)
            except json.JSONDecodeError:
                logging.warning(f"✗ Invalid JSON received: {message[:100]}")
                continue

            topic = data.get("topic")
            payload = data.get("payload", {})
            # 从信封顶层读取 device_id（新协议），如未有则使用连接级存储的 ID
            msg_device_id = data.get("device_id") or connection_device_id or "UNKNOWN"
            # 同步连接级设备 ID
            if data.get("device_id") and connection_device_id != data.get("device_id"):
                connection_device_id = data.get("device_id")

            logging.info(f"↑ RECV [{topic}] from={msg_device_id} payload={json.dumps(payload, ensure_ascii=False)[:200]}")

            # ==== 0. 设备遥测心跳 ====
            if topic == "telemetry/heartbeat":
                if isinstance(payload, dict):
                    # 兼容新旧协议：优先从信封顶层取 device_id，其次从 payload 内取
                    device_id = msg_device_id if msg_device_id != "UNKNOWN" else payload.get("device_id", "UNKNOWN")
                    connection_device_id = device_id

                    # 更新注册表
                    if device_id not in devices:
                        logging.info(f"  ★ New device registered: {device_id} from {remote}")
                    devices[device_id] = {
                        "ws":           websocket,
                        "addr":         str(remote),
                        "telemetry":    payload,
                        "last_seen":    time.strftime("%H:%M:%S"),
                    }

                    # 格式化打印遥测摘要
                    rssi  = payload.get("wifi_rssi", 0)
                    ip    = payload.get("ip", "?")
                    temp  = payload.get("temperature", -1)
                    heap_int = payload.get("free_heap_internal", 0)
                    heap_tot = payload.get("free_heap_total", 0)
                    uptime   = payload.get("uptime_s", 0)
                    logging.info(
                        f"  ♥ HEARTBEAT [{device_id}] "
                        f"IP:{ip} RSSI:{rssi}dBm Temp:{temp:.1f}°C "
                        f"HeapInt:{heap_int//1024}KB HeapTot:{heap_tot//1024}KB "
                        f"Uptime:{uptime}s"
                    )
                continue

            # ==== 1. UI 点击事件 ====
            elif topic == "ui/click" or topic == "ui/action":
                btn_id = payload.get("id") if isinstance(payload, dict) else payload
                logging.debug(f"  Action from: {btn_id} [{msg_device_id}]")

                if btn_id == "btn_add":
                    global_count += 1
                    await send_update(websocket, "count_label", text=f"欢迎使用 SDUI 增强版系统 · 当前计数：{global_count}")
                    # 同时让进度条增加 5，演示 update animation
                    progress = (global_count * 5) % 100
                    await send_update(websocket, "progress", value=progress)
                    logging.info(f"  ✓ [{msg_device_id}] Count → {global_count}, Progress → {progress}")

                elif btn_id == "btn_back":
                    # 返回主页 (演示布局切换的 Fade 动画)
                    await send_layout(websocket, build_home_layout())
                    logging.info("  ✓ Switch to home layout")

                elif btn_id == "btn_play_audio":
                    if len(audio_buffer) == 0:
                        logging.warning("  ⚠ Audio buffer empty, nothing to play.")
                        await send_update(websocket, "stt_label", text="No audio recorded")
                        continue

                    logging.info(f"  ▶ Streaming {len(audio_buffer)} bytes back to terminal...")
                    chunk_size = 1024
                    for i in range(0, len(audio_buffer), chunk_size):
                        chunk = audio_buffer[i:i + chunk_size]
                        b64_chunk = base64.b64encode(chunk).decode('utf-8')
                        await send_topic(websocket, "audio/play", b64_chunk)
                        await asyncio.sleep(0.02)
                    logging.info("  ✓ Streaming finished.")

                else:
                    logging.debug(f"  Unhandled button: {btn_id}")

            # ==== 2. 上行音频流 ====
            elif topic == "audio/record":
                state = payload.get("state") if isinstance(payload, dict) else None

                if state == "start":
                    logging.info("  🎙 Recording Started")
                    audio_buffer.clear()
                    await send_update(websocket, "stt_label", text="Recording...")

                elif state == "stream":
                    b64_data = payload.get("data", "")
                    if b64_data:
                        decoded = base64.b64decode(b64_data)
                        audio_buffer.extend(decoded)
                        logging.debug(f"  📦 Audio chunk: +{len(decoded)}B, total={len(audio_buffer)}B")

                elif state == "stop":
                    logging.info(f"  ⏹ Recording Stopped. Total: {len(audio_buffer)} bytes")
                    await send_update(websocket, "stt_label", text="Processing...")

                    if len(audio_buffer) > 0:
                        # --- 保存 debug WAV ---
                        debug_filename = "debug_recv.wav"
                        try:
                            with wave.open(debug_filename, 'wb') as f:
                                f.setnchannels(1)
                                f.setsampwidth(2)
                                f.setframerate(22050)
                                f.writeframes(audio_buffer)
                            logging.info(f"  💾 Audio saved → {os.path.abspath(debug_filename)}")
                        except Exception as e:
                            logging.error(f"  ✗ Failed to save debug wav: {e}")

                        # --- STT 解析 ---
                        try:
                            wav_io = io.BytesIO()
                            with wave.open(wav_io, 'wb') as f:
                                f.setnchannels(1)
                                f.setsampwidth(2)
                                f.setframerate(22050)
                                f.writeframes(audio_buffer)
                            wav_io.seek(0)

                            with sr.AudioFile(wav_io) as source:
                                audio_data = recognizer.record(source)
                                text = recognizer.recognize_google(audio_data, language='zh-CN')
                                logging.info(f"  🗣 STT Result: {text}")
                                await send_update(websocket, "stt_label", text=f"You said: {text}")
                        except sr.UnknownValueError:
                            logging.warning("  ⚠ STT: No speech detected")
                            await send_update(websocket, "stt_label", text="(no speech)")
                        except Exception as e:
                            logging.warning(f"  ⚠ STT failed: {e}")
                            await send_update(websocket, "stt_label", text="STT error")

            # ==== 3. IMU 运动事件 ====
            elif topic == "motion":
                motion_type = payload.get("type") if isinstance(payload, dict) else "unknown"
                magnitude = payload.get("magnitude", 0) if isinstance(payload, dict) else 0
                logging.info(f"  📳 Motion event: type={motion_type}, magnitude={magnitude:.1f}")

                if motion_type == "shake":
                    # 摇一摇：可以触发任意 Agent 动作，这里演示更新 UI
                    await send_update(websocket, "stt_label", text="🫨 Shake detected!")
                    # 给背景容器加个 shake 动画
                    await send_update(websocket, "count_label", anim={"type":"shake", "amplitude": 15, "duration": 400})
                    logging.info("  ✓ Shake handled → UI updated (anim triggered)")

            # ==== 4. 滑块数值变化 ====
            elif topic == "ui/volume":
                slider_id = payload.get("id")
                val = payload.get("value")
                logging.info(f"  🎚 Slider '{slider_id}' changed to {val}")
                await send_update(websocket, "stt_label", text=f"Volume set to {val}%")
                # 同步更新顶部进度条，演示联动
                await send_update(websocket, "progress", value=val)

            # ==== 5. 未知主题 ====
            else:
                logging.warning(f"  ？ Unknown topic: {topic}")

    except websockets.exceptions.ConnectionClosed as e:
        logging.info(f"✦ Terminal disconnected: {remote} (code={e.code})")
        # 从注册表中移除（标记为断线）
        if connection_device_id and connection_device_id in devices:
            devices[connection_device_id]["ws"] = None
            devices[connection_device_id]["last_seen"] += " (offline)"
            logging.info(f"  Device {connection_device_id} marked offline")


# ============================================================
#  DEBUG 命令行工具：手动下发布局/更新
# ============================================================
async def debug_console(connected_ws):
    """
    启动一个可选的后台调试控制台。
    可在终端运行时实时手动发布指令测试。
    
    命令示例:
      layout             -- 重新下发首屏布局
      update id text     -- 增量更新 (如: update count_label Count:99)
      raw {...}          -- 发送原始 JSON
    """
    logging.info("🔧 Debug console ready. Type 'help' for commands.")
    loop = asyncio.get_event_loop()

    while True:
        try:
            line = await loop.run_in_executor(None, input)
        except EOFError:
            break

        line = line.strip()
        if not line:
            continue

        ws = connected_ws.get("current")
        if not ws:
            logging.warning("No terminal connected.")
            continue

        try:
            if line == "help":
                print("Commands:")
                print("  layout                    -- Re-send home layout (High-Expressive)")
                print("  scroll                    -- Send scrollable container layout")
                print("  particle on/off           -- Show/hide particle effect")
                print("  update <id> <text>         -- Update widget text")
                print("  hide <id>                  -- Hide widget")
                print("  show <id>                  -- Show widget")
                print("  anim <id> <type>           -- Trigger anim (e.g. anim btn_add blink)")
                print("  raw <json>                 -- Send raw JSON")
                print("  count                      -- Show current count")
                print("  list                       -- List all registered devices")
                print("  send <device_id> <topic> <payload>  -- Send to specific device")
            elif line == "layout":
                await send_layout(ws, build_home_layout())
            elif line == "scroll":
                await send_layout(ws, build_test_scroll_layout())
            elif line.startswith("particle "):
                state = line.split(" ", 1)[1]
                hidden = False if state == "on" else True
                await send_update(ws, "bg_particle", hidden=hidden)
            elif line.startswith("anim "):
                parts = line.split(" ", 2)
                if len(parts) >= 3:
                    wid, atype = parts[1], parts[2]
                    anim_desc = {"type": atype, "duration": 800}
                    if atype == "shake": anim_desc["amplitude"] = 12
                    await send_update(ws, wid, anim=anim_desc)
                else:
                    print("Usage: anim <id> <type>")
            elif line.startswith("update "):
                parts = line.split(" ", 2)
                if len(parts) >= 3:
                    await send_update(ws, parts[1], text=parts[2])
                else:
                    print("Usage: update <widget_id> <text>")
            elif line.startswith("hide "):
                wid = line.split(" ", 1)[1]
                await send_update(ws, wid, hidden=True)
            elif line.startswith("show "):
                wid = line.split(" ", 1)[1]
                await send_update(ws, wid, hidden=False)
            elif line.startswith("raw "):
                raw = line[4:]
                await ws.send(raw)
                logging.info(f"↓ RAW sent: {raw[:200]}")
            elif line == "count":
                print(f"Current count: {global_count}")
            elif line == "list":
                if not devices:
                    print("No devices registered.")
                else:
                    print(f"{'Device ID':<16} {'IP':<16} {'RSSI':>6} {'Temp':>7} {'HeapInt':>9} {'Uptime':>8} {'Last Seen':<12} Status")
                    print("-" * 90)
                    for did, info in devices.items():
                        tel = info.get("telemetry", {})
                        status = "online" if info.get("ws") else "offline"
                        print(
                            f"{did:<16} {tel.get('ip','?'):<16} "
                            f"{tel.get('wifi_rssi',0):>5}dBm "
                            f"{tel.get('temperature',-1):>6.1f}°C "
                            f"{tel.get('free_heap_internal',0)//1024:>7}KB "
                            f"{tel.get('uptime_s',0):>7}s "
                            f"{info.get('last_seen','?'):<12} {status}"
                        )
            elif line.startswith("send "):
                # send <device_id> <topic> <payload>
                parts = line.split(" ", 3)
                if len(parts) >= 4:
                    target_id, s_topic, s_payload = parts[1], parts[2], parts[3]
                    target_dev = devices.get(target_id)
                    if target_dev and target_dev.get("ws"):
                        await send_topic(target_dev["ws"], s_topic, s_payload)
                        print(f"Sent [{s_topic}] to {target_id}")
                    else:
                        print(f"Device {target_id} not found or offline.")
                else:
                    print("Usage: send <device_id> <topic> <payload>")
            else:
                print(f"Unknown command: {line}. Type 'help'.")
        except Exception as e:
            logging.error(f"Debug command error: {e}")


# ============================================================
#  入口
# ============================================================
connected_ws_ref = {"current": None}

async def tracked_handler(websocket):
    """包装 handler，跟踪当前连接以供调试控制台使用"""
    connected_ws_ref["current"] = websocket
    try:
        await sdui_handler(websocket)
    finally:
        if connected_ws_ref["current"] == websocket:
            connected_ws_ref["current"] = None


async def main():
    server = await websockets.serve(tracked_handler, "0.0.0.0", 8080)
    logging.info("═══════════════════════════════════════════════")
    logging.info("  SDUI Gateway Server started on ws://0.0.0.0:8080")
    logging.info("  Protocol: Container Layout + Action URI")
    logging.info("═══════════════════════════════════════════════")

    # 启动调试控制台（可选，非阻塞）
    asyncio.create_task(debug_console(connected_ws_ref))

    await asyncio.Future()  # 永不退出


if __name__ == "__main__":
    asyncio.run(main())