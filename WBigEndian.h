#ifndef WMEDIAKITS_BIG_ENDIAN_H_
#define WMEDIAKITS_BIG_ENDIAN_H_

#include <stdint.h>

// Returns true if this code is running on a big-endian architecture.
inline bool IsBigEndianArchitecture() {
  const uint16_t kTestWord = 0x0100;
  uint8_t bytes[sizeof(kTestWord)];
  memcpy(bytes, &kTestWord, sizeof(bytes));
  return !!bytes[0];
}

#endif  // WMEDIAKITS_BIG_ENDIAN_H_
