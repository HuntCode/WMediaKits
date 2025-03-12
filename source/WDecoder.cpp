#include "WDecoder.h"

#include <libavcodec/version.h>

#include <iostream>
#include <algorithm>
#include <sstream>
#include <thread>

namespace wmediakits {

namespace {
// The av_err2str macro uses a compound literal, which is a C99-only feature.
// So instead, we roll our own here.
// TODO(issuetracker.google.com/224642520): dedup with standalone
// sender.
std::string AvErrorToString(int error_num) {
  std::string out(AV_ERROR_MAX_STRING_SIZE, '\0');
  av_make_error_string((char*)data(out), out.length(), error_num);
  return out;
}
}  // namespace

WDecoder::Client::Client() = default;
WDecoder::Client::~Client() = default;

WDecoder::WDecoder() {
#if LIBAVCODEC_VERSION_MAJOR < 59
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  avcodec_register_all();
#pragma GCC diagnostic pop
#endif  // LIBAVCODEC_VERSION_MAJOR < 59
}

WDecoder::~WDecoder() {
    if (context_ && context_->extradata) {
        av_free(context_->extradata);
        context_->extradata = NULL;
    }
}

void WDecoder::SetCodecName(const std::string& codec_name) {
    codec_name_ = codec_name;
}

void WDecoder::Decode(unsigned char* data, int data_len) {
  if (!codec_ && !Initialize()) {
    return;
  }

  // Parse the buffer for the required metadata and the packet to send to the
  // decoder.
  const int bytes_consumed = av_parser_parse2(
      parser_.get(), context_.get(), &packet_->data, &packet_->size,
      data, data_len, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
  if (bytes_consumed < 0) {
    OnError("av_parser_parse2", bytes_consumed);
    return;
  }
  //if (!packet_->data) {
  //  OnError("av_parser_parse2 found no packet", AVERROR_BUFFER_TOO_SMALL);
  //  return;
  //}

  packet_->data = data;
  packet_->size = data_len;

  // Send the packet to the decoder.
  const int send_packet_result =
      avcodec_send_packet(context_.get(), packet_.get());
  if (send_packet_result < 0) {
    // The result should not be EAGAIN because this code always pulls out all
    // the decoded frames after feeding-in each AVPacket.
    OnError("avcodec_send_packet", send_packet_result);
    return;
  }

  // Receive zero or more frames from the decoder.
  for (;;) {
    const int receive_frame_result =
        avcodec_receive_frame(context_.get(), decoded_frame_.get());
    if (receive_frame_result == AVERROR(EAGAIN)) {
      break;  // Decoder needs more input to produce another frame.
    }

    if (receive_frame_result < 0) {
      OnError("avcodec_receive_frame", receive_frame_result);
      return;
    }
    if (client_) {
      client_->OnFrameDecoded(*decoded_frame_);
    }
    av_frame_unref(decoded_frame_.get());
  }
}

bool WDecoder::Initialize() {
    //av_log(NULL, AV_LOG_INFO, "FFmpeg configuration:\n%s\n", avcodec_configuration());

  // NOTE: The codec_name values found in OFFER messages, such as "vp8" or
  // "h264" or "opus" are valid input strings to FFMPEG's look-up function, so
  // no translation is required here.
  codec_ = avcodec_find_decoder_by_name(codec_name_.c_str());
  if (!codec_) {
    HandleInitializationError("codec not available", AVERROR(EINVAL));
    return false;
  }
  std::cout << "Found codec: " << codec_name_ << " (known to FFMPEG as "
               << avcodec_get_name(codec_->id) << ')' << std::endl;

  parser_ = MakeUniqueAVCodecParserContext(codec_->id);
  if (!parser_) {
    HandleInitializationError("failed to allocate parser context",
                              AVERROR(ENOMEM));
    return false;
  }

  context_ = MakeUniqueAVCodecContext(codec_);
  if (!context_) {
    HandleInitializationError("failed to allocate codec context",
                              AVERROR(ENOMEM));
    return false;
  }

  if (codec_name_ == "libfdk_aac") {
      uint8_t eld_conf[] = { 0xF8, 0xE8, 0x50, 0x00 };
      context_->extradata = (uint8_t*)av_malloc(sizeof(eld_conf));
      memcpy(context_->extradata, eld_conf, sizeof(eld_conf));
      context_->extradata_size = sizeof(eld_conf);
  }

  // This should always be greater than zero, so that decoding doesn't block the
  // main thread of this receiver app and cause playback timing issues. The
  // actual number should be tuned, based on the number of CPU cores.
  //
  // This should also be 16 or less, since the encoder implementations emit
  // warnings about too many encode threads. FFMPEG's VP8 implementation
  // actually silently freezes if this is 10 or more. Thus, 8 is used for the
  // max here, just to be safe.
  context_->thread_count =
      std::min(std::max<int>(std::thread::hardware_concurrency(), 1), 8);
  const int open_result = avcodec_open2(context_.get(), codec_, nullptr);
  if (open_result < 0) {
    HandleInitializationError("failed to open codec", open_result);
    return false;
  }

  packet_ = MakeUniqueAVPacket();
  if (!packet_) {
    HandleInitializationError("failed to allocate AVPacket", AVERROR(ENOMEM));
    return false;
  }

  decoded_frame_ = MakeUniqueAVFrame();
  if (!decoded_frame_) {
    HandleInitializationError("failed to allocate AVFrame", AVERROR(ENOMEM));
    return false;
  }

  return true;
}

void WDecoder::HandleInitializationError(const char* what, int av_errnum) {
  // If the codec was found, get FFMPEG's canonical name for it.
  const char* const canonical_name =
      codec_ ? avcodec_get_name(codec_->id) : nullptr;

  codec_ = nullptr;  // Set null to mean "not initialized."

  if (!client_) {
    return;  // Nowhere to emit error to, so don't bother.
  }

  std::ostringstream error;
  error << "Could not initialize codec " << codec_name_;
  if (canonical_name) {
    error << " (known to FFMPEG as " << canonical_name << ')';
  }

  error << " because " << what << " (" << AvErrorToString(av_errnum) << ").";
  client_->OnFatalError(error.str());
}

void WDecoder::OnError(const char* what, int av_errnum) {
  if (!client_) {
    return;
  }

  // Make a human-readable string from the libavcodec error.
  std::ostringstream error;

  char human_readable_error[AV_ERROR_MAX_STRING_SIZE]{0};
  av_make_error_string(human_readable_error, AV_ERROR_MAX_STRING_SIZE,
                       av_errnum);
  error << "what: " << what << "; error: " << human_readable_error;

  // Dispatch to either the fatal error handler, or the one for decode errors,
  // as appropriate.
  switch (av_errnum) {
    case AVERROR_EOF:
    case AVERROR(EINVAL):
    case AVERROR(ENOMEM):
      client_->OnFatalError(error.str());
      break;
    default:
      client_->OnDecodeError(error.str());
      break;
  }
}

}  // namespace wmediakits
