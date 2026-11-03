#ifndef __CONFIG_TYPES_H__
#define __CONFIG_TYPES_H__

/* Local addition (Vendor/libogg/VENDOR.md, "Local additions"): config_types.h.in configured as upstream's
   CMakeLists.txt configures it on a C99 platform (inttypes.h, stdint.h and sys/types.h present; SIZE16 to USIZE64 the
   stdint types). os_types.h includes it only on platforms it does not list itself (Linux among them); MSVC and macOS
   never read it. */
#define INCLUDE_INTTYPES_H 1
#define INCLUDE_STDINT_H 1
#define INCLUDE_SYS_TYPES_H 1

#if INCLUDE_INTTYPES_H
#  include <inttypes.h>
#endif
#if INCLUDE_STDINT_H
#  include <stdint.h>
#endif
#if INCLUDE_SYS_TYPES_H
#  include <sys/types.h>
#endif

typedef int16_t ogg_int16_t;
typedef uint16_t ogg_uint16_t;
typedef int32_t ogg_int32_t;
typedef uint32_t ogg_uint32_t;
typedef int64_t ogg_int64_t;
typedef uint64_t ogg_uint64_t;

#endif
