#ifndef WMEDIAKITS_WMPVPLAYER_H
#define WMEDIAKITS_WMPVPLAYER_H

#include <string>
#include <atomic>
#include <functional>
#include <thread>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

struct mpv_handle;
struct mpv_render_context;

namespace wmediakits {

class WMPVPlayer {
    typedef std::function<void()> OnDisconnect;
public:
    enum class FillMode { 
        Contain, 
        Cover, 
        Stretch 
    }; // 等比/裁剪铺满/拉伸铺满

    WMPVPlayer();
    ~WMPVPlayer();

    // 初始化窗口 + OpenGL + mpv（还不开始播放）
    // title: 窗口标题  w/h: 初始窗口大小（逻辑点）
    bool Init(const std::string& title = "WMPVPlayer",
              int default_w = 1280, int default_h = 720,
              FillMode defaultFill = FillMode::Contain);

    // 开始播放一个 HLS（或任意 url）地址；startSeconds > 0 将从指定秒数开始
    bool Play(const std::string& url, double startSeconds = 0.0);

    // 停止播放并释放 mpv/GL/window
    void Stop();

    void RegisterOnDisconnect(OnDisconnect handler);

    // 控制（可在运行时随时调用）
    void TogglePause();            // 空格同款
    void Seek(double sec); // 正=快进，负=快退
    void AddVolume(int delta);     // +-音量
    void SetRate(double rate);     // 倍速
    void SetFillMode(FillMode m);  // 画面填充策略

private:
    // 线程方法
	void threadFunc();

    // SDL/GL
    bool createWindow(const std::string& title, int w, int h);
    void destroyWindow();
    void renderFrame(); // 在当前 GL 上下文绘制一帧
    bool IsEventForWindow(const SDL_Event& e, SDL_Window* window);

    // mpv
    bool createMpv();
    void destroyMpv();
    void handleMpvEvents();          // 拉取并处理 mpv 事件
    void applyFillMode();

    static void onMpvEvents(void*);  // mpv 唤醒
    static void onMpvRender(void*);  // mpv 渲染更新
    static void* getProcAddr(void*, const char* name);

private:
    // SDL
    SDL_Window* m_win = nullptr;
    SDL_GLContext m_gl = nullptr;
    Uint32        m_evRenderUpdate = 0; // SDL 自定义事件：提示需要调用 mpv_render_context_update
    Uint32        m_evMpvEvents = 0; // SDL 自定义事件：提示有 mpv 普通事件
    Uint32        m_evWake = 0; // 外部 Stop/控制时唤醒
    
	// 线程控制
    std::thread      m_thread;
    std::atomic<bool> m_running{ false }; // 线程是否在跑
    std::atomic<bool> m_quit{ false };

    // mpv
    mpv_handle* m_mpv = nullptr;
    mpv_render_context* m_mpvGL = nullptr;
    bool                m_renderReady = false; // renderContext 是否已创建

    // 配置/状态
	std::string m_title{ "WMPVPlayer" };
    int         m_defaultW{ 1280 };
    int         m_defaultH{ 720 };
    FillMode    m_fill = FillMode::Contain;

    // 播放参数
    std::string m_url;
    double      m_startSeconds{ 0.0 };

	// 回调
    OnDisconnect m_onDisconnect;
};

}  // namespace wmediakits

#endif // WMEDIAKITS_WMPVPLAYER_H
