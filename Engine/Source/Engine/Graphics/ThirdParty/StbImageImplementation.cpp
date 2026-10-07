// The implementation translation unit of stb_image and stb_image_write (Architecture §2.2, §8.13), compiled without the
// precompiled header (Engine/premake5.lua). Graphics/Image.cpp is the PNG codec on top of it, and M6's importers call
// stb_image through the same implementation, so no other file may define these implementation macros
// (Docs/Decisions/0009-m5-decisions.md). The engine only uses the in-memory entry points: file access goes through
// FileSystem and the VFS (UTF-8 paths, atomic writes).

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

#include <stb_image.h>
#include <stb_image_write.h>
