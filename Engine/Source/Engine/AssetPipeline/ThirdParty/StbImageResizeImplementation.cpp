// The implementation translation unit of stb_image_resize2 (Architecture §7.4: CPU mips), compiled without the precompiled
// header (Engine/premake5.lua's ThirdParty filter). Its implementation sets fp_contract off for the rest of this
// translation unit, which keeps its results independent of the configuration and must not leak into engine code
// (Vendor/stb/VENDOR.md). TextureImporter is its only caller; stb_image's implementation is Graphics'
// (Graphics/ThirdParty/StbImageImplementation.cpp, Docs/Decisions/0009-m5-decisions.md decision 11), and
// Docs/Decisions/0010-m6-decisions.md decision 1 moved this one from Asset to AssetPipeline.

#define STB_IMAGE_RESIZE_IMPLEMENTATION

#include <stb_image_resize2.h>
