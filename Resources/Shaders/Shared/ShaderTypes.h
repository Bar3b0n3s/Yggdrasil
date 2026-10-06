#pragma once

// The vocabulary of the headers shared between C++ and Slang (Architecture §8.4): PascalCase names for the vector and
// matrix types, which are Slang's built-in types in shaders and glm's types in C++ (column-major, mul(M, v) in Slang,
// -matrix-layout-column-major; the GPU test "Shaders: matrix convention transforms known vectors" checks it). Each shared struct orders its members so that
// the C++ layout (glm types are 4-byte aligned) equals the constant-buffer layout slangc chooses, which
// "Shaders: SharedStructsMatchReflection" compares member by member with offsetof.
//
// slangc predefines __SLANG__; C++ compilers never do. ENGINE_SHADER is set for the Slang side.
#if defined(__SLANG__) && !defined(ENGINE_SHADER)
	#define ENGINE_SHADER 1
#endif

#if defined(ENGINE_SHADER)

typedef float2 Float2;
typedef float3 Float3;
typedef float4 Float4;
typedef float4x4 Float4x4;
typedef uint2 UInt2;
typedef uint4 UInt4;

#else

	#include <glm/mat4x4.hpp>
	#include <glm/vec2.hpp>
	#include <glm/vec3.hpp>
	#include <glm/vec4.hpp>

	#include <cstdint>

namespace Engine {

	using Float2 = glm::vec2;
	using Float3 = glm::vec3;
	using Float4 = glm::vec4;
	using Float4x4 = glm::mat4;
	using UInt2 = glm::uvec2;
	using UInt4 = glm::uvec4;

	static_assert(sizeof(Float3) == 12 && alignof(Float3) == 4, "shared structs rely on glm's packed 4-byte-aligned vectors");
	static_assert(sizeof(Float4x4) == 64 && alignof(Float4x4) == 4, "shared structs rely on glm's packed 4-byte-aligned matrices");

}

#endif
