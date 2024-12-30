#ifndef WMEDIAKITS_AVCODEC_GLUE_H
#define WMEDIAKITS_AVCODEC_GLUE_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/common.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
}

#include <memory>
#include <utility>

namespace wmediakits {

// Macro that, for an AVFoo, generates code for:
//
//  using FooUniquePtr = std::unique_ptr<Foo, FooFreer>;
//  FooUniquePtr MakeUniqueFoo(...args...);
#define DEFINE_AV_UNIQUE_PTR(name, create_func, free_statement)         \
  namespace internal {                                                  \
  struct name##Freer {                                                  \
    void operator()(name* obj) const {                                  \
      if (obj) {                                                        \
        free_statement;                                                 \
      }                                                                 \
    }                                                                   \
  };                                                                    \
  }                                                                     \
                                                                        \
  using name##UniquePtr = std::unique_ptr<name, internal::name##Freer>; \
                                                                        \
  template <typename... Args>                                           \
  name##UniquePtr MakeUnique##name(Args&&... args) {                    \
    return name##UniquePtr(create_func(std::forward<Args>(args)...));   \
  }

DEFINE_AV_UNIQUE_PTR(AVCodecParserContext,
                     av_parser_init,
                     av_parser_close(obj));
DEFINE_AV_UNIQUE_PTR(AVCodecContext,
                     avcodec_alloc_context3,
                     avcodec_free_context(&obj));
DEFINE_AV_UNIQUE_PTR(AVPacket, av_packet_alloc, av_packet_free(&obj));
DEFINE_AV_UNIQUE_PTR(AVFrame, av_frame_alloc, av_frame_free(&obj));

#undef DEFINE_AV_UNIQUE_PTR

// Macros to enable backwards compability codepaths for older versions of
// ffmpeg, where newer versions have deprecated APIs.  Note that ffmpeg defines
// its own FF_API* macros that are related to removing APIs (not deprecating
// them).
//
// TODO(issuetracker.google.com/224642520): dedup with standalone
// sender.
#define _LIBAVUTIL_OLD_CHANNEL_LAYOUT (LIBAVUTIL_VERSION_MAJOR < 57)

}  // namespace wmediakits

#endif  // #define WMEDIAKITS_AVCODEC_GLUE_H

