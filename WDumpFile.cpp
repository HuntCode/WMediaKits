#include "WDumpFile.h"

#include <vector>

/*
 *  Video Functions
 */
int WWriteVideoFrame(const AVFrame* frame, const char* filename) {
    // 打开文件用于写入
    FILE* file = fopen(filename, "ab");
    if (!file) {
        perror("fopen");
        return -1;
    }

    // 获取帧的宽度、高度和像素格式
    int width = frame->width;
    int height = frame->height;
    enum AVPixelFormat pix_fmt = (AVPixelFormat)frame->format;

    // 确保是 YUV420p 格式
    if (pix_fmt != AV_PIX_FMT_YUV420P) {
        fprintf(stderr, "This function currently only supports YUV420p format.\n");
        fclose(file);
        return -1;
    }

    // 写入 YUV 数据
    // Y plane
    fwrite(frame->data[0], 1, width * height, file);
    // U plane
    fwrite(frame->data[1], 1, width * height / 4, file);
    // V plane
    fwrite(frame->data[2], 1, width * height / 4, file);

    fclose(file);
    return 0;
}

/*
 *  Audio Functions
 */
void InterleaveAudioSamples(const AVFrame* frame, std::vector<uint8_t>& interleaved_audio_buffer) {
    SwrContext* swrCtx = swr_alloc();
    swr_alloc_set_opts2(&swrCtx, &frame->ch_layout, AV_SAMPLE_FMT_FLT, frame->sample_rate,
                        &frame->ch_layout, (AVSampleFormat)frame->format, frame->sample_rate, 0, NULL);

    swr_init(swrCtx);

    int outSamples = av_rescale_rnd(swr_get_delay(swrCtx, frame->sample_rate) + frame->nb_samples, frame->sample_rate, frame->sample_rate, AV_ROUND_UP);
    uint8_t* output = nullptr;
    av_samples_alloc(&output, nullptr, frame->ch_layout.nb_channels, outSamples, AV_SAMPLE_FMT_FLT, 0);

    int convertedSamples = swr_convert(swrCtx, &output, outSamples, (const uint8_t**)frame->data, frame->nb_samples);
    int dataSize = av_samples_get_buffer_size(nullptr, frame->ch_layout.nb_channels, convertedSamples, AV_SAMPLE_FMT_FLT, 1);
    interleaved_audio_buffer.resize(dataSize);
    memcpy(interleaved_audio_buffer.data(), output, dataSize);

    av_freep(&output);
    swr_free(&swrCtx);
}

int WWriteAudioFrame(const AVFrame* frame, const char* filename) {
    // 打开文件用于写入
    FILE* file = fopen(filename, "ab");
    if (!file) {
        perror("fopen");
        return -1;
    }

    // 获取音频的帧大小、声道数、采样格式
    int channels = frame->ch_layout.nb_channels;
    int sample_size = av_get_bytes_per_sample((AVSampleFormat)frame->format);  // 每个样本的字节数
    int frame_size = frame->nb_samples;

    if (av_sample_fmt_is_planar((AVSampleFormat)frame->format)) {
        std::vector<uint8_t> interleaved_audio_buffer;
        InterleaveAudioSamples(frame, interleaved_audio_buffer);
        fwrite(interleaved_audio_buffer.data(), sample_size, frame_size * channels, file);
    }
    else {
        fwrite(frame->data[0], sample_size, frame_size * channels, file);
    }

    fclose(file);
    return 0;
}