/* input_bridge.h - 键鼠事件: 本机注入 / 串口透传 */
#ifndef CMB_INPUT_H
#define CMB_INPUT_H
#include "common.h"

typedef struct {
    int  dx, dy;
    int  buttons[3];   /* 0 left, 1 right, 2 middle */
    int  wheel;
    bool absolute;
    int  x, y;
} mouse_ev_t;

typedef struct {
    int  code;         /* Windows VK (解析后) */
    char code_name[32];/* 浏览器 KeyboardEvent.code (优先映射) */
    bool down;
    bool ctrl, shift, alt, meta;
} key_ev_t;

void input_dispatch_mouse(const mouse_ev_t *ev);
void input_dispatch_key(const key_ev_t *ev);
/* Release every button/key currently owned by the bridge on the active transport. */
void input_release_all(void);
/* Clear local input state. Kept as a compatibility alias for older callers. */
void input_clear_keys(void);

#endif
