#ifndef WMEDIAKITS_DECODER_H_
#define WMEDIAKITS_DECODER_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "avcodec_glue.h"

namespace wmediakits {

// Wraps libavcodec to decode audio or video.
class WDecoder {
 public:
  // Interface for receiving decoded frames and/or errors.
  class Client {
   public:

    virtual void OnFrameDecoded(const AVFrame& frame) = 0;
    virtual void OnDecodeError(const std::string& message) = 0;
    virtual void OnFatalError(const std::string& message) = 0;

   protected:
    Client();
    virtual ~Client();
  };

  explicit WDecoder();
  ~WDecoder();

  Client* GetClient() const { return client_; }
  void SetClient(Client* client) { client_ = client; }

  void SetCodecName(const std::string& codec_name);

  void Decode(unsigned char* data, int data_len);

 private:
  // Helper to initialize the FFMPEG decoder and supporting objects. Returns
  // false if this failed (and the Client was notified).
  bool Initialize();

  // Helper to handle a codec initialization error and notify the Client of the
  // fatal error.
  void HandleInitializationError(const char* what, int av_errnum);

  // Called when any transient or fatal error occurs, generating an Error and
  // notifying the Client of it.
  void OnError(const char* what, int av_errnum);

  std::string codec_name_;
  const AVCodec* codec_ = nullptr;
  AVCodecParserContextUniquePtr parser_;
  AVCodecContextUniquePtr context_;
  AVPacketUniquePtr packet_;
  AVFrameUniquePtr decoded_frame_;

  Client* client_ = nullptr;

};

}  // namespace wmediakits

#endif  // WMEDIAKITS_DECODER_H_
