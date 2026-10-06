#include "EnginePCH.h"
#include "Engine/Core/Hash.h"

// M1 contract stub (Roadmap rule 3): stream B implements XXH64 (one-shot and streaming) and Hash64. FNV-1a is
// constexpr in Hash.h.

namespace Engine {

	uint64_t XXH64(std::span<const std::byte> /*data*/, uint64_t /*seed*/)
	{
		return 0;
	}

	uint64_t XXH64(std::string_view /*text*/, uint64_t /*seed*/)
	{
		return 0;
	}

	XXH64Hasher::XXH64Hasher(uint64_t /*seed*/)
	{
	}

	void XXH64Hasher::Reset(uint64_t /*seed*/)
	{
	}

	void XXH64Hasher::Update(std::span<const std::byte> /*data*/)
	{
	}

	void XXH64Hasher::Update(std::string_view /*text*/)
	{
	}

	void XXH64Hasher::UpdateU64(uint64_t /*value*/)
	{
	}

	uint64_t XXH64Hasher::Digest() const
	{
		return 0;
	}

	uint64_t Hash64(uint64_t /*seed*/, uint64_t /*value*/)
	{
		return 0;
	}

	uint64_t Hash64(uint64_t /*seed*/, std::string_view /*key*/)
	{
		return 0;
	}

}
