#ifndef WMEDIAKITS_WMPVPLAYER_H
#define WMEDIAKITS_WMPVPLAYER_H

#include <string>
#include <atomic>
#include <functional>
#include <thread>
#include <mutex>

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

    struct PlaybackInfo {
        double position = 0.0;   // 当前秒
        double duration = 0.0;   // 总时长（0 代表 live/未知）
        double rate = 1.0;   // 播放速度
        bool   paused = false; // 是否暂停
        bool   seekable = false; // 是否可定位
        int    width = 0;     // 可视宽（像素）
        int    height = 0;     // 可视高（像素）
        // 缓存/缓冲相关（可选项，尽量给出）
        int    buffering_state = 0;   // 0=无缓冲, 1=缓冲中（mpv 的 cache-buffering-state）
        int    buffering_percent = 0;   // 缓冲百分比
        double cache_duration = 0.0; // 前向缓存时长（秒）（demuxer-cache-state.cache-duration）
        double bw_bytes = 0.0; // 后向缓存字节数
        double fw_bytes = 0.0; // 前向缓存字节数
        std::string vcodec;             // 视频编码名
        std::string acodec;             // 音频编码名

        bool is_live() const { return duration <= 0.0 && !seekable; }
    };

    WMPVPlayer();
    ~WMPVPlayer();

    bool Init(const std::string& title = "WMPVPlayer",
              int default_w = 1280, int default_h = 720,
              FillMode defaultFill = FillMode::Contain);

    // 开始播放一个 HLS（或任意 url）地址 
    // startSeconds > 0 将从指定秒数开始
    bool Play(const std::string& url, double startSeconds = 0.0);
    void Stop();

    void RegisterOnDisconnect(OnDisconnect handler);

    // 控制（可在运行时随时调用）
    void TogglePause();            // 空格同款
    void SeekRelative(double sec); // 正=快进，负=快退
	void SeekTo(double sec);       // 直接跳转到指定秒数
    void AddVolume(int delta);     // +-音量
    void SetRate(double rate);     // 倍速
    void SetFillMode(FillMode m);  // 画面填充策略

    bool GetPlaybackInfo(PlaybackInfo& out) const;

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

    // 播放信息快照（任意线程读，播放器线程写）
    mutable std::mutex m_infoMx;
    PlaybackInfo       m_info;

    // 保存 mpv 的 nominal speed（不考虑 pause）
    std::atomic<double> m_speedRaw{ 1.0 };

	// 处理B站这种延迟seek的情况，onPlay时还没有播放，后续紧跟了seek请求
    std::atomic<bool> file_loaded{ false };
    std::atomic<bool> have_pending_seek{ false };
    std::atomic<double> pending_seek_pos{ 0.0 };
};

}  // namespace wmediakits

#endif // WMEDIAKITS_WMPVPLAYER_H
