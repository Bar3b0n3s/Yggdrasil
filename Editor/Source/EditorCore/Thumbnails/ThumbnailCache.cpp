#include "EditorPCH.h"
#include "EditorCore/Thumbnails/ThumbnailCache.h"

#include "EditorCore/EditorContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Platform/SecureRandom.h"

#include <algorithm>
#include <deque>
#include <map>
#include <tuple>

namespace Engine {

	struct ThumbnailCache::State
	{
		EditorContext& Context; // Back-reference: context outlives the cache.
		ThumbnailRenderer Renderer{};
		uint64_t Generation = 0;
		uint64_t LastGeneration = 0;
		UUID DiskEpoch{}; // Process-local generations can repeat in a future editor process; disk entries cannot.
		std::filesystem::path ProjectFile{};
		std::filesystem::path CachePath{};
		std::filesystem::path CanonicalRoot{};
		bool Rendering = false;
		static constexpr uint32_t RendererFormat = 1;
		using Key = std::tuple<AssetHandle, uint64_t, uint32_t>;
		// The enclosing binding supplies generation, canonical root and renderer format to every key.
		std::map<Key, Result<ThumbnailResult>> Results{};
		std::deque<ThumbnailRequest> Pending{};

		explicit State(EditorContext& context, ThumbnailRenderer renderer)
			: Context(context), Renderer(std::move(renderer))
		{
		}

		[[nodiscard]] Status CheckBinding() const
		{
			if (Generation == 0)
				return MakeError(ErrorCode::InvalidState, "thumbnail cache has no project binding");
			if (!Context.HasProject() || Context.GetProject().GetProjectFile() != ProjectFile || Context.GetProject().GetCacheDirectory() != CachePath)
				return MakeError(ErrorCode::Conflict, "thumbnail project binding is no longer current; Reset before closing a project");
			return {};
		}

		[[nodiscard]] Status Validate(const ThumbnailRequest& request) const
		{
			ENGINE_TRY(CheckBinding());
			if (request.ProjectGeneration != Generation)
				return MakeError(ErrorCode::Conflict, "thumbnail request belongs to another project generation");
			if (!request.Asset.IsValid() || request.Size < 16 || request.Size > 512)
				return MakeError(ErrorCode::InvalidArgument, "thumbnail needs a valid asset and a size in [16, 512]");
			if (Context.GetAssets().GetAssetType(request.Asset) == AssetType::None)
				return MakeError(ErrorCode::NotFound, "thumbnail asset '{}' is unknown", request.Asset);
			if (Context.GetAssets().GetVersion(request.Asset) != request.Version)
				return MakeError(ErrorCode::Conflict, "thumbnail asset '{}' changed version", request.Asset);
			return {};
		}

		[[nodiscard]] Result<ThumbnailResult> Generate(const ThumbnailRequest& request)
		{
			ENGINE_TRY(Validate(request));
			std::error_code error;
			const auto root = std::filesystem::canonical(CachePath, error);
			if (error || root != CanonicalRoot)
				return MakeError(ErrorCode::Conflict, "thumbnail cache root changed since binding");
			const AssetType type = Context.GetAssets().GetAssetType(request.Asset);
			if (type != AssetType::Mesh && type != AssetType::Material && type != AssetType::Prefab && type != AssetType::Environment && type != AssetType::Texture)
				return ThumbnailResult{ .TypeIcon = true };
			// Numeric tokens only: source paths and labels never enter cache filenames.
			const auto file = root / std::format("thumbnail-v{}-{}-{:016x}-{}-{:016x}-{}.png", RendererFormat, DiskEpoch, Generation, request.Asset, request.Version, request.Size);
			const auto resolved = std::filesystem::weakly_canonical(file, error);
			if (error || resolved != file || resolved.parent_path() != root)
				return MakeError(ErrorCode::Io, "thumbnail filename resolves outside its bound cache entry");
			if (FileSystem::Exists(file))
			{
				ENGINE_TRY_ASSIGN(const Image image, ReadPng(file));
				if (image.Width != request.Size || image.Height != request.Size)
					return MakeError(ErrorCode::Validation, "cached thumbnail dimensions do not match the request");
				return ThumbnailResult{ .Path = FileSystem::PathToUtf8(file), .Cached = true };
			}
			if (!Renderer)
				return MakeError(ErrorCode::Unsupported, "visual thumbnails require an injected renderer");
			Rendering = true;
			Result<Image> rendered = Renderer(request);
			Rendering = false;
			if (!rendered)
				return std::unexpected(std::move(rendered).error());
			ENGINE_TRY(Validate(request));
			if (!rendered->IsValid() || rendered->Width != request.Size || rendered->Height != request.Size)
				return MakeError(ErrorCode::Validation, "thumbnail renderer must return a valid square image of the requested size");
			// Recheck confinement after the injected callback, before publishing any bytes.
			const auto afterRoot = std::filesystem::canonical(CachePath, error);
			if (error || afterRoot != root)
				return MakeError(ErrorCode::Conflict, "thumbnail cache root changed while rendering");
			const auto afterFile = std::filesystem::weakly_canonical(file, error);
			if (error || afterFile != file)
				return MakeError(ErrorCode::Io, "thumbnail cache entry changed while rendering");
			ENGINE_TRY_ASSIGN(const Buffer png, EncodePng(*rendered));
			ENGINE_TRY(FileSystem::WriteFileAtomic(file, png, AtomicWriteOptions{ .KeepBackup = false }));
			return ThumbnailResult{ .Path = FileSystem::PathToUtf8(file) };
		}
	};

	ThumbnailCache::ThumbnailCache(EditorContext& context, ThumbnailRenderer renderer)
		: m_State(CreateScope<State>(context, std::move(renderer)))
	{
	}
	ThumbnailCache::~ThumbnailCache() = default;

	Status ThumbnailCache::BindProject(uint64_t projectGeneration)
	{
		if (!m_State->Context.HasProject() || m_State->Generation != 0)
			return MakeError(ErrorCode::InvalidState, "thumbnail binding needs an open project and an unbound cache");
		if (projectGeneration == 0 || projectGeneration <= m_State->LastGeneration)
			return MakeError(ErrorCode::InvalidArgument, "thumbnail project generations must increase and cannot be zero");
		const LoadedProject& project = m_State->Context.GetProject();
		std::error_code error;
		auto root = std::filesystem::canonical(project.GetCacheDirectory(), error);
		if (error)
			return MakeError(ErrorCode::Io, "cannot resolve thumbnail cache root: {}", error.message());
		ENGINE_TRY_ASSIGN(const Random::State seed, SecureRandom::GenerateState());
		UUIDGenerator ids = UUIDGenerator::CreateRandom(seed);
		m_State->ProjectFile = project.GetProjectFile();
		m_State->CachePath = project.GetCacheDirectory();
		m_State->CanonicalRoot = std::move(root);
		m_State->Generation = projectGeneration;
		m_State->LastGeneration = projectGeneration;
		m_State->DiskEpoch = ids.Next();
		return {};
	}

	uint64_t ThumbnailCache::GetProjectGeneration() const
	{
		return m_State->Generation;
	}

	void ThumbnailCache::Reset()
	{
		m_State->Pending.clear();
		m_State->Results.clear();
		m_State->Generation = 0;
		m_State->ProjectFile.clear();
		m_State->CachePath.clear();
		m_State->CanonicalRoot.clear();
	}

	Result<ThumbnailResult> ThumbnailCache::Request(const ThumbnailRequest& request)
	{
		ENGINE_TRY(m_State->Validate(request));
		if (m_State->Rendering)
			return MakeError(ErrorCode::InvalidState, "thumbnail generation cannot recurse into the renderer");
		const State::Key key{ request.Asset, request.Version, request.Size };
		if (const auto found = m_State->Results.find(key); found != m_State->Results.end())
		{
			auto result = found->second;
			if (result && !result->TypeIcon)
				result->Cached = true;
			return result;
		}
		auto result = m_State->Generate(request);
		ENGINE_TRY(m_State->Validate(request));
		m_State->Results.emplace(key, result);
		return result;
	}

	Status ThumbnailCache::Queue(const ThumbnailRequest& request)
	{
		ENGINE_TRY(m_State->Validate(request));
		const State::Key key{ request.Asset, request.Version, request.Size };
		if (m_State->Results.contains(key))
			return {};
		const auto same = [&request](const ThumbnailRequest& queued)
		{
			return queued.Asset == request.Asset && queued.Version == request.Version && queued.Size == request.Size;
		};
		if (std::none_of(m_State->Pending.begin(), m_State->Pending.end(), same))
			m_State->Pending.push_back(request);
		return {};
	}

	Status ThumbnailCache::Pump(uint32_t maxItems)
	{
		ENGINE_TRY(m_State->CheckBinding());
		if (maxItems == 0)
			return MakeError(ErrorCode::InvalidArgument, "thumbnail pump budget must be positive");
		if (m_State->Rendering)
			return MakeError(ErrorCode::InvalidState, "thumbnail pump cannot run inside the renderer");
		std::optional<Error> failure;
		for (uint32_t index = 0; index < maxItems && !m_State->Pending.empty(); ++index)
		{
			const ThumbnailRequest request = m_State->Pending.front();
			m_State->Pending.pop_front();
			const auto result = Request(request);
			if (!result && !failure)
				failure = result.error();
		}
		return failure ? Status(std::unexpected(*failure)) : Status{};
	}

	Result<std::optional<ThumbnailResult>> ThumbnailCache::Find(const ThumbnailRequest& request) const
	{
		ENGINE_TRY(m_State->Validate(request));
		const auto found = m_State->Results.find(State::Key{ request.Asset, request.Version, request.Size });
		if (found == m_State->Results.end())
			return std::nullopt;
		if (!found->second)
			return std::unexpected(found->second.error());
		return *found->second;
	}

	void ThumbnailCache::Invalidate(AssetHandle asset)
	{
		std::erase_if(m_State->Results, [asset](const auto& entry)
		{
			return !asset.IsValid() || std::get<0>(entry.first) == asset;
		});
		std::erase_if(m_State->Pending, [asset](const ThumbnailRequest& request)
		{
			return !asset.IsValid() || request.Asset == asset;
		});
	}

}
