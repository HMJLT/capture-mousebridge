/* capture.h - 采集卡帧抓取 */
#ifndef CMB_CAPTURE_H
#define CMB_CAPTURE_H
#include "common.h"

int  capture_enum(video_dev_t *out, int max);
bool capture_start(int source, int w, int h, int fps);
void capture_stop(void);
bool capture_is_active(void);

/* 在 dst 矩形内等比居中绘制最新帧(双缓冲直读, 无拷贝); 返回是否画到帧 */
bool capture_blit(HDC hdc, const RECT *dst);
/* 把最新一帧原分辨率存成 BMP (agent 核对画面/取文字用) */
bool capture_save_bmp(const char *path);
/* 运行时改预览帧率(最小化到托盘时降到 10fps 省 CPU, 恢复时还原) */
void capture_set_fps(int fps);

void capture_thread_tick(void);

#endif
