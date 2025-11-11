#include <cmath>
#include <cstdio>
#include <chrono>
#include <thread>

#include "WMPVPlayer.h"

extern "C" {
#include <mpv/client.h>
#include <mpv/render_gl.h>
}

namespace wmediakits {

WMPVPlayer::WMPVPlayer() 
{
}

WMPVPlayer::~WMPVPlayer()
{ 
    Stop(); 
}

bool WMPVPlayer::Init(const std::string& title, int default_w, int default_h, FillMode defaultFill)
{
    m_title = title;
    m_defaultW = default_w;
    m_defaultH = default_h;
    m_fill = defaultFill;
    return true; // 轻量，不做 SDL/mpv 初始化
}

bool WMPVPlayer::Play(const std::string& url, double startSeconds)
{
    if (m_running.exchange(true)) {
        // 已在运行；简化起见返回 false（可扩展为切流）
        return false;
    }

    m_url = url;
    m_startSeconds = startSeconds;
    m_quit.store(false);

    m_thread = std::thread([this] { threadFunc(); });
    return true;
}

void WMPVPlayer::Stop()
{
    if (!m_running.load())
        return;
    m_quit.store(true);

    // 唤醒 SDL_WaitEvent
    if (m_evWake) {
        SDL_Event ev{};
        ev.type = m_evWake;
        SDL_PushEvent(&ev);
    }
    else {
        // 保险起见，再发一个 SDL_QUIT
        SDL_Event ev{};
        ev.type = SDL_QUIT;
        SDL_PushEvent(&ev);
    }

    if (m_thread.joinable())
        m_thread.join();

    m_running.store(false);
}


void WMPVPlayer::RegisterOnDisconnect(OnDisconnect handler)
{
    m_onDisconnect = handler;
}

void WMPVPlayer::TogglePause()
{
    if (!m_mpv) return;
    const char* cmd[] = { "cycle", "pause", nullptr };
    mpv_command_async(m_mpv, 0, cmd);
}

void WMPVPlayer::SeekRelative(double sec)
{
    if (!m_mpv) return;

    // 1) 读当前 time-pos
    double cur = 0.0;
    if (mpv_get_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &cur) < 0)
        return;

    // 2) 读 duration（可选，用于夹紧范围）
    double dur = 0.0;
    if (mpv_get_property(m_mpv, "duration", MPV_FORMAT_DOUBLE, &dur) < 0)
        dur = 0.0;

    // 3) 计算目标，并夹在 [0, dur-0.05]
    double dst = cur + sec;
    if (dst < 0.0) dst = 0.0;
    if (dur > 0.0 && dst > dur - 0.05) dst = dur - 0.05;

    // 4) 直接设置 time-pos（异步、线程安全）
    mpv_set_property_async(m_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE, &dst);
}

void WMPVPlayer::SeekTo(double pos_sec)
{
    if (!m_mpv) return;

    if (!file_loaded.load()) {
        pending_seek_pos.store(pos_sec);
        have_pending_seek.store(true);
        return;
    }

    // 可选夹紧（避免负值；有时 duration 取不到就不做上限夹紧）
    if (pos_sec < 0.0) pos_sec = 0.0;

    // 夹在时长范围内
    double dur = 0.0;
    if (mpv_get_property(m_mpv, "duration", MPV_FORMAT_DOUBLE, &dur) == 0 && dur > 0.0) {
        if (pos_sec > dur - 0.05) pos_sec = dur - 0.05;
    }

    // 绝对 seek：直接设置 time-pos
    mpv_set_property_async(m_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE, &pos_sec);
}

void WMPVPlayer::AddVolume(int delta)
{
    if (!m_mpv) return;
    char buf[32]; std::snprintf(buf, sizeof(buf), "%d", delta);
    const char* cmd[] = { "add", "volume", buf, nullptr };
    mpv_command_async(m_mpv, 0, cmd);
}

void WMPVPlayer::SetRate(double rate)
{
    if (!m_mpv) return;

    if (rate <= 0.0) {
        // AirPlay: rate==0 -> 暂停
        int paused = 1;
        mpv_set_property_async(m_mpv, 0, "pause", MPV_FORMAT_FLAG, &paused);

        // 立刻更新本地快照（更快的 UI 反馈），后续以 PROPERTY_CHANGE 为准
        {
            std::lock_guard<std::mutex> lk(m_infoMx);
            m_info.paused = true;
            m_info.rate = 0.0;  // 有效速率
        }
        return;
    }

    // rate > 0: 先取消暂停，再设置 speed
    int paused = 0;
    mpv_set_property_async(m_mpv, 0, "pause", MPV_FORMAT_FLAG, &paused);

    mpv_set_property_async(m_mpv, 0, "speed", MPV_FORMAT_DOUBLE, &rate);

    // 本地更新（立即生效，之后 PROPERTY_CHANGE 会再确认）
    {
        std::lock_guard<std::mutex> lk(m_infoMx);
        m_info.paused = false;
        m_info.rate = rate;   // 有效速率
    }

    // 保存 nominal speed
    m_speedRaw.store(rate, std::memory_order_release);
}

void WMPVPlayer::SetFillMode(FillMode m)
{
    m_fill = m;
    // 若已创建 mpv core，在播放器线程里应用；此处只唤醒
    if (m_evWake) {
        SDL_Event ev{}; ev.type = m_evWake;
        SDL_PushEvent(&ev);
    }
}

bool WMPVPlayer::GetPlaybackInfo(PlaybackInfo& out) const
{
    std::lock_guard<std::mutex> lk(m_infoMx);
    out = m_info;
    return true;
}

void WMPVPlayer::threadFunc()
{
    // 不要让 SDL 接管信号，避免和 mpv/你的程序冲突
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        std::fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return;
    }

    // SDL 自定义事件
    m_evRenderUpdate = SDL_RegisterEvents(1);
    m_evMpvEvents = SDL_RegisterEvents(1);
    m_evWake = SDL_RegisterEvents(1);
    if (m_evRenderUpdate == (Uint32)-1 || m_evMpvEvents == (Uint32)-1 || m_evWake == (Uint32)-1) {
        std::fprintf(stderr, "SDL_RegisterEvents failed\n");
        return;
    }

    if (!createMpv()) {
        destroyMpv();
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_TIMER);
        return;
    }

    if (!createWindow(m_title, m_defaultW, m_defaultH)) {
        destroyWindow();
        destroyMpv();
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_TIMER);
        return;
    }

	applyFillMode();

    //const char* cmd[] = { "loadfile", m_url.c_str(), nullptr };
    //mpv_command_async(m_mpv, 0, cmd);

    //if (m_startSeconds > 0.0) {
    //    double pos = m_startSeconds;
    //    mpv_set_property_async(m_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    //}

    if (m_startSeconds > 0.0) {
        char start_opt[64];
        // 按 mpv 语法来，支持小数即可
        snprintf(start_opt, sizeof(start_opt), "start=%f", m_startSeconds);

        const char* cmd[] = {
            "loadfile",
            m_url.c_str(),
            "replace",   // flags
            "-1",        // index: -1 表示不特别指定，只是占位，兼容 0.38+ 的签名
            start_opt,   // per-file options：这里就带上 start
            nullptr
        };
        mpv_command_async(m_mpv, 0, cmd);
    }
    else {
        const char* cmd[] = {
            "loadfile",
            m_url.c_str(),
            nullptr
        };
        mpv_command_async(m_mpv, 0, cmd);
    }

    while (!m_quit.load()) {
        SDL_Event e{};
        if (SDL_WaitEvent(&e) != 1) {
            std::fprintf(stderr, "SDL_WaitEvent error\n");
            break;
        }

        bool need_redraw = false;

        switch (e.type) {
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_EXPOSED ||
                e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                need_redraw = true;

            if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
                if (IsEventForWindow(e, m_win)) {
                    m_quit = true;
                    
                    std::thread([this] { 
                        TogglePause();
                        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                        if (m_onDisconnect) {
                            m_onDisconnect();
                        }
                    }).detach();

                }
            }
            break;
        case SDL_KEYDOWN:
            if (e.key.keysym.sym == SDLK_SPACE) {
                TogglePause();
            }
            else if (e.key.keysym.sym == SDLK_RIGHT) {
                SeekRelative(+5.0);
            }
            else if (e.key.keysym.sym == SDLK_LEFT) {
                SeekRelative(-5.0);
            }
            else if (e.key.keysym.sym == SDLK_UP) {
                AddVolume(+5);
            }
            else if (e.key.keysym.sym == SDLK_DOWN) {
                AddVolume(-5);
            }
            break;
        default:
            if (e.type == m_evWake) {
                // 外部唤醒：可能是 Stop() 或 SetFillMode()
                if (m_mpv && m_renderReady) 
                    applyFillMode();
            }
            // 普通事件：处理 mpv 事件队列
            if (e.type == m_evMpvEvents) {
                handleMpvEvents();
            }
            // 渲染更新：需要调用 update 拉取 flags
            if (e.type == m_evRenderUpdate) {
                if (m_renderReady) {
                    uint64_t flags = mpv_render_context_update(m_mpvGL);
                    if (flags & MPV_RENDER_UPDATE_FRAME)
                        need_redraw = true;
                }
            }
 
            break;
        }

        if (need_redraw && m_renderReady) {
            renderFrame();
        }
    }

    destroyWindow();
    destroyMpv();
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_TIMER);
}

bool WMPVPlayer::createWindow(const std::string& title, int w, int h)
{
    m_win = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!m_win) {
        std::fprintf(stderr, "CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    m_gl = SDL_GL_CreateContext(m_win);
    if (!m_gl) {
        std::fprintf(stderr, "Create GL context failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(m_win, m_gl);
    SDL_GL_SetSwapInterval(0); // 关 vsync（你也可设 1）

    // Render API：绑定到当前 GL 上下文
    mpv_opengl_init_params gl_init{};
    gl_init.get_proc_address = &WMPVPlayer::getProcAddr;

    int adv = 1; // 使用高级控制（必须 async）
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE,               (void*)MPV_RENDER_API_TYPE_OPENGL },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS,     &gl_init },
        { MPV_RENDER_PARAM_ADVANCED_CONTROL,       &adv },
        { MPV_RENDER_PARAM_INVALID,                nullptr }
    };

    if (mpv_render_context_create(&m_mpvGL, m_mpv, params) < 0) {
        std::fprintf(stderr, "mpv_render_context_create failed\n");
        return false;
    }

    // 渲染更新回调：只发 SDL 事件
    mpv_render_context_set_update_callback(m_mpvGL, &WMPVPlayer::onMpvRender, this);

    m_renderReady = true;
    return true;
}

void WMPVPlayer::destroyWindow()
{
    if (m_mpvGL) {
        mpv_render_context_free(m_mpvGL);
        m_mpvGL = nullptr;
    }

    if (m_gl) {
        SDL_GL_DeleteContext(m_gl);
        m_gl = nullptr;
    }
    if (m_win) {
        SDL_DestroyWindow(m_win);
        m_win = nullptr;
    }

	m_renderReady = false;
}

void WMPVPlayer::renderFrame()
{
    if (!m_renderReady) 
        return;

    // 必须保证当前 GL 上下文是我们的
    SDL_GL_MakeCurrent(m_win, m_gl);

    // 用 DPI 下的实际可绘制像素尺寸
    int w = 0, h = 0;
#if SDL_VERSION_ATLEAST(2,26,0)
    SDL_GL_GetDrawableSize(m_win, &w, &h);
#else
    SDL_GetWindowSize(m_win, &w, &h);
#endif

    mpv_opengl_fbo fbo{};
    fbo.fbo = 0; // 默认帧缓冲
    fbo.w = w;
    fbo.h = h;

    int flip = 1;
    mpv_render_param rp[] = {
        { MPV_RENDER_PARAM_OPENGL_FBO, &fbo },
        { MPV_RENDER_PARAM_FLIP_Y,     &flip },
        { MPV_RENDER_PARAM_INVALID,    nullptr }
    };
    mpv_render_context_render(m_mpvGL, rp);
    SDL_GL_SwapWindow(m_win);
}

bool WMPVPlayer::IsEventForWindow(const SDL_Event& e, SDL_Window* window)
{
    return e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP
        ? e.motion.windowID == SDL_GetWindowID(window)
        : e.window.windowID == SDL_GetWindowID(window);
}

bool WMPVPlayer::createMpv()
{
    m_mpv = mpv_create();
    if (!m_mpv) { std::fprintf(stderr, "mpv_create failed\n"); return false; }

    // 强制 Render API 模式，不自建窗口
    mpv_set_option_string(m_mpv, "config", "no");
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    // 可选优化
    mpv_set_option_string(m_mpv, "hwdec", "auto-safe");
    mpv_set_option_string(m_mpv, "osc", "no");
    mpv_set_option_string(m_mpv, "user-agent", "AppleCoreMedia/1.0");
    mpv_set_option_string(m_mpv, "cache", "yes");
    mpv_set_option_string(m_mpv, "force-seekable", "yes");

    if (mpv_initialize(m_mpv) < 0) {
        std::fprintf(stderr, "mpv_initialize failed\n");
        return false;
    }

    // 绑定事件唤醒（线程安全，只发 SDL 事件）
    mpv_set_wakeup_callback(m_mpv, &WMPVPlayer::onMpvEvents, this);

    mpv_observe_property(m_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "speed", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "seekable", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "dwidth", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 0, "dheight", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 0, "video-codec", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, 0, "audio-codec", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, 0, "cache-buffering-state", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 0, "cache-buffering-percent", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 0, "demuxer-cache-state", MPV_FORMAT_NODE);

    return true;
}

void WMPVPlayer::destroyMpv()
{
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
}

void WMPVPlayer::handleMpvEvents()
{
    if (!m_mpv) return;
    while (1) {
        mpv_event* ev = mpv_wait_event(m_mpv, 0.0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;

        // 这里可以根据需要处理更多事件
        switch (ev->event_id) {
        case MPV_EVENT_FILE_LOADED:
            // 初始化一次快照
            {
                std::lock_guard<std::mutex> lk(m_infoMx);

                file_loaded.store(true);

                if (have_pending_seek.exchange(false)) {
                    double pos = pending_seek_pos.load();
                    mpv_set_property_async(m_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE, &pos);
                }

                int seekable = 0, paused = 0;
                double pos = 0.0, dur = 0.0, speed = 1.0;
                int64_t dw = 0, dh = 0;
                mpv_get_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
                mpv_get_property(m_mpv, "duration", MPV_FORMAT_DOUBLE, &dur);
                mpv_get_property(m_mpv, "seekable", MPV_FORMAT_FLAG, &seekable);
                mpv_get_property(m_mpv, "pause", MPV_FORMAT_FLAG, &paused);
                mpv_get_property(m_mpv, "speed", MPV_FORMAT_DOUBLE, &speed);
                mpv_get_property(m_mpv, "dwidth", MPV_FORMAT_INT64, &dw);
                mpv_get_property(m_mpv, "dheight", MPV_FORMAT_INT64, &dh);
                m_speedRaw.store(speed, std::memory_order_release);
                m_info.position = pos;
                m_info.duration = dur;
                m_info.seekable = !!seekable;
                m_info.paused = !!paused;
                m_info.rate = speed;
                m_info.width = (int)dw;
                m_info.height = (int)dh;
            }
            break;
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto* p = (mpv_event_property*)ev->data;
            if (!p || !p->name) break;

            std::lock_guard<std::mutex> lk(m_infoMx);
            if (std::strcmp(p->name, "time-pos") == 0 && p->format == MPV_FORMAT_DOUBLE) {
                m_info.position = *(double*)p->data;
            }
            else if (std::strcmp(p->name, "duration") == 0 && p->format == MPV_FORMAT_DOUBLE) {
                m_info.duration = *(double*)p->data;
            }
            else if (std::strcmp(p->name, "pause") == 0 && p->format == MPV_FORMAT_FLAG) {
                bool paused = !!*(int*)p->data;
                m_info.paused = paused;
                double raw = m_speedRaw.load(std::memory_order_acquire);
                m_info.rate = paused ? 0.0 : raw;        // 统一用有效速率
            }
            else if (std::strcmp(p->name, "speed") == 0 && p->format == MPV_FORMAT_DOUBLE) {
                double raw = *(double*)p->data;
                m_speedRaw.store(raw, std::memory_order_release);
                m_info.rate = m_info.paused ? 0.0 : raw; // 有效速率
            }
            else if (std::strcmp(p->name, "seekable") == 0 && p->format == MPV_FORMAT_FLAG) {
                m_info.seekable = !!*(int*)p->data;
            }
            else if (std::strcmp(p->name, "dwidth") == 0 && p->format == MPV_FORMAT_INT64) {
                m_info.width = (int)*(int64_t*)p->data;
            }
            else if (std::strcmp(p->name, "dheight") == 0 && p->format == MPV_FORMAT_INT64) {
                m_info.height = (int)*(int64_t*)p->data;
            }
            else if (std::strcmp(p->name, "video-codec") == 0 && p->format == MPV_FORMAT_STRING) {
                m_info.vcodec = (const char*)p->data;
            }
            else if (std::strcmp(p->name, "audio-codec") == 0 && p->format == MPV_FORMAT_STRING) {
                m_info.acodec = (const char*)p->data;
            }
            else if (std::strcmp(p->name, "cache-buffering-state") == 0 && p->format == MPV_FORMAT_INT64) {
                m_info.buffering_state = (int)*(int64_t*)p->data; // 0/1
            }
            else if (std::strcmp(p->name, "cache-buffering-percent") == 0 && p->format == MPV_FORMAT_INT64) {
                m_info.buffering_percent = (int)*(int64_t*)p->data;
            }
            else if (std::strcmp(p->name, "demuxer-cache-state") == 0 && p->format == MPV_FORMAT_NODE) {
                // 解析 node map
                auto* n = (mpv_node*)p->data;
                if (n->format == MPV_FORMAT_NODE_MAP) {
                    for (int i = 0; i < n->u.list->num; ++i) {
                        const char* key = n->u.list->keys[i];
                        mpv_node* val = n->u.list->values + i;
                        if (!key) continue;
                        if (std::strcmp(key, "cache-duration") == 0 && val->format == MPV_FORMAT_DOUBLE) {
                            m_info.cache_duration = val->u.double_;
                        }
                        else if (std::strcmp(key, "fw-bytes") == 0 && val->format == MPV_FORMAT_INT64) {
                            m_info.fw_bytes = (double)val->u.int64;
                        }
                        else if (std::strcmp(key, "bw-bytes") == 0 && val->format == MPV_FORMAT_INT64) {
                            m_info.bw_bytes = (double)val->u.int64;
                        }
                        // 还可以解析 seekable-ranges 等，按需扩展
                    }
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

void WMPVPlayer::applyFillMode()
{
    if (!m_mpv) return;
    switch (m_fill) {
    case FillMode::Contain:
        mpv_set_property_string(m_mpv, "keepaspect", "yes");
        mpv_set_property_string(m_mpv, "panscan", "0");
        break;
    case FillMode::Cover:
        mpv_set_property_string(m_mpv, "keepaspect", "yes");
        mpv_set_property_string(m_mpv, "panscan", "1.0");
        break;
    case FillMode::Stretch:
        mpv_set_property_string(m_mpv, "panscan", "0");
        mpv_set_property_string(m_mpv, "keepaspect", "no");
        break;
    }
}

// --- mpv callbacks ---

void WMPVPlayer::onMpvEvents(void* ctx) {
    auto* self = static_cast<WMPVPlayer*>(ctx);
    if (!self->m_evMpvEvents)
        return;
    SDL_Event ev;
    ev.type = self->m_evMpvEvents;
    SDL_PushEvent(&ev);
}

void WMPVPlayer::onMpvRender(void* ctx) {
    auto* self = static_cast<WMPVPlayer*>(ctx);
    if (!self->m_evRenderUpdate)
        return;
    SDL_Event ev;
    ev.type = self->m_evRenderUpdate;
    SDL_PushEvent(&ev);
}

void* WMPVPlayer::getProcAddr(void*, const char* name) {
    return SDL_GL_GetProcAddress(name);
}

}  // namespace wmediakits