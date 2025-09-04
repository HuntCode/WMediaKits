#include <cmath>
#include <cstdio>

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


void WMPVPlayer::RegisterOnDisconnect(OnDisconnect handler)
{
    m_onDisconnect = handler;
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

    const char* cmd[] = { "loadfile", m_url.c_str(), nullptr };
    mpv_command_async(m_mpv, 0, cmd);

    if (m_startSeconds > 0.0) {
        double pos = m_startSeconds;
        mpv_set_property_async(m_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    }

    while (!m_quit.load()) {
        SDL_Event e{};
        if (SDL_WaitEvent(&e) != 1) {
            std::fprintf(stderr, "SDL_WaitEvent error\n");
            break;
        }

        bool need_redraw = false;

        switch (e.type) {
        case SDL_QUIT:
            m_quit.store(true);
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_EXPOSED ||
                e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                need_redraw = true;

            if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
                if (IsEventForWindow(e, m_win)) {
                    m_quit = true;
                    std::thread([this] { 
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
                Seek(+5.0);
            }
            else if (e.key.keysym.sym == SDLK_LEFT) {
                Seek(-5.0);
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
            // 例如：读取时长/轨道信息等
            break;
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

// --- controls ---

void WMPVPlayer::TogglePause()
{
    if (!m_mpv) return;
    const char* cmd[] = { "cycle", "pause", nullptr };
    mpv_command_async(m_mpv, 0, cmd);
}

void WMPVPlayer::Seek(double sec)
{
    if (!m_mpv) return;
    char buf[64]; std::snprintf(buf, sizeof(buf), "%.3f", sec);
    const char* cmd[] = { "seek", buf, "relative", "+exact", nullptr };
    mpv_command_async(m_mpv, 0, cmd);
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
    mpv_node n{}; n.format = MPV_FORMAT_DOUBLE; n.u.double_ = rate;
    mpv_set_property_async(m_mpv, 0, "speed", MPV_FORMAT_NODE, &n);
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

// --- mpv callbacks ---

void* WMPVPlayer::getProcAddr(void*, const char* name) {
    return SDL_GL_GetProcAddress(name);
}

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

}  // namespace wmediakits