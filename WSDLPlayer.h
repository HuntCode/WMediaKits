#ifndef WMEDIAKITS_SDLPLAYER_H
#define WMEDIAKITS_SDLPLAYER_H

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <iostream>
#include <vector>
#include <functional>

#include "WDecoder.h"

namespace wmediakits {

// custom event
struct CreateWindowEvent {
    //const char* title;
    //int x, y, w, h;
    int w;
    int h;
};

class ISDLEventHandler {
public:
    virtual ~ISDLEventHandler() = default;

    virtual void OnMouseDown(const SDL_MouseButtonEvent& buttonEvent) = 0;
    virtual void OnMouseUp(const SDL_MouseButtonEvent& buttonEvent) = 0;
    virtual void OnMouseMove(const SDL_MouseMotionEvent& motionEvent) = 0;
    virtual void OnMouseWheel(const SDL_MouseWheelEvent& wheelEvent) = 0;
    virtual void OnKeyDown(const SDL_KeyboardEvent& keyEvent) = 0;
    virtual void OnKeyUp(const SDL_KeyboardEvent& keyEvent) = 0;
};

class WSDLPlayer : public WDecoder::Client {
    typedef std::function<void()> OnDisconnect;

public:
    WSDLPlayer(std::shared_ptr<ISDLEventHandler> eventHandler);
    ~WSDLPlayer();

    void Init(const std::string & name);
    void Play();
    void Stop();

    void ProcessVideo(uint8_t* buffer, int bufSize);
    void ProcessAudio(uint8_t* buffer, int bufSize);

    void RegisterOnDisconnect(OnDisconnect handler);

private:
    void InitDecoder();
    void VideoThreadFunc();
    void AudioThreadFunc();
    void Render();

    void ClearQueue(std::queue<AVFrame*>& queue);

    void CreateWindowAndRenderer(int width, int height);

    void PushCustomEvent(CreateWindowEvent event);
    void HandleCustomEvents();

    bool IsEventForWindow(const SDL_Event& e, SDL_Window* window);
    void HandleEvents();

    static SDL_AudioFormat GetSDLAudioFormat(AVSampleFormat format);

    /* WDecoder::Client */
    void OnFrameDecoded(const AVFrame& frame) override;
    void OnDecodeError(const std::string& message) override;
    void OnFatalError(const std::string& message) override;

    static std::atomic<int> s_instanceCount;
    std::mutex m_initMutex;

    std::string m_winName;
    //MiracastDevice* m_device;
    int m_lastX = 0, m_lastY = 0;
    
    // audio
    SDL_AudioSpec m_audioSpec;
    SDL_AudioDeviceID m_audioDevice;

    std::condition_variable m_audioCV;
    std::mutex m_audioMutex;
    std::queue<std::vector<uint8_t>> m_audioQueue;

    // video
    SDL_Window* m_window;
    SDL_Renderer* m_renderer;
    SDL_Texture* m_texture;

    std::condition_variable m_videoCV;
    std::mutex m_videoMutex;
    std::queue<std::vector<uint8_t>> m_videoQueue;

    std::mutex m_renderMutex;
    std::queue<AVFrame*> m_renderQueue;

    std::atomic<bool> m_quit{ false };
    int m_videoWidth;
    int m_videoHeight;

    WDecoder m_audioDecoder;
    WDecoder m_videoDecoder;

    std::thread m_audioThread;
    std::thread m_videoThread;

    std::queue<CreateWindowEvent> m_customEventQueue;
    std::mutex m_queueMutex;

    std::shared_ptr<ISDLEventHandler> m_eventHandler;

    OnDisconnect m_onDisconnect;
};

}  // namespace wmediakits

#endif // WMEDIAKITS_SDLPLAYER_H