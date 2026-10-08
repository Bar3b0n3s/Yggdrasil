/*
Local implementation translation unit (not upstream; see VENDOR.md, "Local additions"). It replaces upstream's
miniaudio.c in the build: miniaudio's implementation with stb_vorbis as its Ogg Vorbis decoding backend, in the order
miniaudio.h documents ("Vorbis": the stb_vorbis header before the implementation, its implementation after it).
miniaudio.h registers the Vorbis backend because STB_VORBIS_INCLUDE_STB_VORBIS_H is defined when its implementation
is compiled. stb_vorbis.c is vendored unmodified in Vendor/stb (Vendor/stb/VENDOR.md).
*/

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
