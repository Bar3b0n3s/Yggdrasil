// The implementation translation unit of cgltf (Vendor/cgltf/VENDOR.md; Architecture §7.4), compiled without the
// precompiled header (Engine/premake5.lua, the files:**/Engine/*/ThirdParty/**.cpp filter) so the implementation macro
// precedes every inclusion of the header. Only the parser is compiled: the engine never writes glTF, so cgltf_write.h
// and its implementation are not part of the program. GltfImporter is the only caller; it parses from memory and
// supplies every buffer itself, so cgltf's own file reader (fopen) is never reached.

#define CGLTF_IMPLEMENTATION

#include <cgltf.h>
