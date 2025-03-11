#include "WSDLPlayer.h"
#include "WBigEndian.h"
#include "WUtils.h"

namespace wmediakits {

constexpr SDL_AudioFormat kSDLAudioFormatUnknown = 0;

// custom event type
enum {
    SDL_EVENT_CREATE_WINDOW = SDL_USEREVENT
};

std::atomic<int> WSDLPlayer::s_instanceCount = 0;

WSDLPlayer::WSDLPlayer(std::shared_ptr<ISDLEventHandler> eventHandler)
    : m_audioDevice(0), m_window(nullptr), m_renderer(nullptr), m_texture(nullptr),
      m_videoWidth(0), m_videoHeight(0), m_eventHandler(eventHandler), m_onDisconnect(nullptr)
{
    {
        std::lock_guard<std::mutex> lock(m_initMutex);
        if (s_instanceCount == 0) {
            SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
            if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
                std::cerr << "SDL initialization failed: " << SDL_GetError() << std::endl;
                return;
            }
        }
        ++s_instanceCount;
    }
}

WSDLPlayer::~WSDLPlayer()
{
    Stop();
    SDL_DestroyTexture(m_texture);
    SDL_DestroyRenderer(m_renderer);
    SDL_DestroyWindow(m_window);
    SDL_CloseAudioDevice(m_audioDevice);

    {
        std::lock_guard<std::mutex> lock(m_initMutex);
        if (--s_instanceCount == 0) {
            //SDL_Quit();
        }
    }
}

void WSDLPlayer::Init(const std::string& name, const std::string& acodec_name, const std::string& vcodec_name)
{
    m_winName = name;

    InitDecoder(acodec_name, vcodec_name);

    m_audioThread = std::thread(&WSDLPlayer::AudioThreadFunc, this);
    m_videoThread = std::thread(&WSDLPlayer::VideoThreadFunc, this);
}

void WSDLPlayer::Play()
{
    m_quit = false;
    // SDL main loop
    while (!m_quit) {
        HandleEvents();
        HandleCustomEvents();
        Render();
        SDL_Delay(10); // Delay to reduce CPU usage
    }
}

void WSDLPlayer::Stop()
{
    m_quit = true;
    m_audioCV.notify_all();
    m_videoCV.notify_all();

    if (m_audioThread.joinable()) m_audioThread.join();
    if (m_videoThread.joinable()) m_videoThread.join();
}

void WSDLPlayer::ProcessVideo(uint8_t* buffer, int bufSize)
{
    {
        std::lock_guard<std::mutex> lock(m_videoMutex);
        m_videoQueue.push(std::vector<uint8_t>(buffer, buffer + bufSize));
    }
    m_videoCV.notify_one();
}

void WSDLPlayer::ProcessAudio(uint8_t* buffer, int bufSize)
{
    {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        m_audioQueue.push(std::vector<uint8_t>(buffer, buffer + bufSize));
    }
    m_audioCV.notify_one();
}

void WSDLPlayer::RegisterOnDisconnect(OnDisconnect handler)
{
    m_onDisconnect = handler;
}

void WSDLPlayer::InitDecoder(const std::string& acodec_name, const std::string& vcodec_name)
{
    m_audioDecoder.SetCodecName(acodec_name); //"opus"
    m_audioDecoder.SetClient(this);

    m_videoDecoder.SetCodecName(vcodec_name); //"vp8"
    m_videoDecoder.SetClient(this);
}

void WSDLPlayer::VideoThreadFunc()
{
    while (!m_quit) {
        std::vector<uint8_t> data;
        {
            std::unique_lock<std::mutex> lock(m_videoMutex);
            m_videoCV.wait(lock, [this] { return !m_videoQueue.empty() || m_quit; });
            if (!m_videoQueue.empty()) {
                data = std::move(m_videoQueue.front());
                m_videoQueue.pop();
            }
        }

        if (!data.empty()) {
            m_videoDecoder.Decode(const_cast<uint8_t*>(data.data()), data.size());
        }
    }
}

void WSDLPlayer::AudioThreadFunc()
{
    while (!m_quit) {
        std::vector<uint8_t> data;
        {
            std::unique_lock<std::mutex> lock(m_audioMutex);
            m_audioCV.wait(lock, [this] { return !m_audioQueue.empty() || m_quit; });

            if (!m_audioQueue.empty()) {
                data = std::move(m_audioQueue.front());
                m_audioQueue.pop();
            }
        }

        if (!data.empty()) {
            m_audioDecoder.Decode(const_cast<uint8_t*>(data.data()), data.size());
        }
    }
}

void WSDLPlayer::Render()
{
    std::lock_guard<std::mutex> lock(m_renderMutex);

    if (!m_renderQueue.empty()) {
        AVFrame* frame = m_renderQueue.front();
        m_renderQueue.pop();

        SDL_UpdateYUVTexture(m_texture, nullptr,
            frame->data[0], frame->linesize[0],
            frame->data[1], frame->linesize[1],
            frame->data[2], frame->linesize[2]);

        SDL_RenderClear(m_renderer);
        SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);
        SDL_RenderPresent(m_renderer);

        av_frame_free(&frame);
    }
}

void WSDLPlayer::ClearQueue(std::queue<AVFrame*>& queue)
{
    while (!queue.empty()) {
        AVFrame* frame = queue.front();
        queue.pop();
        av_frame_free(&frame);
    }
}

void WSDLPlayer::CreateWindowAndRenderer(int width, int height)
{
    if(m_texture != nullptr)
        SDL_DestroyTexture(m_texture);
    if(m_renderer != nullptr)
        SDL_DestroyRenderer(m_renderer);
    if(m_window != nullptr)
        SDL_DestroyWindow(m_window);

    m_window = SDL_CreateWindow(m_winName.c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        width, height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!m_window) {
        std::cerr << "Failed to create window: " << SDL_GetError() << std::endl;
        return;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED);
    if (!m_renderer) {
        std::cerr << "Failed to create renderer: " << SDL_GetError() << std::endl;
        return;
    }

    m_texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
        width, height);
    if (!m_texture) {
        std::cerr << "Failed to create texture: " << SDL_GetError() << std::endl;
        return;
    }
}

void WSDLPlayer::PushCustomEvent(CreateWindowEvent event)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_customEventQueue.push(event);
}

void WSDLPlayer::HandleCustomEvents()
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    while (!m_customEventQueue.empty()) {
        CreateWindowEvent event = m_customEventQueue.front();
        m_customEventQueue.pop();

        CreateWindowAndRenderer(m_videoWidth, m_videoHeight); 
    }
}

bool WSDLPlayer::IsEventForWindow(const SDL_Event& e, SDL_Window* window)
{
    return e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP
        ? e.motion.windowID == SDL_GetWindowID(window)
        : e.window.windowID == SDL_GetWindowID(window);
}

void WSDLPlayer::HandleEvents()
{
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        //std::cout << "event.type " << event.type << std::endl;
        switch (event.type) {
        case SDL_QUIT:
            m_quit = true;
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                m_quit = true;
                if (m_onDisconnect) {
                    m_onDisconnect();
                }
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (m_eventHandler) {
                m_eventHandler->OnMouseDown(event.button);
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (m_eventHandler) {
                m_eventHandler->OnMouseUp(event.button);
            }
            break;
        case SDL_MOUSEMOTION:
            if(IsEventForWindow(event, m_window))
                if (m_eventHandler) {
                    m_eventHandler->OnMouseMove(event.motion);
                }
            break;
        case SDL_MOUSEWHEEL:
            if (m_eventHandler) {
                m_eventHandler->OnMouseWheel(event.wheel);
            }
            break;
        case SDL_KEYDOWN:
            if (m_eventHandler) {
                m_eventHandler->OnKeyDown(event.key);
            }
            break;
        case SDL_KEYUP:
            if (m_eventHandler) {
                m_eventHandler->OnKeyUp(event.key);
            }
            break;
        default:
            break;
        }
    }
}

SDL_AudioFormat WSDLPlayer::GetSDLAudioFormat(AVSampleFormat format) {
    switch (format) {
    case AV_SAMPLE_FMT_U8P:
    case AV_SAMPLE_FMT_U8:
        return AUDIO_U8;

    case AV_SAMPLE_FMT_S16P:
    case AV_SAMPLE_FMT_S16:
        return IsBigEndianArchitecture() ? AUDIO_S16MSB : AUDIO_S16LSB;

    case AV_SAMPLE_FMT_S32P:
    case AV_SAMPLE_FMT_S32:
        return IsBigEndianArchitecture() ? AUDIO_S32MSB : AUDIO_S32LSB;

    case AV_SAMPLE_FMT_FLTP:
    case AV_SAMPLE_FMT_FLT:
        return IsBigEndianArchitecture() ? AUDIO_F32MSB : AUDIO_F32LSB;

    default:
        // Either NONE, or the 64-bit formats are unsupported.
        break;
    }

    return kSDLAudioFormatUnknown;
}

void WSDLPlayer::OnFrameDecoded(const AVFrame& frame)
{
    if (frame.width > 0 && frame.height > 0) { //video
        std::lock_guard<std::mutex> lock(m_renderMutex);
        AVFrame* cloneFrame = av_frame_clone(&frame);
        // create window
        if (frame.width != m_videoWidth || frame.height != m_videoHeight) {
            m_videoWidth = frame.width;
            m_videoHeight = frame.height;

            CreateWindowEvent event;
            event.w = m_videoWidth;
            event.h = m_videoHeight;
            PushCustomEvent(event);
            ClearQueue(m_renderQueue);
        }

        m_renderQueue.push(cloneFrame);
    }
    else { //audio
        if (m_audioDevice == 0) {
            int frame_channels =
#if _LIBAVUTIL_OLD_CHANNEL_LAYOUT
                frame.channels;
#else
                frame.ch_layout.nb_channels;
#endif  // _LIBAVUTIL_OLD_CHANNEL_LAYOUT

            // create audio device
            m_audioSpec.freq = frame.sample_rate;
            m_audioSpec.format = GetSDLAudioFormat(static_cast<AVSampleFormat>(frame.format));
            m_audioSpec.channels = frame_channels;

            constexpr auto kMinBufferDuration = std::chrono::milliseconds(20);
            constexpr auto kOneSecond = std::chrono::seconds(1);
            const auto required_samples = static_cast<int>(m_audioSpec.freq * kMinBufferDuration / kOneSecond);
            m_audioSpec.samples = 1 << av_log2(required_samples);
            if (m_audioSpec.samples < required_samples) {
                m_audioSpec.samples *= 2;
            }
            m_audioSpec.callback = nullptr;
            m_audioSpec.userdata = nullptr;

            m_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &m_audioSpec, nullptr, 0);
            if (m_audioDevice == 0) {
                std::cerr << "Failed to open audio device: " << SDL_GetError() << std::endl;
                return;
            }
            SDL_PauseAudioDevice(m_audioDevice, 0); // resume audio play
        }

        int channels = frame.ch_layout.nb_channels;
        int sample_size = av_get_bytes_per_sample((AVSampleFormat)frame.format);  // 每个样本的字节数
        int frame_size = frame.nb_samples;

        if (av_sample_fmt_is_planar((AVSampleFormat)frame.format)) {
            std::vector<uint8_t> interleaved_audio_buffer;
            InterleaveAudioSamples(&frame, interleaved_audio_buffer);
            SDL_QueueAudio(m_audioDevice, interleaved_audio_buffer.data(), interleaved_audio_buffer.size());
        }
        else {
            SDL_QueueAudio(m_audioDevice, frame.data[0], sample_size * frame_size * channels);
        }
    }
}

void WSDLPlayer::OnDecodeError(const std::string& message)
{
}

void WSDLPlayer::OnFatalError(const std::string& message)
{
}

}  // namespace wmediakits