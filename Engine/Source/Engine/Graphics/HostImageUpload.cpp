#include "EnginePCH.h"
#include "Engine/Graphics/HostImageUpload.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"

// M5 contract stub (Roadmap rule 3): stream A (loader, device, selection, creation wrappers, host image upload) implements
// both upload paths, the per-format host-copy query, the block allocator and the deferred-release queue (§8.1 path 1).

namespace Engine {

	HostImageUpload::HostImageUpload(GraphicsDevice& /*device*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	HostImageUpload::~HostImageUpload()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool HostImageUpload::IsHostImageCopyAvailable() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool HostImageUpload::SupportsHostImageCopy(nvrhi::Format /*format*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	Result<TextureUpload> HostImageUpload::CreateTexture(const nvrhi::TextureDesc& /*desc*/,
		std::span<const TextureSubresourceData> /*subresources*/, TextureUploadPath /*preferredPath*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "HostImageUpload::CreateTexture is not implemented yet");
	}

	void HostImageUpload::CollectGarbage()
	{
		ENGINE_CONTRACT_STUB();
	}

	size_t HostImageUpload::GetHostImageCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	size_t HostImageUpload::GetPendingReleaseCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	std::string_view TextureUploadPathToString(TextureUploadPath path)
	{
		switch (path)
		{
			case TextureUploadPath::HostImageCopy: return "HostImageCopy";
			case TextureUploadPath::Staging:       return "Staging";
		}

		ENGINE_CORE_ASSERT(false, "Unknown TextureUploadPath {}", std::to_underlying(path));
		return "Unknown";
	}

}
