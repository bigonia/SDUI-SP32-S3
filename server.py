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


# ============================================================
#  首屏布局定义 (Server 驱动 UI)
# ============================================================
def build_home_layout():
    """
    构建首屏 UI 布局 JSON。
    终端收到后会清除 Loading 动画并渲染此布局。
    针对 1.75" 圆屏 (466x466, 安全区 386x386)。
    """
    return {
        "flex": "column",
        "justify": "center",
        "align_items": "center",
        "gap": 12,
        "children": [
            # ---- 计数标签 ----
            {
                "type": "label",
                "id": "count_label",
                "text": "Count: 0",
                "font_size": 24
            },
            # ---- +1 按钮 (上报 Server，由 Server 维护计数) ----
            {
                "type": "button",
                "id": "btn_add",
                "text": "Add +1",
                "w": 140,
                "h": 50,
                "on_click": "server://ui/click"
            },
            # ---- 播放音频 按钮 ----
            {
                "type": "button",
                "id": "btn_play_audio",
                "text": "Play Audio",
                "w": 140,
                "h": 50,
                "on_click": "server://ui/click"
            },
            # ---- 按住说话 按钮 (本地直接触发 audio_manager) ----
            {
                "type": "button",
                "id": "btn_rec",
                "text": "Hold to Talk",
                "w": 180,
                "h": 50,
                "bg_color": "#2ecc71",
                "on_press": "local://audio/cmd/record_start",
                "on_release": "local://audio/cmd/record_stop"
            },
            # ---- STT 结果标签 ----
            {
                "type": "label",
                "id": "stt_label",
                "text": "",
                "font_size": 16,
                "text_color": "#888888"
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

            logging.info(f"↑ RECV [{topic}] payload={json.dumps(payload, ensure_ascii=False)[:200]}")

            # ==== 1. UI 点击事件 ====
            if topic == "ui/click":
                btn_id = payload.get("id") if isinstance(payload, dict) else payload
                logging.debug(f"  Button clicked: {btn_id}")

                if btn_id == "btn_add":
                    global_count += 1
                    await send_update(websocket, "count_label", text=f"Count: {global_count}")
                    logging.info(f"  ✓ Count → {global_count}")

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
                    logging.info("  ✓ Shake handled → UI updated")

            # ==== 4. 未知主题 ====
            else:
                logging.warning(f"  ？ Unknown topic: {topic}")

    except websockets.exceptions.ConnectionClosed as e:
        logging.info(f"✦ Terminal disconnected: {remote} (code={e.code})")


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
                print("  layout                    -- Re-send home layout")
                print("  update <id> <text>         -- Update widget text")
                print("  hide <id>                  -- Hide widget")
                print("  show <id>                  -- Show widget")
                print("  raw <json>                 -- Send raw JSON")
                print("  count                      -- Show current count")
            elif line == "layout":
                await send_layout(ws, build_home_layout())
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