#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化音频总线与 Codec 芯片，并拉起后台录音任务
void audio_app_start(void);

// 触发录音开始与结束 (供 UI 按钮调用)
void audio_record_start(void);
void audio_record_stop(void);

// 接收云端下发的 Base64 音频并播放
void audio_play_base64(const char *base64_data);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_MANAGER_H