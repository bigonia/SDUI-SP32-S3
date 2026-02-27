#ifndef IMU_MANAGER_H
#define IMU_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

// 启动 IMU 传感器轮询与状态机任务
void imu_app_start(void);

#ifdef __cplusplus
}
#endif

#endif // IMU_MANAGER_H