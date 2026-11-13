/* capture.c - Media Foundation 采集 (UVC/采集卡) + 双缓冲 DIB
 *
 * 为什么不用 avicap(VFW): avicap 连 WDM 驱动时驱动自己会弹「视频源选择」窗口,
 * 而且不少采集卡在 VFW 下只能拿到黑帧 —— 这是"视频输入不可用 + 莫名弹窗"的根因。
 * Media Foundation 直接吃 UVC 设备, 不弹任何对话框。
 *
 * 帧直接写进 DIB Section, 前台读帧与后台抓帧通过临界区换指针, 不拷贝整帧。
 */
#include "capture.h"
#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

#define CMB_MAX_DEVS 16

static IMFActivate    *g_acts[CMB_MAX_DEVS];
static int             g_act_count = 0;
static IMFMediaSource *g_src = NULL;
static IMFSourceReader *g_reader = NULL;
static HANDLE          g_thread = NULL;
static volatile LONG   g_running = 0;
static int             g_fmt_w = 0, g_fmt_h = 0, g_fmt_stride = 0;
static bool            g_mf_ready = false;

static CRITICAL_SECTION g_lock;
static bool g_lock_init = false;

static HBITMAP g_dib[2] = {NULL, NULL};
static unsigned char *g_bits[2] = {NULL, NULL};
static int  g_dib_w = 0, g_dib_h = 0;
static int  g_front = -1;     /* 可读帧索引, -1 = 无帧 */
static int  g_back = 0;       /* 抓帧写入索引 */

static double g_last_t = 0;
static double g_last_copy = 0;
static double g_fps_ema = 0;
static LONGLONG g_ts_origin = -1;      /* 首帧流时间戳, 用来把流时间换算到本机时钟 */
static double  g_ts_origin_wall = 0;
static const GUID GUID_LOW_LATENCY =
    {0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb7, 0x47, 0x22, 0x70, 0x5e, 0x3c}};

static void ensure_lock(void) {
    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = true; }
}

static bool mf_ensure(void) {
    if (g_mf_ready) return true;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    g_mf_ready = true;
    return true;
}

static void set_error(const char *msg, const char *log_msg) {
    strncpy_s(g_state.cap_error, sizeof(g_state.cap_error), msg, _TRUNCATE);
    g_state.capturing = false;
    g_running = 0;
    if (log_msg) cmb_log("error", "%s", log_msg);
    gui_request_repaint();
}

/* 释放两帧缓冲 (调用方无需持锁) */
static void free_dibs(void) {
    ensure_lock();
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < 2; i++) {
        if (g_dib[i]) { DeleteObject(g_dib[i]); g_dib[i] = NULL; }
        g_bits[i] = NULL;
    }
    g_dib_w = g_dib_h = 0;
    g_front = -1; g_back = 0;
    LeaveCriticalSection(&g_lock);
}

/* 确保缓冲尺寸; 返回 false 表示创建失败 */
static bool ensure_dibs(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (g_dib_w == w && g_dib_h == h && g_dib[0] && g_dib[1]) return true;

    for (int i = 0; i < 2; i++) {
        if (g_dib[i]) { DeleteObject(g_dib[i]); g_dib[i] = NULL; g_bits[i] = NULL; }
    }

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          /* 负高度 = 自上而下 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(NULL);
    for (int i = 0; i < 2; i++) {
        void *bits = NULL;
        g_dib[i] = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        g_bits[i] = (unsigned char *)bits;
        if (!g_dib[i] || !bits) {
            ReleaseDC(NULL, screen);
            for (int k = 0; k < 2; k++) if (g_dib[k]) { DeleteObject(g_dib[k]); g_dib[k] = NULL; g_bits[k] = NULL; }
            return false;
        }
    }
    ReleaseDC(NULL, screen);
    g_dib_w = w; g_dib_h = h;
    g_front = -1; g_back = 0;
    return true;
}

/* 把一帧样本拷进后台 DIB (行内 memcpy, 不做逐像素处理) */
static void copy_sample(IMFSample *sample) {
    IMFMediaBuffer *buf = NULL;
    if (FAILED(sample->lpVtbl->ConvertToContiguousBuffer(sample, &buf)) || !buf) return;

    int w = g_fmt_w, h = g_fmt_h;
    if (w <= 0 || h <= 0) { buf->lpVtbl->Release(buf); return; }

    IMF2DBuffer *buf2 = NULL;
    BYTE *base = NULL;
    LONG pitch = 0;
    bool locked2d = false;

    if (SUCCEEDED(buf->lpVtbl->QueryInterface(buf, &IID_IMF2DBuffer, (void **)&buf2)) && buf2) {
        if (SUCCEEDED(buf2->lpVtbl->Lock2D(buf2, &base, &pitch)) && base) locked2d = true;
        else { buf2->lpVtbl->Release(buf2); buf2 = NULL; }
    }
    if (!locked2d) {
        BYTE *p = NULL;
        UINT32 cb = 0;
        if (FAILED(buf->lpVtbl->Lock(buf, &p, NULL, &cb)) || !p) {
            if (buf2) buf2->lpVtbl->Release(buf2);
            buf->lpVtbl->Release(buf);
            return;
        }
        base = p;
        pitch = (LONG)(w * 4);
    }

    ensure_lock();
    EnterCriticalSection(&g_lock);
    if (ensure_dibs(w, h)) {
        unsigned char *dst = g_bits[g_back];
        int back = g_back;
        if (dst) {
            size_t row = (size_t)w * 4;
            for (int y = 0; y < h; y++) {
                /* pitch 为负说明缓冲自下而上, 但 pbScanline0 指向图像首行, 逐行走即可 */
                const BYTE *s = base + (ptrdiff_t)y * pitch;
                memcpy(dst + (size_t)y * row, s, row);
            }
            g_front = back;
            g_back = 1 - back;
            g_state.cap_frames++;
            g_state.cap_width = w;
            g_state.cap_height = h;
            double t = now_sec();
            if (g_last_t > 0 && t > g_last_t) {
                double inst = 1.0 / (t - g_last_t);
                g_fps_ema = g_fps_ema > 0 ? (g_fps_ema * 0.8 + inst * 0.2) : inst;
                g_state.cap_fps = g_fps_ema;
            }
            g_last_t = t;
            g_state.last_frame_at = t;
        }
    }
    LeaveCriticalSection(&g_lock);

    if (locked2d) buf2->lpVtbl->Unlock2D(buf2);
    else buf->lpVtbl->Unlock(buf);
    if (buf2) buf2->lpVtbl->Release(buf2);
    buf->lpVtbl->Release(buf);
}

static void sync_current_media_type(IMFSourceReader *reader) {
    IMFMediaType *mt = NULL;
    if (!reader || FAILED(reader->lpVtbl->GetCurrentMediaType(
            reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &mt)) || !mt) return;

    int w = g_fmt_w, h = g_fmt_h, stride = g_fmt_stride;
    UINT64 size = 0;
    if (SUCCEEDED(mt->lpVtbl->GetUINT64(mt, &MF_MT_FRAME_SIZE, &size))) {
        UINT32 next_w = (UINT32)(size >> 32);
        UINT32 next_h = (UINT32)(size & 0xFFFFFFFFu);
        if (next_w && next_h) { w = (int)next_w; h = (int)next_h; }
    }
    UINT32 next_stride = 0;
    if (SUCCEEDED(mt->lpVtbl->GetUINT32(mt, &MF_MT_DEFAULT_STRIDE, &next_stride)) && next_stride)
        stride = (int)next_stride;
    mt->lpVtbl->Release(mt);

    ensure_lock();
    EnterCriticalSection(&g_lock);
    bool size_changed = w != g_fmt_w || h != g_fmt_h;
    g_fmt_w = w;
    g_fmt_h = h;
    g_fmt_stride = stride;
    if (size_changed && ensure_dibs(w, h)) g_front = -1;
    LeaveCriticalSection(&g_lock);
}

static DWORD WINAPI capture_worker(LPVOID arg) {
    (void)arg;
    ensure_lock();
    double t_start = now_sec();
    int frames_at_start = g_state.cap_frames;

    while (g_running) {
        if (!g_reader) break;

        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample *sample = NULL;
        HRESULT hr = g_reader->lpVtbl->ReadSample(g_reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                  0, &stream, &flags, &ts, &sample);
        if (FAILED(hr)) {
            if (g_running) set_error("读取画面失败：采集卡可能被拔出或被其它程序占用",
                                     "读取画面失败, 已停止预览");
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (g_running) set_error("采集源已结束：设备可能被拔出", "采集源结束, 已停止预览");
            break;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            sync_current_media_type(g_reader);
            if (sample) { sample->lpVtbl->Release(sample); sample = NULL; }
            gui_request_repaint();
            continue;
        }
        if (flags & MF_SOURCE_READERF_STREAMTICK) {
            if (sample) { sample->lpVtbl->Release(sample); sample = NULL; }
            continue;
        }
        if (!sample) {
            if (g_state.cap_frames == frames_at_start && now_sec() - t_start > 5.0) {
                set_error("采集源已连接但 5 秒内没有画面：请确认 HDMI 有信号、采集卡插稳后重新选择视频源",
                          "采集源无画面, 已停止预览");
                break;
            }
            Sleep(2);
            continue;
        }

        /* 设备可能给 60fps, 按目标帧率丢帧, 省掉无谓的整帧拷贝 */
        double now = now_sec();
        if (ts > 0) {
            if (g_ts_origin < 0) {
                g_ts_origin = ts;
                g_ts_origin_wall = now;
            } else {
                double age = (now - g_ts_origin_wall) - (double)(ts - g_ts_origin) / 10000000.0;
                if (age >= 0 && age < 5.0)
                    g_state.cap_latency_ms = (int)(age * 1000.0 + 0.5);
            }
        }
        if (ts > 0 && g_ts_origin >= 0 && g_state.cap_frames > 0) {
            double age = (now - g_ts_origin_wall) - (double)(ts - g_ts_origin) / 10000000.0;
            /* 队列一旦积压, 先读空积压, 只显示最新的一帧 */
            if (age > 0.12) {
                sample->lpVtbl->Release(sample);
                continue;
            }
        }
        double gap = 1.0 / (double)max(1, g_state.fps_cap);
        if (g_state.cap_frames > 0 && now - g_last_copy < gap - 0.002) {
            sample->lpVtbl->Release(sample);
            continue;
        }
        g_last_copy = now;

        copy_sample(sample);
        sample->lpVtbl->Release(sample);
        gui_request_repaint();
    }

    return 0;
}

static void release_acts(void) {
    for (int i = 0; i < g_act_count; i++) {
        if (g_acts[i]) { g_acts[i]->lpVtbl->Release(g_acts[i]); g_acts[i] = NULL; }
    }
    g_act_count = 0;
}

int capture_enum(video_dev_t *out, int max) {
    if (!out || max <= 0) return 0;
    memset(out, 0, sizeof(video_dev_t) * (size_t)max);
    release_acts();
    if (!mf_ensure()) return 0;

    IMFAttributes *attr = NULL;
    if (FAILED(MFCreateAttributes(&attr, 1)) || !attr) return 0;
    attr->lpVtbl->SetGUID(attr, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                          &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate **devs = NULL;
    UINT32 n = 0;
    HRESULT hr = MFEnumDeviceSources(attr, &devs, &n);
    attr->lpVtbl->Release(attr);
    if (FAILED(hr) || !devs) return 0;

    int count = 0;
    for (UINT32 i = 0; i < n; i++) {
        if (count >= max || count >= CMB_MAX_DEVS) {
            devs[i]->lpVtbl->Release(devs[i]);
            continue;
        }
        wchar_t *wname = NULL;
        UINT32 wlen = 0;
        if (FAILED(devs[i]->lpVtbl->GetAllocatedString(devs[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                                       &wname, &wlen)) || !wname) {
            devs[i]->lpVtbl->Release(devs[i]);
            continue;
        }
        char nm[160] = "";
        WideCharToMultiByte(CP_UTF8, 0, wname, -1, nm, sizeof(nm), NULL, NULL);
        CoTaskMemFree(wname);
        if (!nm[0]) { devs[i]->lpVtbl->Release(devs[i]); continue; }

        snprintf(out[count].name, sizeof(out[count].name), "%s", nm);
        out[count].index = count;
        g_acts[count] = devs[i];
        count++;
    }
    CoTaskMemFree(devs);
    g_act_count = count;
    return count;
}

bool capture_start(int source, int w, int h, int fps) {
    capture_stop();
    /* fps 传 0/负数 表示"用默认", 不能真按 1fps 跑 */
    if (fps <= 0) fps = g_state.fps_cap > 0 ? g_state.fps_cap : 60;
    if (fps < 5) fps = 5;
    if (fps > 240) fps = 240;
    g_state.fps_cap = fps;

    if (!mf_ensure()) {
        strncpy_s(g_state.cap_error, sizeof(g_state.cap_error),
                  "无法初始化 Media Foundation", _TRUNCATE);
        return false;
    }
    if (source < 0) {
        strncpy_s(g_state.cap_error, sizeof(g_state.cap_error), "尚未选择采集源", _TRUNCATE);
        return false;
    }
    /* 每次打开都重新枚举一次: 被 Shutdown 过的 IMFActivate 再激活会失效 */
    video_dev_t tmpdev[CMB_MAX_DEVS];
    capture_enum(tmpdev, CMB_MAX_DEVS);
    if (source >= g_act_count || !g_acts[source]) {
        strncpy_s(g_state.cap_error, sizeof(g_state.cap_error),
                  "采集源列表已变化, 请点「刷新」后重新选择", _TRUNCATE);
        return false;
    }

    IMFMediaSource *src = NULL;
    HRESULT hr_act = g_acts[source]->lpVtbl->ActivateObject(g_acts[source], &IID_IMFMediaSource,
                                                           (void **)&src);
    if (FAILED(hr_act) || !src) {
        /* 刚关掉同一张卡时驱动可能还没放干净, 等 300ms 再试一次 */
        Sleep(300);
        src = NULL;
        hr_act = g_acts[source]->lpVtbl->ActivateObject(g_acts[source], &IID_IMFMediaSource,
                                                        (void **)&src);
    }
    if (FAILED(hr_act) || !src) {
        strncpy_s(g_state.cap_error, sizeof(g_state.cap_error),
                  "无法打开采集源：可能被其它程序占用", _TRUNCATE);
        cmb_log("error", "打开采集源失败: #%d hr=0x%08lX", source, (unsigned long)hr_act);
        return false;
    }

    IMFAttributes *ra = NULL;
    if (SUCCEEDED(MFCreateAttributes(&ra, 3)) && ra) {
        /* 打开颜色转换, 让 RGB32 输出能由 MF 自带转换器补齐 */
        ra->lpVtbl->SetUINT32(ra, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        /* 低延迟模式: 关掉 MF 的额外缓冲, 画面更跟手 */
        ra->lpVtbl->SetUINT32(ra, &GUID_LOW_LATENCY, TRUE);
    }
    g_ts_origin = -1;
    g_ts_origin_wall = 0;
    g_state.cap_latency_ms = -1;
    IMFSourceReader *rd = NULL;
    HRESULT hr = MFCreateSourceReaderFromMediaSource(src, ra, &rd);
    if (ra) ra->lpVtbl->Release(ra);
    if (FAILED(hr) || !rd) {
        src->lpVtbl->Release(src);
        strncpy_s(g_state.cap_error, sizeof(g_state.cap_error), "创建画面读取器失败", _TRUNCATE);
        return false;
    }

    /* 首选: 指定 RGB32 + 目标尺寸 + 帧率 */
    IMFMediaType *mt = NULL;
    MFCreateMediaType(&mt);
    mt->lpVtbl->SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    mt->lpVtbl->SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    UINT32 rw = (w > 0 ? (UINT32)w : 1280u);
    UINT32 rh = (h > 0 ? (UINT32)h : 720u);
    mt->lpVtbl->SetUINT64(mt, &MF_MT_FRAME_SIZE, ((UINT64)rw << 32) | rh);
    mt->lpVtbl->SetUINT64(mt, &MF_MT_FRAME_RATE,
                          ((UINT64)(fps > 0 ? (UINT32)fps : 30u) << 32) | 1u);
    mt->lpVtbl->SetUINT32(mt, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = rd->lpVtbl->SetCurrentMediaType(rd, MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, mt);
    mt->lpVtbl->Release(mt);

    if (FAILED(hr)) {
        /* 次选: 只要求 RGB32, 尺寸交给设备 */
        IMFMediaType *m2 = NULL;
        MFCreateMediaType(&m2);
        m2->lpVtbl->SetGUID(m2, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        m2->lpVtbl->SetGUID(m2, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
        hr = rd->lpVtbl->SetCurrentMediaType(rd, MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, m2);
        m2->lpVtbl->Release(m2);
    }
    if (FAILED(hr)) {
        rd->lpVtbl->Release(rd);
        src->lpVtbl->Release(src);
        strncpy_s(g_state.cap_error, sizeof(g_state.cap_error),
                  "采集源不支持 RGB32 输出格式", _TRUNCATE);
        cmb_log("error", "设置画面格式失败: 0x%08lX", (unsigned long)hr);
        return false;
    }

    g_fmt_w = w > 0 ? w : 1280;
    g_fmt_h = h > 0 ? h : 720;
    g_fmt_stride = g_fmt_w * 4;
    IMFMediaType *cur = NULL;
    if (SUCCEEDED(rd->lpVtbl->GetCurrentMediaType(rd, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) && cur) {
        UINT64 sz = 0;
        if (SUCCEEDED(cur->lpVtbl->GetUINT64(cur, &MF_MT_FRAME_SIZE, &sz))) {
            UINT32 aw = (UINT32)(sz >> 32), ah = (UINT32)(sz & 0xFFFFFFFFu);
            if (aw && ah) { g_fmt_w = (int)aw; g_fmt_h = (int)ah; }
        }
        UINT32 st = 0;
        if (SUCCEEDED(cur->lpVtbl->GetUINT32(cur, &MF_MT_DEFAULT_STRIDE, &st)))
            g_fmt_stride = (int)st;
        cur->lpVtbl->Release(cur);
    }

    ensure_lock();
    EnterCriticalSection(&g_lock);
    bool ok = ensure_dibs(g_fmt_w, g_fmt_h);
    LeaveCriticalSection(&g_lock);
    if (!ok) {
        rd->lpVtbl->Release(rd);
        src->lpVtbl->Release(src);
        strncpy_s(g_state.cap_error, sizeof(g_state.cap_error), "画面缓冲分配失败", _TRUNCATE);
        return false;
    }

    g_src = src;
    g_reader = rd;
    g_state.capturing = true;
    g_state.cap_error[0] = 0;
    g_state.video_source = source;
    g_state.cap_fps = 0;
    g_state.cap_width = g_fmt_w;
    g_state.cap_height = g_fmt_h;
    g_last_t = 0;
    g_fps_ema = 0;
    g_running = 1;
    g_thread = CreateThread(NULL, 0, capture_worker, NULL, 0, NULL);
    if (g_thread) SetThreadPriority(g_thread, THREAD_PRIORITY_BELOW_NORMAL);
    if (!g_thread) {
        g_running = 0;
        rd->lpVtbl->Release(rd); g_reader = NULL;
        src->lpVtbl->Release(src); g_src = NULL;
        g_state.capturing = false;
        strncpy_s(g_state.cap_error, sizeof(g_state.cap_error), "无法创建采集线程", _TRUNCATE);
        return false;
    }
    {
        wchar_t *nm = NULL;
        UINT32 nl = 0;
        char nb[160] = "";
        if (SUCCEEDED(g_acts[source]->lpVtbl->GetAllocatedString(
                g_acts[source], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &nm, &nl)) && nm) {
            WideCharToMultiByte(CP_UTF8, 0, nm, -1, nb, sizeof(nb), NULL, NULL);
            CoTaskMemFree(nm);
        }
        cmb_log("ok", "采集源已打开: %hs (%dx%d)", nb, g_fmt_w, g_fmt_h);
    }
    return true;
}

void capture_stop(void) {
    g_running = 0;
    /* 先 Shutdown 媒体源, 让阻塞中的 ReadSample 立刻返回, 否则关源后立刻重开会失败 */
    if (g_src) g_src->lpVtbl->Shutdown(g_src);
    if (g_thread) {
        DWORD wr = WaitForSingleObject(g_thread, 2500);
        if (wr == WAIT_OBJECT_0) { CloseHandle(g_thread); g_thread = NULL; }
        else {
            /* 驱动仍卡在 ReadSample 里: 不释放 COM 对象, 避免工作线程踩空 */
            cmb_log("warn", "采集线程未在 2.5 秒内退出, 跳过本次资源释放");
            g_thread = NULL;
            g_state.capturing = false;
            g_state.cap_fps = 0;
            return;
        }
    }
    if (g_reader) { g_reader->lpVtbl->Release(g_reader); g_reader = NULL; }
    if (g_src) { g_src->lpVtbl->Release(g_src); g_src = NULL; }
    free_dibs();
    g_state.capturing = false;
    g_state.cap_fps = 0;
}

bool capture_is_active(void) { return g_reader != NULL; }

bool capture_blit(HDC hdc, const RECT *dst) {
    bool painted = false;
    if (!g_lock_init) return false;
    EnterCriticalSection(&g_lock);
    if (g_front >= 0 && g_dib[g_front] && g_bits[g_front] &&
        g_dib_w > 0 && g_dib_h > 0) {
        int dw = dst->right - dst->left;
        int dh = dst->bottom - dst->top;
        if (dw > 0 && dh > 0) {
            /* 等比适配 */
            double sx = (double)dw / g_dib_w;
            double sy = (double)dh / g_dib_h;
            double s = min(sx, sy);
            int w = (int)(g_dib_w * s);
            int h = (int)(g_dib_h * s);
            int x = dst->left + (dw - w) / 2;
            int y = dst->top + (dh - h) / 2;

            BITMAPINFO bi;
            ZeroMemory(&bi, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = g_dib_w;
            bi.bmiHeader.biHeight = -g_dib_h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;

            SetStretchBltMode(hdc, COLORONCOLOR);
            StretchDIBits(hdc, x, y, w, h, 0, 0, g_dib_w, g_dib_h,
                          g_bits[g_front], &bi, DIB_RGB_COLORS, SRCCOPY);
            painted = true;
        }
    }
    LeaveCriticalSection(&g_lock);
    return painted;
}

void capture_thread_tick(void) { /* 采集是独立线程, 无需主循环驱动 */ }

void capture_set_fps(int fps) {
    if (fps < 5) fps = 5;
    if (fps > 240) fps = 240;
    if (capture_is_active()) g_state.fps_cap = fps;
    else g_state.fps_cap = fps;
}

bool capture_save_bmp(const char *path) {
    if (!g_lock_init || !path || !*path) return false;
    bool ok = false;
    EnterCriticalSection(&g_lock);
    if (g_front >= 0 && g_bits[g_front] && g_dib_w > 0 && g_dib_h > 0) {
        int w = g_dib_w, h = g_dib_h;
        long row = ((long)(w * 4) + 3) & ~3L;
        BITMAPFILEHEADER fh;
        ZeroMemory(&fh, sizeof(fh));
        BITMAPINFOHEADER ih;
        ZeroMemory(&ih, sizeof(ih));
        ih.biSize = sizeof(ih);
        ih.biWidth = w;
        ih.biHeight = h;              /* BMP 底部在上, 原样写正高度 */
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        ih.biCompression = BI_RGB;
        ih.biSizeImage = (DWORD)(row * h);
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + ih.biSizeImage;

        FILE *fp = NULL;
        if (fopen_s(&fp, path, "wb") == 0 && fp) {
            fwrite(&fh, sizeof(fh), 1, fp);
            fwrite(&ih, sizeof(ih), 1, fp);
            /* 内存里是 top-down, 写成 bottom-up */
            for (int y = h - 1; y >= 0; y--)
                fwrite((const char *)g_bits[g_front] + row * y, 1, (size_t)row, fp);
            fclose(fp);
            ok = true;
        }
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}


