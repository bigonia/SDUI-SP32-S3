import asyncio
import websockets
import json
import logging
import base64
import wave
import io
import os
import time
from concurrent.futures import ThreadPoolExecutor

# AI 相关依赖
from openai import AsyncOpenAI
from faster_whisper import WhisperModel
import edge_tts

# ============================================================
#  SDUI Gateway Server — DeepSeek AI 语音对话终端
# ============================================================
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s'
)
logging.getLogger("websockets").setLevel(logging.WARNING)

# ---- AI 引擎配置 ----
# 1. DeepSeek API 配置 (请替换为您自己的 API KEY)
DEEPSEEK_API_KEY = os.getenv("DEEPSEEK_API_KEY", "sk-ce6b2df0dfa6455e9c862f033dbbb16b")
aclient = AsyncOpenAI(api_key=DEEPSEEK_API_KEY, base_url="https://api.deepseek.com")

# 2. Faster-Whisper 本地 STT 配置
logging.info("⏳ 正在加载本地 Whisper STT 模型...")
# 使用 CPU 和 int8 量化，保证在普通机器上也有极快的推理速度
whisper_model = WhisperModel("base", device="cpu", compute_type="int8")
executor = ThreadPoolExecutor(max_workers=4)
logging.info("✅ STT 模型加载完毕")

# ---- 多终端设备注册表与 Session 状态 ----
# key: device_id
# value: { "ws": ws, "addr": addr, "telemetry": {}, "audio_buffer": bytearray, 
#          "messages": [], "stats": {"rounds": 0, "total_tokens": 0} }
devices: dict = {}

def get_or_create_device(device_id, websocket, remote):
    if device_id not in devices:
        devices[device_id] = {
            "ws": websocket,
            "addr": str(remote),
            "telemetry": {},
            "last_seen": time.strftime("%H:%M:%S"),
            "audio_buffer": bytearray(), # 每个设备独立的音频缓冲
            "messages": [],              # 多轮对话历史
            "stats": {"rounds": 0, "total_tokens": 0} # 统计数据
        }
    else:
        devices[device_id]["ws"] = websocket
        devices[device_id]["addr"] = str(remote)
    return devices[device_id]


# ============================================================
#  UI 布局构建器 (SDUI 引擎)
# ============================================================
def build_chat_bubble(text, is_user=False):
    """构建单条聊天气泡 UI"""
    bg_color = "#2ecc71" if is_user else "#333333" # 用户绿色，AI深灰
    text_color = "#ffffff"
    # SDUI 中的 long_mode 设为 scroll 可以让长文本在气泡内滚动，避免撑爆容器
    return {
        "type": "container",
        "w": "full",
        "h": "content",
        "bg_color": bg_color,
        "radius": 10,
        "pad": 10,
        "flex": "column",
        "justify": "center",
        "children": [
            {
                "type": "label",
                "text": text,
                "font_size": 16,
                "text_color": text_color,
                "w": "full",
                "long_mode": "scroll"
            }
        ]
    }

def build_ai_layout(device_state):
    """构建沉浸式 AI 对话终端布局"""
    stats = device_state["stats"]
    messages = device_state["messages"]
    
    # 抽取需要展示的对话记录 (过滤掉 system prompt)
    display_msgs = [m for m in messages if m["role"] != "system"]
    
    # 渲染历史对话气泡
    bubble_children = []
    if not display_msgs:
        bubble_children.append({
            "type": "label",
            "text": "请按住底部按钮开始对话...",
            "font_size": 16,
            "text_color": "#888888",
            "align": "center"
        })
    else:
        for msg in display_msgs:
            bubble_children.append(build_chat_bubble(msg["content"], is_user=(msg["role"]=="user")))

    # 构建完整 JSON 树
    return {
        "flex": "column",
        "justify": "start",
        "align_items": "center",
        "gap": 10,
        "children": [
            # 1. 顶部状态栏
            {
                "type": "label",
                "id": "status_label",
                "text": "🟢 系统就绪，等待唤醒",
                "font_size": 16,
                "text_color": "#f1c40f"
            },
            # 2. 统计信息栏
            {
                "type": "container",
                "flex": "row",
                "justify": "space_between",
                "w": "90%",
                "h": 30,
                "children": [
                    {"type": "label", "text": f"💬 轮数: {stats['rounds']}", "font_size": 14, "text_color": "#aaaaaa"},
                    {"type": "label", "text": f"🪙 Tokens: {stats['total_tokens']}", "font_size": 14, "text_color": "#aaaaaa"}
                ]
            },
            # 3. 对话历史滚动区
            {
                "type": "container",
                "id": "scroll_box",
                "scrollable": True,
                "w": "95%", 
                "h": 260, # 给底部留出空间
                "flex": "column", 
                "gap": 10,
                "bg_color": "#111111", 
                "pad": 10, 
                "radius": 10,
                "children": bubble_children
            },
            # 4. 底部交互控制区
            {
                "type": "container",
                "flex": "row",
                "gap": 20,
                "w": "full",
                "justify": "center",
                "children": [
                    {
                        "type": "button",
                        "id": "btn_new_chat",
                        "text": "🔄 新对话",
                        "w": 100, "h": 50,
                        "bg_color": "#e74c3c",
                        "radius": 25,
                        "on_click": "server://ui/new_chat"
                    },
                    {
                        "type": "button",
                        "id": "btn_rec",
                        "text": "🎙️ 按住说话",
                        "w": 140, "h": 50,
                        "bg_color": "#3498db",
                        "radius": 25,
                        "on_press": "local://audio/cmd/record_start",
                        "on_release": "local://audio/cmd/record_stop",
                        # 按下时的呼吸动画
                        "anim": {"type": "color_pulse", "color_a": "#3498db", "color_b": "#2980b9", "duration": 800, "repeat": -1}
                    }
                ]
            }
        ]
    }

# ============================================================
#  辅助发送函数
# ============================================================
async def send_topic(ws, topic: str, payload):
    msg = json.dumps({"topic": topic, "payload": payload}, ensure_ascii=False)
    await ws.send(msg)

async def send_layout(ws, layout: dict):
    await send_topic(ws, "ui/layout", layout)

async def send_update(ws, widget_id: str, **props):
    update = {"id": widget_id, **props}
    await send_topic(ws, "ui/update", update)

# ============================================================
#  AI 业务流水线 (STT -> LLM -> TTS)
# ============================================================
def stt_task(audio_bytes):
    """[同步任务] 供线程池调用：将字节流写入临时文件并使用 faster-whisper 识别"""
    tmp_file = f"tmp_stt_{time.time()}.wav"
    try:
        with wave.open(tmp_file, 'wb') as f:
            f.setnchannels(1)
            f.setsampwidth(2)
            f.setframerate(22050) # 匹配 ESP32 默认录音频率
            f.writeframes(audio_bytes)
        
        # 纯本地识别
        segments, info = whisper_model.transcribe(tmp_file, beam_size=5, language="zh")
        text = "".join([s.text for s in segments])
        return text.strip()
    finally:
        if os.path.exists(tmp_file):
            os.remove(tmp_file)

async def process_chat_round(ws, device_id, device_state):
    """核心 AI 问答流水线"""
    audio_data = bytes(device_state["audio_buffer"])
    device_state["audio_buffer"].clear()
    
    if len(audio_data) < 10000: # 抛弃过短的无意触碰 (约0.5秒)
        await send_update(ws, "status_label", text="🟢 等待唤醒...")
        return

    # --- 保存 debug WAV 便于调试 ---
    debug_filename = f"debug_recv_{device_id}.wav"
    try:
        with wave.open(debug_filename, 'wb') as f:
            f.setnchannels(1)
            f.setsampwidth(2)
            f.setframerate(22050) # 匹配 ESP32 默认录音频率
            f.writeframes(audio_data)
        logging.info(f"[{device_id}] 💾 调试音频已保存 → {os.path.abspath(debug_filename)}")
    except Exception as e:
        logging.error(f"[{device_id}] ✗ 无法保存调试音频: {e}")

    try:
        # 1. 本地 STT (放到线程池中防阻塞异步循环)
        await send_update(ws, "status_label", text="🎙️ 正在识别...")
        loop = asyncio.get_running_loop()
        user_text = await loop.run_in_executor(executor, stt_task, audio_data)
        
        if not user_text:
            logging.warning(f"[{device_id}] STT 识别为空")
            await send_update(ws, "status_label", text="⚠️ 未听到声音，请重试")
            return

        logging.info(f"[{device_id}] 用户: {user_text}")
        
        # 存入上下文并刷新 UI (展示用户提问气泡)
        device_state["messages"].append({"role": "user", "content": user_text})
        await send_layout(ws, build_ai_layout(device_state))
        
        # 2. DeepSeek 大模型请求
        await send_update(ws, "status_label", text="🧠 DeepSeek 思考中...")
        
        # 如果是首次对话，注入系统提示词
        if not any(m["role"] == "system" for m in device_state["messages"]):
            device_state["messages"].insert(0, {
                "role": "system", 
                "content": "你是运行在 ESP32 智能终端上的语音助手，请用简短、自然、口语化的中文回答用户。"
            })

        response = await aclient.chat.completions.create(
            model="deepseek-chat",
            messages=device_state["messages"]
        )
        
        ai_text = response.choices[0].message.content
        used_tokens = response.usage.total_tokens
        
        logging.info(f"[{device_id}] AI: {ai_text} (消耗 {used_tokens} tokens)")
        
        # 记录状态并刷新 UI (展示 AI 回复气泡和状态更新)
        device_state["messages"].append({"role": "assistant", "content": ai_text})
        device_state["stats"]["rounds"] += 1
        device_state["stats"]["total_tokens"] += used_tokens
        await send_layout(ws, build_ai_layout(device_state))
        
        # 3. Edge-TTS 合成并下发流
        await send_update(ws, "status_label", text="🔊 正在播放...")
        
        # ESP32 默认 I2S 驱动能完美播放 16bit-Mono PCM 流，我们将 edge-tts 格式与之匹配
        communicate = edge_tts.Communicate(
            text=ai_text, 
            voice="zh-CN-XiaoxiaoNeural", # 微软优质中文女声
            rate="+10%",                  # 稍微加快一点语速显得更智能
            output_format="raw-16khz-16bit-mono-pcm" 
        )
        
        chunk_buffer = bytearray()
        async for chunk in communicate.stream():
            if chunk["type"] == "audio":
                chunk_buffer.extend(chunk["data"])
                
                # 每积累约 2KB 下发一次切片 (避免终端内存 OOM)
                if len(chunk_buffer) >= 2048:
                    b64_chunk = base64.b64encode(chunk_buffer).decode('utf-8')
                    await send_topic(ws, "audio/play", b64_chunk)
                    chunk_buffer.clear()
                    await asyncio.sleep(0.01) # 略微让渡 CPU 防网络拥塞

        # 发送剩余的切片
        if len(chunk_buffer) > 0:
            b64_chunk = base64.b64encode(chunk_buffer).decode('utf-8')
            await send_topic(ws, "audio/play", b64_chunk)

        await send_update(ws, "status_label", text="🟢 系统就绪，等待唤醒")

    except Exception as e:
        logging.error(f"[{device_id}] Pipeline Error: {e}")
        await send_update(ws, "status_label", text="❌ 发生错误，请重试")


# ============================================================
#  WebSocket 主路由网关
# ============================================================
async def sdui_handler(websocket):
    remote = websocket.remote_address
    connection_device_id = None
    logging.info(f"✦ 终端已连接: {remote}")

    try:
        async for message in websocket:
            try:
                data = json.loads(message)
            except json.JSONDecodeError:
                continue

            topic = data.get("topic")
            payload = data.get("payload", {})
            msg_device_id = data.get("device_id") or connection_device_id or "UNKNOWN"
            
            # 初始化与设备状态绑定
            if msg_device_id != "UNKNOWN":
                connection_device_id = msg_device_id
                device_state = get_or_create_device(msg_device_id, websocket, remote)
            
            # ==== 1. 设备遥测心跳 (建连与保活) ====
            if topic == "telemetry/heartbeat":
                if msg_device_id == "UNKNOWN" and isinstance(payload, dict):
                    msg_device_id = payload.get("device_id", "UNKNOWN")
                    connection_device_id = msg_device_id
                    device_state = get_or_create_device(msg_device_id, websocket, remote)
                
                device_state["telemetry"] = payload
                device_state["last_seen"] = time.strftime("%H:%M:%S")
                
                # 首次收到心跳，下发完整 AI 交互界面
                if not hasattr(websocket, 'initialized'):
                    websocket.initialized = True
                    await send_layout(websocket, build_ai_layout(device_state))
                continue

            if not connection_device_id or connection_device_id == "UNKNOWN":
                continue # 未注册的无效请求

            # ==== 2. 音频链路 ====
            if topic == "audio/record":
                state = payload.get("state")
                if state == "start":
                    device_state["audio_buffer"].clear()
                    await send_update(websocket, "status_label", text="👂 录音中...")
                    # 也可以给界面的某个元素加点动画
                    await send_update(websocket, "scroll_box", anim={"type": "breathe", "min_opa": 180, "max_opa": 255, "duration": 1000})

                elif state == "stream":
                    b64_data = payload.get("data", "")
                    if b64_data:
                        device_state["audio_buffer"].extend(base64.b64decode(b64_data))

                elif state == "stop":
                    # 停止动画，启动处理流水线
                    await send_update(websocket, "scroll_box", anim={"type": "none"})
                    asyncio.create_task(process_chat_round(websocket, connection_device_id, device_state))

            # ==== 3. UI 交互路由 ====
            elif topic == "ui/new_chat":
                logging.info(f"[{connection_device_id}] 用户请求开启新对话")
                # 清理上下文
                device_state["messages"].clear()
                device_state["stats"] = {"rounds": 0, "total_tokens": 0}
                # 全量下发刷新屏幕
                await send_layout(websocket, build_ai_layout(device_state))

    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        logging.info(f"✦ 终端断开连接: {remote}")
        if connection_device_id and connection_device_id in devices:
            devices[connection_device_id]["ws"] = None


async def main():
    server = await websockets.serve(sdui_handler, "0.0.0.0", 8080)
    logging.info("=========================================================")
    logging.info("  🚀 SDUI DeepSeek AI Server started on ws://0.0.0.0:8080")
    logging.info("=========================================================")
    await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())