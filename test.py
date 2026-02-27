import numpy as np
# 假设你的原始数据已经保存
with open("debug_recv.wav", "rb") as f:
    content = f.read()[44:] # 跳过 44 字节的 WAV 头
    data = np.frombuffer(content, dtype=np.int16)
    print(f"数据总量: {len(data)} 采样点")
    print(f"最大值: {np.max(data)}, 最小值: {np.min(data)}")
    print(f"非零比例: {np.count_nonzero(data) / len(data) * 100:.2f}%")