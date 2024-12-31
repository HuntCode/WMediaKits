#ifndef WMEDIAKITS_DUMPFILE_H_
#define WMEDIAKITS_DUMPFILE_H_

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
}

/*
 *  Video Functions 
 */
int WWriteVideoFrame(const AVFrame* frame, const char* filename);

/* 
 *  Audio Functions
 */
int WWriteAudioFrame(const AVFrame* frame, const char* filename);

#endif // WMEDIAKITS_DUMPFILE_H_