#include "EnginePCH.h"
#include "Engine/Graphics/ShaderReflection.h"

#include "Engine/Core/Assert.h"

// M5 contract stub (Roadmap rule 3): stream C (shader pipeline) implements parsing slangc's -reflection-json through
// Core/Json. The lookups over parsed data are implemented.

namespace Engine {

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

	Result<ShaderReflection> ParseShaderReflection(std::string_view /*json*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ParseShaderReflection is not implemented yet");
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
