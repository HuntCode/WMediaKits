#ifndef WMEDIAKITS_UTILS_H
#define WMEDIAKITS_UTILS_H

#include <vector>

extern "C" {
#include <libswresample/swresample.h>
}

inline void InterleaveAudioSamples(const AVFrame* frame, std::vector<uint8_t>& interleaved_audio_buffer) {
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

#endif // WMEDIAKITS_UTILS_H