#include "WDumpFile.h"
#include "WUtils.h"

#include <vector>

/*
 *  Video Functions
 */
int WWriteVideoFrame(const AVFrame* frame, const char* filename) {
    FILE* file = fopen(filename, "ab");
    if (!file) {
        perror("fopen");
        return -1;
    }

    int width = frame->width;
    int height = frame->height;
    enum AVPixelFormat pix_fmt = (AVPixelFormat)frame->format;

    // TODO: rescale if fmt is not AV_PIX_FMT_YUV420P

    // assume AV_PIX_FMT_YUV420P
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