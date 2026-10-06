#include "EnginePCH.h"
#include "Engine/Graphics/ShaderLibrary.h"

#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"

// M5 contract stub (Roadmap rule 3): stream C (shader pipeline) implements the variant stems, loading through the VFS,
// the caches and shader creation of Architecture §8.12.

namespace Engine {

	std::string MakeShaderVariantStem(std::string_view /*program*/, std::string_view /*entry*/, std::span<const ShaderDefine> /*permutation*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	ShaderLibrary::ShaderLibrary(GraphicsDevice* /*device*/, const VirtualFileSystem& /*vfs*/, VfsPath root)
		: m_Root(std::move(root))
	{
		ENGINE_CONTRACT_STUB();
	}

	ShaderLibrary::~ShaderLibrary() = default;

	Result<nvrhi::ShaderHandle> ShaderLibrary::Get(std::string_view /*program*/, std::string_view /*entry*/,
		std::span<const ShaderDefine> /*permutation*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ShaderLibrary::Get is not implemented yet");
	}

	Result<const ShaderReflection*> ShaderLibrary::GetReflection(std::string_view /*program*/, std::string_view /*entry*/,
		std::span<const ShaderDefine> /*permutation*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ShaderLibrary::GetReflection is not implemented yet");
	}

	void ShaderLibrary::Clear()
	{
		ENGINE_CONTRACT_STUB();
	}

}
