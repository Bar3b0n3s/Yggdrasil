#include "EnginePCH.h"
#include "Engine/Graphics/ShaderReflection.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"

#include <nlohmann/json.hpp>

#include <algorithm>

// slangc's -reflection-json, as written by Scripts/CompileShaders.py for one entry point (Slang 2026.8, the pinned
// version). The parts read here:
//
//     { "parameters": [ { "name": ..., "binding": { "kind": ..., "space": S, "index": B }, "format": ...,
//                         "type": { "kind": "constantBuffer" | "resource" | "samplerState" | "array", ... } } ],
//       "entryPoints": [ { "name": ..., "stage": ..., "threadGroupSize": [x, y, z],
//                          "bindings": [ { "name": ..., "binding": { ..., "used": 0 | 1 } } ] } ] }
//
// "space" is omitted for space 0 and "used" for push constants; a parameter without a use flag counts as used. A structured
// buffer's type also carries its element type ("resultType"), whose struct fields have the same "binding" records as a
// constant buffer's.
// Specialization constants are parameters too ("specializationConstant") and are skipped: they are no bindings.
// Parameters with several binding kinds (a ParameterBlock, a struct of resources) are not part of the binding model of
// §8.4 and are rejected.

namespace Engine {

	namespace {

		// One storage-image format spelling of the reflection (the [vk::image_format] names, GLSL's layout qualifiers) and
		// its NVRHI format.
		struct ImageFormatName
		{
			std::string_view Name{};
			nvrhi::Format Format = nvrhi::Format::UNKNOWN;
		};

		// Every SPIR-V image format with an NVRHI equivalent.
		constexpr auto ImageFormatNames = std::to_array<ImageFormatName>({
			{ "rgba32f", nvrhi::Format::RGBA32_FLOAT },
			{ "rgba16f", nvrhi::Format::RGBA16_FLOAT },
			{ "rg32f", nvrhi::Format::RG32_FLOAT },
			{ "rg16f", nvrhi::Format::RG16_FLOAT },
			{ "r11f_g11f_b10f", nvrhi::Format::R11G11B10_FLOAT },
			{ "r32f", nvrhi::Format::R32_FLOAT },
			{ "r16f", nvrhi::Format::R16_FLOAT },
			{ "rgba16", nvrhi::Format::RGBA16_UNORM },
			{ "rgb10_a2", nvrhi::Format::R10G10B10A2_UNORM },
			{ "rgba8", nvrhi::Format::RGBA8_UNORM },
			{ "rg16", nvrhi::Format::RG16_UNORM },
			{ "rg8", nvrhi::Format::RG8_UNORM },
			{ "r16", nvrhi::Format::R16_UNORM },
			{ "r8", nvrhi::Format::R8_UNORM },
			{ "rgba16_snorm", nvrhi::Format::RGBA16_SNORM },
			{ "rgba8_snorm", nvrhi::Format::RGBA8_SNORM },
			{ "rg16_snorm", nvrhi::Format::RG16_SNORM },
			{ "rg8_snorm", nvrhi::Format::RG8_SNORM },
			{ "r16_snorm", nvrhi::Format::R16_SNORM },
			{ "r8_snorm", nvrhi::Format::R8_SNORM },
			{ "rgba32i", nvrhi::Format::RGBA32_SINT },
			{ "rgba16i", nvrhi::Format::RGBA16_SINT },
			{ "rgba8i", nvrhi::Format::RGBA8_SINT },
			{ "rg32i", nvrhi::Format::RG32_SINT },
			{ "rg16i", nvrhi::Format::RG16_SINT },
			{ "rg8i", nvrhi::Format::RG8_SINT },
			{ "r32i", nvrhi::Format::R32_SINT },
			{ "r16i", nvrhi::Format::R16_SINT },
			{ "r8i", nvrhi::Format::R8_SINT },
			{ "rgba32ui", nvrhi::Format::RGBA32_UINT },
			{ "rgba16ui", nvrhi::Format::RGBA16_UINT },
			{ "rgba8ui", nvrhi::Format::RGBA8_UINT },
			{ "rg32ui", nvrhi::Format::RG32_UINT },
			{ "rg16ui", nvrhi::Format::RG16_UINT },
			{ "rg8ui", nvrhi::Format::RG8_UINT },
			{ "r32ui", nvrhi::Format::R32_UINT },
			{ "r16ui", nvrhi::Format::R16_UINT },
			{ "r8ui", nvrhi::Format::R8_UINT },
		});

		// The use flags of the entry point's "bindings", by parameter name.
		using UsageMap = std::map<std::string, bool, std::less<>>;

	}

	namespace Utils {

		static std::unexpected<Error> MakeShapeError(const JsonReader& value, std::string message)
		{
			return std::unexpected(value.MakeLocatedError(ErrorCode::Validation, std::move(message)));
		}

		static Result<nvrhi::ShaderType> ReadStage(const JsonReader& entryPoint)
		{
			ENGINE_TRY_ASSIGN(const JsonReader stage, entryPoint.GetMember("stage"));
			ENGINE_TRY_ASSIGN(const std::string name, stage.ReadString());
			if (name == "vertex")
				return nvrhi::ShaderType::Vertex;
			if (name == "fragment")
				return nvrhi::ShaderType::Pixel;
			if (name == "compute")
				return nvrhi::ShaderType::Compute;
			return MakeShapeError(stage, std::format("unknown stage '{}' (expected vertex, fragment or compute)", name));
		}

		static Result<std::array<uint32_t, 3>> ReadThreadGroupSize(const JsonReader& entryPoint)
		{
			ENGINE_TRY_ASSIGN(const JsonReader size, entryPoint.GetMember("threadGroupSize"));
			ENGINE_TRY_ASSIGN(const size_t count, size.GetArraySize());
			std::array<uint32_t, 3> threadGroupSize{};
			if (count != threadGroupSize.size())
				return MakeShapeError(size, std::format("expected {} thread-group dimensions, got {}", threadGroupSize.size(), count));
			for (size_t index = 0; index < threadGroupSize.size(); ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader dimension, size.GetElement(index));
				ENGINE_TRY_ASSIGN(threadGroupSize[index], dimension.ReadUInt32());
			}
			return threadGroupSize;
		}

		static Result<UsageMap> ReadUsage(const JsonReader& entryPoint)
		{
			UsageMap usage;
			const std::optional<JsonReader> bindings = entryPoint.FindMember("bindings");
			if (!bindings)
				return usage;
			ENGINE_TRY_ASSIGN(const size_t count, bindings->GetArraySize());
			for (size_t index = 0; index < count; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader element, bindings->GetElement(index));
				ENGINE_TRY_ASSIGN(std::string name, element.ReadMember<std::string>("name"));
				bool used = true;
				if (const std::optional<JsonReader> binding = element.FindMember("binding"))
				{
					ENGINE_TRY(binding->ExpectType(JsonType::Object));
					if (const std::optional<JsonReader> flag = binding->FindMember("used"))
					{
						ENGINE_TRY_ASSIGN(const uint32_t value, flag->ReadUInt32());
						used = value != 0;
					}
				}
				usage.insert_or_assign(std::move(name), used);
			}
			return usage;
		}

		// The binding kind of a parameter; nullopt for a specialization constant, which is not a binding.
		static Result<std::optional<ShaderBindingKind>> ReadBindingKind(const JsonReader& binding)
		{
			ENGINE_TRY_ASSIGN(const JsonReader kind, binding.GetMember("kind"));
			ENGINE_TRY_ASSIGN(const std::string name, kind.ReadString());
			if (name == "constantBuffer")
				return ShaderBindingKind::ConstantBuffer;
			if (name == "shaderResource")
				return ShaderBindingKind::ShaderResource;
			if (name == "unorderedAccess")
				return ShaderBindingKind::UnorderedAccess;
			if (name == "samplerState")
				return ShaderBindingKind::Sampler;
			if (name == "pushConstantBuffer")
				return ShaderBindingKind::PushConstantBuffer;
			if (name == "specializationConstant")
				return std::optional<ShaderBindingKind>();
			return MakeShapeError(kind, std::format("unknown binding kind '{}' (not part of the binding model of §8.4)", name));
		}

		static Result<ShaderResourceShape> ReadResourceShape(const JsonReader& type)
		{
			ENGINE_TRY_ASSIGN(const JsonReader baseShape, type.GetMember("baseShape"));
			ENGINE_TRY_ASSIGN(const std::string shape, baseShape.ReadString());
			bool isArray = false;
			if (const std::optional<JsonReader> array = type.FindMember("array"))
			{
				ENGINE_TRY_ASSIGN(isArray, array->ReadBool());
			}
			if (const std::optional<JsonReader> multisample = type.FindMember("multisample"))
			{
				ENGINE_TRY_ASSIGN(const bool isMultisampled, multisample->ReadBool());
				if (isMultisampled)
					return MakeShapeError(*multisample, "multisampled textures are not supported");
			}

			if (shape == "texture2D")
				return isArray ? ShaderResourceShape::Texture2DArray : ShaderResourceShape::Texture2D;
			if (shape == "textureCube")
				return isArray ? ShaderResourceShape::TextureCubeArray : ShaderResourceShape::TextureCube;
			if (!isArray)
			{
				if (shape == "texture1D")
					return ShaderResourceShape::Texture1D;
				if (shape == "texture3D")
					return ShaderResourceShape::Texture3D;
				if (shape == "structuredBuffer")
					return ShaderResourceShape::StructuredBuffer;
				if (shape == "byteAddressBuffer")
					return ShaderResourceShape::ByteAddressBuffer;
				if (shape == "textureBuffer")
					return ShaderResourceShape::TypedBuffer;
			}
			return MakeShapeError(baseShape, std::format("unsupported resource shape '{}'{}", shape, isArray ? " (array)" : ""));
		}

		static Result<nvrhi::Format> ReadImageFormat(const JsonReader& format)
		{
			ENGINE_TRY_ASSIGN(const std::string name, format.ReadString());
			for (const ImageFormatName& entry : ImageFormatNames)
			{
				if (entry.Name == name)
					return entry.Format;
			}
			return MakeShapeError(format, std::format("unknown image format '{}'", name));
		}

		static Result<std::string> ReadTypeKind(const JsonReader& type)
		{
			ENGINE_TRY(type.ExpectType(JsonType::Object));
			return type.ReadMember<std::string>("kind");
		}

		// Adds the struct `type` (kind "struct") of `size` bytes to `structs` unless a struct of its name is there already,
		// after the structs its fields nest (directly or as array elements).
		static Status CollectStruct(const JsonReader& type, uint32_t size, std::vector<ShaderStruct>& structs)
		{
			ENGINE_TRY_ASSIGN(std::string name, type.ReadMember<std::string>("name"));
			const auto isNamed = [&name](const ShaderStruct& shaderStruct)
			{
				return shaderStruct.Name == name;
			};
			if (std::ranges::any_of(structs, isNamed))
				return {};

			ShaderStruct shaderStruct{ .Name = std::move(name), .Size = size };
			ENGINE_TRY_ASSIGN(const JsonReader fields, type.GetMember("fields"));
			ENGINE_TRY_ASSIGN(const size_t count, fields.GetArraySize());
			for (size_t index = 0; index < count; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader field, fields.GetElement(index));
				ENGINE_TRY_ASSIGN(std::string fieldName, field.ReadMember<std::string>("name"));
				ENGINE_TRY_ASSIGN(const JsonReader binding, field.GetMember("binding"));
				ENGINE_TRY_ASSIGN(const std::string bindingKind, binding.ReadMember<std::string>("kind"));
				if (bindingKind != "uniform")
				{
					const std::string message = std::format("member '{}' of '{}' is a '{}' binding; constant-buffer structs hold plain data only",
						fieldName, shaderStruct.Name, bindingKind);
					return MakeShapeError(binding, message);
				}
				ENGINE_TRY_ASSIGN(const uint32_t offset, binding.ReadMember<uint32_t>("offset"));
				ENGINE_TRY_ASSIGN(const uint32_t fieldSize, binding.ReadMember<uint32_t>("size"));

				// A nested struct is recorded with its own size: the member's size, or an array's element stride.
				ENGINE_TRY_ASSIGN(JsonReader fieldType, field.GetMember("type"));
				ENGINE_TRY_ASSIGN(std::string fieldKind, ReadTypeKind(fieldType));
				uint32_t nestedSize = fieldSize;
				while (fieldKind == "array")
				{
					ENGINE_TRY_ASSIGN(nestedSize, fieldType.ReadMember<uint32_t>("uniformStride"));
					ENGINE_TRY_ASSIGN(fieldType, fieldType.GetMember("elementType"));
					ENGINE_TRY_ASSIGN(fieldKind, ReadTypeKind(fieldType));
				}
				if (fieldKind == "struct")
					ENGINE_TRY(CollectStruct(fieldType, nestedSize, structs));

				shaderStruct.Fields.push_back({ .Name = std::move(fieldName), .Offset = offset, .Size = fieldSize });
			}
			structs.push_back(std::move(shaderStruct));
			return {};
		}

		// The element layout of a constant buffer or push-constant parameter: its size and, for a struct, its name and
		// the structs it reaches.
		static Status ReadConstantBufferLayout(const JsonReader& type, ShaderBinding& binding, std::vector<ShaderStruct>& structs)
		{
			ENGINE_TRY_ASSIGN(const JsonReader elementLayout, type.GetMember("elementVarLayout"));
			ENGINE_TRY_ASSIGN(const JsonReader layoutBinding, elementLayout.GetMember("binding"));
			ENGINE_TRY_ASSIGN(binding.ByteSize, layoutBinding.ReadMember<uint32_t>("size"));
			ENGINE_TRY_ASSIGN(const JsonReader elementType, elementLayout.GetMember("type"));
			ENGINE_TRY_ASSIGN(const std::string elementKind, ReadTypeKind(elementType));
			if (elementKind != "struct")
				return {};
			ENGINE_TRY_ASSIGN(binding.StructName, elementType.ReadMember<std::string>("name"));
			return CollectStruct(elementType, binding.ByteSize, structs);
		}

		// The element struct of a structured buffer (its "resultType"), sized by the extent of its fields, the end of the one
		// that ends last: slangc reports std430 field offsets for the element but no stride. A shared struct without tail
		// padding beyond its last member (Shared/ShaderLight.h pads explicitly) has exactly that size in C++. Elements that are
		// not structs carry no struct.
		static Status ReadStructuredBufferElement(const JsonReader& type, std::vector<ShaderStruct>& structs)
		{
			const std::optional<JsonReader> element = type.FindMember("resultType");
			if (!element.has_value())
				return {};
			ENGINE_TRY_ASSIGN(const std::string elementKind, ReadTypeKind(*element));
			if (elementKind != "struct")
				return {};
			ENGINE_TRY_ASSIGN(const JsonReader fields, element->GetMember("fields"));
			ENGINE_TRY_ASSIGN(const size_t count, fields.GetArraySize());
			uint32_t extent = 0;
			for (size_t index = 0; index < count; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader field, fields.GetElement(index));
				ENGINE_TRY_ASSIGN(const JsonReader binding, field.GetMember("binding"));
				ENGINE_TRY_ASSIGN(const uint32_t offset, binding.ReadMember<uint32_t>("offset"));
				ENGINE_TRY_ASSIGN(const uint32_t fieldSize, binding.ReadMember<uint32_t>("size"));
				extent = std::max(extent, offset + fieldSize);
			}
			return CollectStruct(*element, extent, structs);
		}

		// One global parameter; nullopt for a specialization constant.
		static Result<std::optional<ShaderBinding>> ReadParameter(const JsonReader& parameter, const UsageMap& usage,
			std::vector<ShaderStruct>& structs)
		{
			ShaderBinding binding;
			ENGINE_TRY_ASSIGN(binding.Name, parameter.ReadMember<std::string>("name"));
			if (!parameter.HasMember("binding"))
			{
				// A ParameterBlock or a struct of resources.
				return MakeShapeError(parameter, std::format("parameter '{}' has several binding kinds (outside the binding model of §8.4)", binding.Name));
			}
			ENGINE_TRY_ASSIGN(const JsonReader bindingInfo, parameter.GetMember("binding"));
			ENGINE_TRY_ASSIGN(const std::optional<ShaderBindingKind> kind, ReadBindingKind(bindingInfo));
			if (!kind)
				return std::optional<ShaderBinding>();
			binding.Kind = *kind;

			// Push constants have no descriptor set or binding (§8.4).
			if (binding.Kind != ShaderBindingKind::PushConstantBuffer)
			{
				if (const std::optional<JsonReader> space = bindingInfo.FindMember("space"))
				{
					ENGINE_TRY_ASSIGN(binding.Set, space->ReadUInt32());
				}
				ENGINE_TRY_ASSIGN(binding.Binding, bindingInfo.ReadMember<uint32_t>("index"));
			}

			ENGINE_TRY_ASSIGN(JsonReader type, parameter.GetMember("type"));
			ENGINE_TRY_ASSIGN(std::string typeKind, ReadTypeKind(type));
			if (typeKind == "array")
			{
				ENGINE_TRY_ASSIGN(binding.ArraySize, type.ReadMember<uint32_t>("elementCount"));
				ENGINE_TRY_ASSIGN(type, type.GetMember("elementType"));
				ENGINE_TRY_ASSIGN(typeKind, ReadTypeKind(type));
				if (typeKind == "array")
					return MakeShapeError(type, std::format("parameter '{}' is an array of arrays, which has no single binding", binding.Name));
				if (binding.Kind == ShaderBindingKind::PushConstantBuffer)
					return MakeShapeError(type, std::format("push constants '{}' cannot be an array", binding.Name));
			}

			switch (binding.Kind)
			{
				case ShaderBindingKind::ConstantBuffer:
				case ShaderBindingKind::PushConstantBuffer:
				{
					if (typeKind != "constantBuffer")
						return MakeShapeError(type, std::format("constant buffer '{}' has type kind '{}'", binding.Name, typeKind));
					ENGINE_TRY(ReadConstantBufferLayout(type, binding, structs));
					break;
				}
				case ShaderBindingKind::ShaderResource:
				case ShaderBindingKind::UnorderedAccess:
				{
					if (typeKind != "resource")
						return MakeShapeError(type, std::format("resource '{}' has type kind '{}'", binding.Name, typeKind));
					ENGINE_TRY_ASSIGN(binding.Shape, ReadResourceShape(type));
					if (binding.Shape == ShaderResourceShape::StructuredBuffer)
						ENGINE_TRY(ReadStructuredBufferElement(type, structs));
					break;
				}
				case ShaderBindingKind::Sampler:
				{
					if (typeKind != "samplerState")
						return MakeShapeError(type, std::format("sampler '{}' has type kind '{}'", binding.Name, typeKind));
					break;
				}
			}

			if (const std::optional<JsonReader> format = parameter.FindMember("format"))
			{
				ENGINE_TRY_ASSIGN(binding.StorageFormat, ReadImageFormat(*format));
			}

			const auto use = usage.find(binding.Name);
			binding.Used = use == usage.end() || use->second;
			return std::optional<ShaderBinding>(std::move(binding));
		}

	}

	const ShaderBinding* ShaderReflection::FindBinding(std::string_view name) const
	{
		for (const ShaderBinding& binding : Bindings)
		{
			if (binding.Name == name)
				return &binding;
		}
		return nullptr;
	}

	const ShaderStruct* ShaderReflection::FindStruct(std::string_view name) const
	{
		for (const ShaderStruct& shaderStruct : Structs)
		{
			if (shaderStruct.Name == name)
				return &shaderStruct;
		}
		return nullptr;
	}

	Result<ShaderReflection> ParseShaderReflection(std::string_view json)
	{
		ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(json));
		const JsonReader root(document);
		ENGINE_TRY(root.ExpectType(JsonType::Object));

		ShaderReflection reflection;
		ENGINE_TRY_ASSIGN(const JsonReader entryPoints, root.GetMember("entryPoints"));
		ENGINE_TRY_ASSIGN(const size_t entryPointCount, entryPoints.GetArraySize());
		if (entryPointCount != 1)
			return Utils::MakeShapeError(entryPoints, std::format("expected exactly one entry point, got {}", entryPointCount));
		ENGINE_TRY_ASSIGN(const JsonReader entryPoint, entryPoints.GetElement(0));
		ENGINE_TRY_ASSIGN(reflection.EntryPoint, entryPoint.ReadMember<std::string>("name"));
		ENGINE_TRY_ASSIGN(reflection.Stage, Utils::ReadStage(entryPoint));
		if (reflection.Stage == nvrhi::ShaderType::Compute)
		{
			ENGINE_TRY_ASSIGN(reflection.ThreadGroupSize, Utils::ReadThreadGroupSize(entryPoint));
		}
		ENGINE_TRY_ASSIGN(const UsageMap usage, Utils::ReadUsage(entryPoint));

		// A program without global parameters may omit the list.
		if (const std::optional<JsonReader> parameters = root.FindMember("parameters"))
		{
			ENGINE_TRY_ASSIGN(const size_t parameterCount, parameters->GetArraySize());
			for (size_t index = 0; index < parameterCount; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader parameter, parameters->GetElement(index));
				ENGINE_TRY_ASSIGN(std::optional<ShaderBinding> binding, Utils::ReadParameter(parameter, usage, reflection.Structs));
				if (binding)
					reflection.Bindings.push_back(std::move(*binding));
			}
		}

		std::ranges::sort(reflection.Structs, std::less<>(), &ShaderStruct::Name);
		return reflection;
	}

	std::string_view ShaderBindingKindToString(ShaderBindingKind kind)
	{
		switch (kind)
		{
			case ShaderBindingKind::ConstantBuffer:     return "ConstantBuffer";
			case ShaderBindingKind::ShaderResource:     return "ShaderResource";
			case ShaderBindingKind::UnorderedAccess:    return "UnorderedAccess";
			case ShaderBindingKind::Sampler:            return "Sampler";
			case ShaderBindingKind::PushConstantBuffer: return "PushConstantBuffer";
		}

		ENGINE_CORE_ASSERT(false, "Unknown ShaderBindingKind {}", std::to_underlying(kind));
		return "Unknown";
	}

}
