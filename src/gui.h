/* gui.h - 原生 Win32 窗口界面 */
#ifndef CMB_GUI_H
#define CMB_GUI_H
#include "common.h"

int  gui_run(HINSTANCE hInst, int nCmdShow);
void gui_request_repaint(void);   /* 后端线程触发画面刷新 */
void gui_show_message(const char *text);

#endif
