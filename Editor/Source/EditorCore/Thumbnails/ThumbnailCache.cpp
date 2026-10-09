#include "EditorPCH.h"
#include "EditorCore/Thumbnails/ThumbnailCache.h"

namespace Engine {

	Status ThumbnailCache::BindProject(uint64_t /*projectGeneration*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	uint64_t ThumbnailCache::GetProjectGeneration() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	void ThumbnailCache::Reset()
	{
		ENGINE_CONTRACT_STUB();
	}

	Status ThumbnailCache::Queue(const ThumbnailRequest& /*request*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Status ThumbnailCache::Pump(uint32_t /*maxItems*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Result<std::optional<ThumbnailResult>> ThumbnailCache::Find(const ThumbnailRequest& /*request*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	ThumbnailCache::ThumbnailCache(EditorContext& /*context*/, ThumbnailRenderer /*renderer*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	ThumbnailCache::~ThumbnailCache()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<ThumbnailResult> ThumbnailCache::Request(const ThumbnailRequest& /*request*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	void ThumbnailCache::Invalidate(AssetHandle /*asset*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
