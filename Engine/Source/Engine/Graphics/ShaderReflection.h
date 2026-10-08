#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The reflection of one compiled shader variant (Architecture §8.4, §8.12): the parts of slangc's -reflection-json
// (<Program>/<Entry>[.<KEY>-<VALUE>...].refl.json, written by Scripts/CompileShaders.py) that PipelineFactory checks
// binding layouts against and that the CPU-only tests "Shaders: LayoutsMatchReflection" and
// "Shaders: SharedStructsMatchReflection" compare with the C++ side. Plain data, parsed without a GPU.

namespace Engine {

	// The register class of a binding, from slangc's binding kind.
	enum class ShaderBindingKind : uint8_t
	{
		ConstantBuffer,    // "constantBuffer" (b registers, Vulkan binding 256 + register)
		ShaderResource,    // "shaderResource" (t registers: textures, structured buffers; binding 0 + register)
		UnorderedAccess,   // "unorderedAccess" (u registers: storage images and buffers; binding 384 + register)
		Sampler,           // "samplerState" (s registers; binding 128 + register)
		PushConstantBuffer // "pushConstantBuffer" ([[vk::push_constant]]): no set or binding, only a size
	};

	// The resource shape behind a binding, from slangc's type kind and baseShape.
	enum class ShaderResourceShape : uint8_t
	{
		None, // constant buffers, push constants and samplers
		Texture1D,
		Texture2D,
		Texture2DArray,
		Texture3D,
		TextureCube,
		TextureCubeArray,
		StructuredBuffer,
		ByteAddressBuffer,
		TypedBuffer
	};

	// One member of a reflected struct: its byte offset and size within the struct (the constant buffer layout slangc
	// chose, which the C++ struct must reproduce, §8.4).
	struct ShaderStructField
	{
		std::string Name{};
		uint32_t Offset = 0;
		uint32_t Size = 0;
	};

	// A struct type the shader uses in a constant buffer, push constants or as a structured buffer's element, with its fields
	// in declaration order.
	struct ShaderStruct
	{
		std::string Name{}; // the Slang type name, which equals the C++ name for the shared structs of Resources/Shaders/Shared
		// The element size of a constant buffer or push constants (or a nested struct's member size); for a structured buffer's
		// element, which slangc reports without a stride, the end of its last field.
		uint32_t Size = 0;
		std::vector<ShaderStructField> Fields{};
	};

	// One global shader parameter.
	struct ShaderBinding
	{
		std::string Name{};
		ShaderBindingKind Kind = ShaderBindingKind::ConstantBuffer;
		ShaderResourceShape Shape = ShaderResourceShape::None;
		// Descriptor set (the register space, §8.4) and Vulkan binding number (the register shifted by NVRHI's default
		// VulkanBindingOffsets, as slangc reports it). Both 0 for push constants.
		uint32_t Set = 0;
		uint32_t Binding = 0;
		// 1 for a single resource; the element count of a fixed-size array; 0 for an unbounded array.
		uint32_t ArraySize = 1;
		// Constant buffers and push constants: the size of their element struct in bytes (the struct is in Structs).
		uint32_t ByteSize = 0;
		std::string StructName{};
		// Storage images (UnorderedAccess textures): the format of their [vk::image_format] attribute (§8.4), nullopt when
		// the shader declares none.
		std::optional<nvrhi::Format> StorageFormat{};
		// Whether the entry point statically uses the binding (the entry point's "used" flag).
		bool Used = false;
	};

	struct ShaderReflection
	{
		std::string EntryPoint{};
		nvrhi::ShaderType Stage = nvrhi::ShaderType::None; // Vertex, Pixel or Compute
		std::vector<ShaderBinding> Bindings{};             // in slangc's order
		std::vector<ShaderStruct> Structs{};               // every struct reachable from a constant buffer, push constants or a structured buffer's element, by name, each once
		std::array<uint32_t, 3> ThreadGroupSize{};         // compute only; zero otherwise

		// The binding named `name`, or nullptr.
		[[nodiscard]] const ShaderBinding* FindBinding(std::string_view name) const;
		// The struct named `name`, or nullptr.
		[[nodiscard]] const ShaderStruct* FindStruct(std::string_view name) const;
	};

	// Parses one slangc -reflection-json document for the entry point it describes (the variant files hold exactly one).
	// Struct fields are taken from the constant buffers' and push constants' element layouts ("elementVarLayout") and the
	// structured buffers' element types ("resultType"), nested structs flattened into their own ShaderStruct entries. Errors:
	// Parse for malformed JSON (json::parse with allow_exceptions false), Validation naming the JSON pointer for an
	// unexpected shape (no entry point, an unknown stage, binding kind or image format). Never throws.
	[[nodiscard]] Result<ShaderReflection> ParseShaderReflection(std::string_view json);

	// The enumerator name ("ConstantBuffer", ...).
	[[nodiscard]] std::string_view ShaderBindingKindToString(ShaderBindingKind kind);

}
