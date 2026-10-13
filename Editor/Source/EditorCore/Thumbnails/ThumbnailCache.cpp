#include "EditorPCH.h"
#include "EditorCore/Thumbnails/ThumbnailCache.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/AssetPipeline/AssetDependencyGraph.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Project/ProjectSettings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <deque>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		constexpr std::string_view ThumbnailDirectoryName = "Thumbnails";
		constexpr std::string_view ThumbnailManifestFormat = "ThumbnailManifest";
		constexpr uint32_t ThumbnailManifestVersion = 1;

		[[nodiscard]] static std::string ThumbnailStem(AssetHandle asset, uint32_t size)
		{
			return std::format("{}-{}", asset.ToString(), size);
		}

		[[nodiscard]] static std::string ThumbnailImageName(AssetHandle asset, uint32_t size, uint64_t digest)
		{
			return std::format("{}-{:016x}.png", ThumbnailStem(asset, size), digest);
		}

		[[nodiscard]] static std::optional<uint64_t> ParseThumbnailHex(std::string_view text)
		{
			uint64_t value = 0;
			if (text.size() != 16)
				return std::nullopt;
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
			if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
				return std::nullopt;
			return value;
		}

		// The asset and size of a file named by ThumbnailStem plus `suffix` ("-<digest>.png" or ".json"), and the digest
		// for an image; nullopt for any other name.
		struct ThumbnailFileName
		{
			AssetHandle Asset{};
			uint32_t Size = 0;
			std::optional<uint64_t> Digest{};
		};
		[[nodiscard]] static std::optional<ThumbnailFileName> ParseThumbnailFileName(std::string_view name)
		{
			ThumbnailFileName parsed;
			std::string_view rest;
			if (name.ends_with(".json"))
				rest = name.substr(0, name.size() - 5);
			else if (name.ends_with(".png") && name.size() > 21 && name[name.size() - 21] == '-')
			{
				parsed.Digest = ParseThumbnailHex(name.substr(name.size() - 20, 16));
				if (!parsed.Digest)
					return std::nullopt;
				rest = name.substr(0, name.size() - 21);
			}
			else
				return std::nullopt;
			if (rest.size() < 18 || rest[16] != '-')
				return std::nullopt;
			const std::optional<uint64_t> asset = ParseThumbnailHex(rest.substr(0, 16));
			const std::string_view sizeText = rest.substr(17);
			const auto sizeParsed = std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), parsed.Size);
			if (!asset || sizeParsed.ec != std::errc() || sizeParsed.ptr != sizeText.data() + sizeText.size())
				return std::nullopt;
			parsed.Asset = AssetHandle(*asset);
			return parsed;
		}

	}

	struct ThumbnailCache::State
	{
		// The load state of one input when a result was produced; any later difference makes the result stale.
		struct Input
		{
			AssetHandle Handle{};
			uint64_t Version = 0;
			AssetState LoadState = AssetState::Unloaded;
		};
		struct Entry
		{
			Result<ThumbnailResult> Value = ThumbnailResult{};
			uint64_t Version = 0; // the asset's version when the result was produced
			std::vector<Input> Inputs{};
			uint64_t Quality = 0; // QualityKey when the result was produced
			bool Revalidate = false;
		};

		EditorContext& Context; // Back-reference: context outlives the cache.
		ThumbnailRenderer Renderer{};
		uint64_t Generation = 0;
		uint64_t LastGeneration = 0;
		std::filesystem::path ProjectFile{};
		std::filesystem::path CachePath{};
		std::filesystem::path CanonicalRoot{};
		bool Rendering = false;
		static constexpr uint32_t RendererFormat = 2;
		static constexpr size_t MaxPumpVisits = 64;
		using Key = std::pair<AssetHandle, uint32_t>;
		// The enclosing binding supplies generation, canonical root and renderer format to every key.
		std::map<Key, Entry> Results{};
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

		[[nodiscard]] static bool IsVisual(AssetType type)
		{
			return type == AssetType::Mesh || type == AssetType::Material || type == AssetType::Prefab || type == AssetType::Environment
				|| type == AssetType::Texture;
		}

		// The visual types the host captures through the scene renderer, whose images depend on the render quality; a
		// texture's preview is its decoded pixels.
		[[nodiscard]] static bool IsCaptured(AssetType type)
		{
			return IsVisual(type) && type != AssetType::Texture;
		}

		// The render quality captures use: the bound project's Rendering settings. Every field reaching the captures belongs
		// in GetRenderQuality and in QualityKey.
		[[nodiscard]] RenderQualitySettings GetRenderQuality() const
		{
			if (!Context.HasProject())
				return {};
			const RenderingSettings& rendering = Context.GetProject().GetSettings().Rendering;
			return { .ShadowMapSize = rendering.ShadowMapSize, .SsaoHalfResolution = rendering.SsaoHalfResolution };
		}

		// What an image of `asset` depends on besides its asset inputs: the render quality for a captured type, 0 otherwise.
		[[nodiscard]] uint64_t QualityKey(AssetHandle asset) const
		{
			if (!IsCaptured(Context.GetAssets().GetAssetType(asset)))
				return 0;
			const RenderQualitySettings quality = GetRenderQuality();
			return (static_cast<uint64_t>(quality.ShadowMapSize) << 2) | (quality.SsaoHalfResolution ? 2u : 0u) | 1u;
		}

		// The asset and everything it references directly or through others, sorted. Imports record their references
		// under the source handle, which a sub-asset therefore shares.
		[[nodiscard]] std::vector<AssetHandle> FindInputs(AssetHandle asset) const
		{
			const EditorAssetManager& assets = Context.GetAssets();
			std::set<AssetHandle> visited;
			std::vector<AssetHandle> open{ asset };
			while (!open.empty())
			{
				const AssetHandle handle = open.back();
				open.pop_back();
				if (!handle.IsValid() || !visited.insert(handle).second)
					continue;
				for (const AssetHandle reference : assets.GetDependencyGraph().GetDependencies(handle))
					open.push_back(reference);
				if (const auto location = assets.GetRegistry().Locate(handle); location && location->Record->Metadata.Handle != handle)
					for (const AssetHandle reference : assets.GetDependencyGraph().GetDependencies(location->Record->Metadata.Handle))
						open.push_back(reference);
			}
			return { visited.begin(), visited.end() };
		}

		[[nodiscard]] std::vector<Input> Snapshot(std::span<const AssetHandle> handles) const
		{
			const EditorAssetManager& assets = Context.GetAssets();
			std::vector<Input> inputs;
			inputs.reserve(handles.size());
			for (const AssetHandle handle : handles)
				inputs.push_back({ .Handle = handle, .Version = assets.GetVersion(handle), .LoadState = assets.GetState(handle) });
			return inputs;
		}

		[[nodiscard]] bool IsCurrent(const Entry& entry, const ThumbnailRequest& request) const
		{
			if (entry.Revalidate || entry.Version != request.Version || entry.Quality != QualityKey(request.Asset))
				return false;
			const EditorAssetManager& assets = Context.GetAssets();
			return std::ranges::all_of(entry.Inputs, [&assets](const Input& input)
			{
				return assets.GetVersion(input.Handle) == input.Version && assets.GetState(input.Handle) == input.LoadState;
			});
		}

		void HashFileStamp(XXH64Hasher& hasher, const VfsPath& path) const
		{
			// A missing or unreadable file hashes differently from every existing one, so its appearance changes the digest.
			const Result<FileInfo> info = Context.GetVfs().GetInfo(path);
			hasher.UpdateU64(info.has_value() ? 1 : 0);
			if (info.has_value())
			{
				hasher.UpdateU64(info->Size);
				hasher.UpdateU64(info->ModificationTime);
			}
		}

		// The content identity of an image of `asset` at `size` rendered from `inputs` (sorted): see the header.
		[[nodiscard]] uint64_t ComputeDigest(AssetHandle asset, uint32_t size, std::span<const AssetHandle> inputs) const
		{
			const EditorAssetManager& assets = Context.GetAssets();
			const AssetRegistry& registry = assets.GetRegistry();
			XXH64Hasher hasher;
			hasher.UpdateU64(RendererFormat);
			hasher.UpdateU64(EngineCookVersion);
			hasher.UpdateU64(asset.GetValue());
			hasher.UpdateU64(size);
			hasher.UpdateU64(QualityKey(asset));
			for (const AssetHandle input : inputs)
			{
				hasher.UpdateU64(input.GetValue());
				const auto location = registry.Locate(input);
				const AssetRecord* record = location ? location->Record : registry.Find(input);
				if (record == nullptr)
				{
					// Built-ins change only with the engine (RendererFormat, EngineCookVersion); unknown handles are missing.
					hasher.UpdateU64(assets.GetAssetType(input) != AssetType::None ? 1 : 0);
					continue;
				}
				hasher.UpdateU64(2);
				HashFileStamp(hasher, record->SourcePath);
				HashFileStamp(hasher, record->MetaPath);
				for (const AssetRecord* dependency : registry.GetDependencyRecords(record->Metadata.Handle))
				{
					HashFileStamp(hasher, dependency->SourcePath);
					HashFileStamp(hasher, dependency->MetaPath);
				}
			}
			return hasher.Digest();
		}

		// <canonical root>/Thumbnails, confined to the bound root. Conflict when the root moved since binding.
		[[nodiscard]] Result<std::filesystem::path> GetDirectory() const
		{
			std::error_code error;
			const auto root = std::filesystem::canonical(CachePath, error);
			if (error || root != CanonicalRoot)
				return MakeError(ErrorCode::Conflict, "thumbnail cache root changed since binding");
			const auto directory = root / Utils::ThumbnailDirectoryName;
			const auto resolved = std::filesystem::weakly_canonical(directory, error);
			if (error || resolved != directory)
				return MakeError(ErrorCode::Io, "the thumbnail directory resolves outside its bound cache");
			return directory;
		}

		struct DiskEntry
		{
			uint64_t Digest = 0;
			std::vector<AssetHandle> Inputs{};
		};

		// The manifest of `asset` at `size`; nullopt when absent or not one this version wrote (it is then replaced).
		[[nodiscard]] static std::optional<DiskEntry> ReadManifest(const std::filesystem::path& file, AssetHandle asset, uint32_t size)
		{
			if (!FileSystem::Exists(file))
				return std::nullopt;
			const Result<std::string> text = FileSystem::ReadText(file);
			if (!text.has_value())
				return std::nullopt;
			const Result<Json> document = JsonReader::Parse(*text);
			if (!document.has_value())
				return std::nullopt;
			const JsonReader reader(*document);
			const Result<UUID> manifestAsset = reader.ReadMember<UUID>("Asset");
			const Result<uint32_t> manifestSize = reader.ReadMember<uint32_t>("Size");
			const Result<uint32_t> format = reader.ReadMember<uint32_t>("RendererFormat");
			const Result<std::string> digest = reader.ReadMember<std::string>("Digest");
			const std::optional<JsonReader> inputs = reader.FindMember("Inputs");
			if (!reader.ReadFormatHeader(Utils::ThumbnailManifestFormat, Utils::ThumbnailManifestVersion, Utils::ThumbnailManifestVersion).has_value()
				|| !manifestAsset || *manifestAsset != asset || !manifestSize || *manifestSize != size || !format || *format != RendererFormat
				|| !digest || !inputs)
				return std::nullopt;
			DiskEntry entry;
			const std::optional<uint64_t> parsed = Utils::ParseThumbnailHex(*digest);
			const Result<size_t> count = inputs->GetArraySize();
			if (!parsed || !count)
				return std::nullopt;
			entry.Digest = *parsed;
			for (size_t index = 0; index < *count; ++index)
			{
				const Result<JsonReader> element = inputs->GetElement(index);
				if (!element)
					return std::nullopt;
				const Result<UUID> handle = element->ReadUUID();
				if (!handle.has_value() || !handle->IsValid())
					return std::nullopt;
				entry.Inputs.push_back(*handle);
			}
			std::ranges::sort(entry.Inputs);
			return entry;
		}

		// A cached image whose inputs still have the files it was rendered from; nullopt to render.
		[[nodiscard]] std::optional<ThumbnailResult> FindOnDisk(const std::filesystem::path& directory, const ThumbnailRequest& request,
			std::vector<AssetHandle>& inputs) const
		{
			const std::string stem = Utils::ThumbnailStem(request.Asset, request.Size);
			const std::optional<DiskEntry> manifest = ReadManifest(directory / (stem + ".json"), request.Asset, request.Size);
			if (!manifest || !std::ranges::binary_search(manifest->Inputs, request.Asset))
				return std::nullopt;
			if (ComputeDigest(request.Asset, request.Size, manifest->Inputs) != manifest->Digest)
				return std::nullopt;
			const auto file = directory / Utils::ThumbnailImageName(request.Asset, request.Size, manifest->Digest);
			const Result<Image> image = ReadPng(file);
			if (!image.has_value() || image->Width != request.Size || image->Height != request.Size)
				return std::nullopt;
			inputs = manifest->Inputs;
			return ThumbnailResult{ .Path = FileSystem::PathToUtf8(file), .Cached = true };
		}

		// A type icon or a reusable disk entry, without loading or rendering; nullopt when a render is needed.
		// `inputs` receives what the result depends on.
		[[nodiscard]] Result<std::optional<ThumbnailResult>> FindWithoutRendering(const ThumbnailRequest& request, std::vector<AssetHandle>& inputs) const
		{
			ENGINE_TRY(Validate(request));
			inputs = { request.Asset };
			ENGINE_TRY_ASSIGN(const std::filesystem::path directory, GetDirectory());
			if (!IsVisual(Context.GetAssets().GetAssetType(request.Asset)))
				return std::optional(ThumbnailResult{ .TypeIcon = true });
			return FindOnDisk(directory, request, inputs);
		}

		// Writes the image and its manifest, then removes the asset's previous image at this size.
		[[nodiscard]] Status Publish(const std::filesystem::path& directory, const ThumbnailRequest& request, const Image& image,
			std::span<const AssetHandle> inputs, uint64_t digest, const std::filesystem::path& file) const
		{
			const std::string stem = Utils::ThumbnailStem(request.Asset, request.Size);
			const auto manifestFile = directory / (stem + ".json");
			const std::optional<DiskEntry> previous = ReadManifest(manifestFile, request.Asset, request.Size);
			ENGINE_TRY(FileSystem::CreateDirectories(directory));
			ENGINE_TRY_ASSIGN(const Buffer png, EncodePng(image));
			ENGINE_TRY(FileSystem::WriteFileAtomic(file, png, AtomicWriteOptions{ .KeepBackup = false }));
			Json manifest = Json::object();
			manifest["Format"] = std::string(Utils::ThumbnailManifestFormat);
			manifest["Version"] = Utils::ThumbnailManifestVersion;
			manifest["Asset"] = request.Asset.ToString();
			manifest["Size"] = request.Size;
			manifest["RendererFormat"] = RendererFormat;
			manifest["Digest"] = std::format("{:016x}", digest);
			manifest["Inputs"] = Json::array();
			for (const AssetHandle input : inputs)
				manifest["Inputs"].push_back(input.ToString());
			ENGINE_TRY_ASSIGN(const std::string text, JsonWriter::Write(manifest));
			ENGINE_TRY(FileSystem::WriteFileAtomic(manifestFile, std::as_bytes(std::span(text.data(), text.size())), AtomicWriteOptions{ .KeepBackup = false }));
			if (previous && previous->Digest != digest)
			{
				const auto old = directory / Utils::ThumbnailImageName(request.Asset, request.Size, previous->Digest);
				if (const Status removed = FileSystem::Remove(old); !removed && removed.error().GetCode() != ErrorCode::NotFound)
					ENGINE_WARN("Cannot remove the replaced thumbnail '{}': {}", FileSystem::PathToUtf8(old), removed.error());
			}
			return {};
		}

		// Renders through the injected renderer and publishes the image. `inputs` receives what it depends on.
		[[nodiscard]] Result<ThumbnailResult> Render(const ThumbnailRequest& request, std::vector<AssetHandle>& inputs)
		{
			ENGINE_TRY(Validate(request));
			ENGINE_TRY_ASSIGN(const std::filesystem::path directory, GetDirectory());
			inputs = FindInputs(request.Asset);
			if (!Renderer)
				return MakeError(ErrorCode::Unsupported, "visual thumbnails require an injected renderer");
			Rendering = true;
			Result<Image> rendered = Renderer(request);
			Rendering = false;
			// Rendering may load references the graph did not know yet.
			inputs = FindInputs(request.Asset);
			if (!rendered)
				return std::unexpected(std::move(rendered).error());
			ENGINE_TRY(Validate(request));
			if (!rendered->IsValid() || rendered->Width != request.Size || rendered->Height != request.Size)
				return MakeError(ErrorCode::Validation, "thumbnail renderer must return a valid square image of the requested size");
			// Recheck confinement after the injected callback, before publishing any bytes.
			ENGINE_TRY_ASSIGN(const std::filesystem::path after, GetDirectory());
			if (after != directory)
				return MakeError(ErrorCode::Conflict, "thumbnail cache root changed while rendering");
			const uint64_t digest = ComputeDigest(request.Asset, request.Size, inputs);
			const auto file = directory / Utils::ThumbnailImageName(request.Asset, request.Size, digest);
			ENGINE_TRY(Publish(directory, request, *rendered, inputs, digest, file));
			return ThumbnailResult{ .Path = FileSystem::PathToUtf8(file) };
		}

		void Store(const ThumbnailRequest& request, Result<ThumbnailResult> value, std::span<const AssetHandle> inputs)
		{
			Results.insert_or_assign(Key{ request.Asset, request.Size }, Entry{
																			 .Value = std::move(value),
																			 .Version = request.Version,
																			 .Inputs = Snapshot(inputs),
																			 .Quality = QualityKey(request.Asset),
																			 .Revalidate = false,
																		 });
		}

		// The handles a render needs loaded that are not: Unloaded ones start loading. True when everything is loaded or
		// failed (a failed reference renders with its placeholder), false while any load is in flight.
		[[nodiscard]] bool LoadInputs(AssetHandle asset)
		{
			EditorAssetManager& assets = Context.GetAssets();
			bool ready = true;
			for (const AssetHandle handle : FindInputs(asset))
			{
				if (assets.GetAssetType(handle) == AssetType::None)
					continue; // a missing reference renders with its placeholder
				const AssetState state = assets.GetState(handle);
				if (state == AssetState::Unloaded)
				{
					// The manager publishes the result on the main thread; the next visit sees Loaded or Failed.
					static_cast<void>(assets.LoadAsync(handle));
					ready = false;
				}
				else if (state == AssetState::Loading)
					ready = false;
			}
			return ready;
		}

		// Removes images no manifest names, manifests of unregistered assets with their images, and the root-level files
		// of the per-session naming (renderer format 1). Best effort: failures are logged once per sweep.
		void Sweep()
		{
			std::optional<Error> failure;
			const auto remove = [&failure](const std::filesystem::path& file)
			{
				if (const Status removed = FileSystem::Remove(file); !removed && removed.error().GetCode() != ErrorCode::NotFound && !failure)
					failure = removed.error();
			};
			if (const auto legacy = FileSystem::ListDirectory(CanonicalRoot); legacy.has_value())
			{
				for (const auto& file : *legacy)
					if (const std::string name = FileSystem::PathToUtf8(file.filename()); name.starts_with("thumbnail-v1-") && name.ends_with(".png"))
						remove(file);
			}
			const auto directory = CanonicalRoot / Utils::ThumbnailDirectoryName;
			const auto listed = FileSystem::ListDirectory(directory);
			if (!listed.has_value())
			{
				if (listed.error().GetCode() != ErrorCode::NotFound)
					ENGINE_WARN("Cannot clean the thumbnail cache: {}", listed.error());
				return;
			}
			const EditorAssetManager& assets = Context.GetAssets();
			std::map<Key, uint64_t> current;
			for (const auto& file : *listed)
			{
				const std::optional<Utils::ThumbnailFileName> name = Utils::ParseThumbnailFileName(FileSystem::PathToUtf8(file.filename()));
				if (!name || name->Digest)
					continue;
				const std::optional<DiskEntry> manifest = ReadManifest(file, name->Asset, name->Size);
				if (!manifest || assets.GetAssetType(name->Asset) == AssetType::None)
					remove(file);
				else
					current.emplace(Key{ name->Asset, name->Size }, manifest->Digest);
			}
			for (const auto& file : *listed)
			{
				const std::optional<Utils::ThumbnailFileName> name = Utils::ParseThumbnailFileName(FileSystem::PathToUtf8(file.filename()));
				if (!name || !name->Digest)
					continue;
				const auto kept = current.find(Key{ name->Asset, name->Size });
				if (kept == current.end() || kept->second != *name->Digest)
					remove(file);
			}
			if (failure)
				ENGINE_WARN("Cannot clean the thumbnail cache: {}", *failure);
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
		m_State->ProjectFile = project.GetProjectFile();
		m_State->CachePath = project.GetCacheDirectory();
		m_State->CanonicalRoot = std::move(root);
		m_State->Generation = projectGeneration;
		m_State->LastGeneration = projectGeneration;
		m_State->Sweep();
		return {};
	}

	uint64_t ThumbnailCache::GetProjectGeneration() const
	{
		return m_State->Generation;
	}

	RenderQualitySettings ThumbnailCache::GetRenderQuality() const
	{
		return m_State->GetRenderQuality();
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
		if (const auto found = m_State->Results.find(State::Key{ request.Asset, request.Size });
			found != m_State->Results.end() && m_State->IsCurrent(found->second, request))
		{
			auto result = found->second.Value;
			if (result && !result->TypeIcon)
				result->Cached = true;
			return result;
		}
		std::vector<AssetHandle> inputs;
		Result<std::optional<ThumbnailResult>> found = m_State->FindWithoutRendering(request, inputs);
		Result<ThumbnailResult> result = !found ? Result<ThumbnailResult>(std::unexpected(std::move(found).error()))
			: found->has_value()                ? Result<ThumbnailResult>(std::move(**found))
												: m_State->Render(request, inputs);
		ENGINE_TRY(m_State->Validate(request));
		m_State->Store(request, result, inputs);
		return result;
	}

	Status ThumbnailCache::Queue(const ThumbnailRequest& request)
	{
		ENGINE_TRY(m_State->Validate(request));
		if (const auto found = m_State->Results.find(State::Key{ request.Asset, request.Size });
			found != m_State->Results.end() && m_State->IsCurrent(found->second, request))
			return {};
		const auto queued = std::ranges::find_if(m_State->Pending, [&request](const ThumbnailRequest& pending)
		{
			return pending.Asset == request.Asset && pending.Size == request.Size;
		});
		if (queued != m_State->Pending.end())
			queued->Version = request.Version;
		else
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
		const EditorAssetManager& assets = m_State->Context.GetAssets();
		std::optional<Error> failure;
		uint32_t produced = 0;
		// Each queued request is visited at most once, and at most MaxPumpVisits (or maxItems) of them per safe point.
		const size_t budget = std::min(m_State->Pending.size(), std::max<size_t>(maxItems, State::MaxPumpVisits));
		for (size_t visits = budget; visits > 0 && produced < maxItems && !m_State->Pending.empty(); --visits)
		{
			ThumbnailRequest request = m_State->Pending.front();
			m_State->Pending.pop_front();
			if (request.ProjectGeneration != m_State->Generation)
				continue;
			// Deleting an asset is no error: its queued work and result go silently.
			if (assets.GetAssetType(request.Asset) == AssetType::None)
			{
				m_State->Results.erase(State::Key{ request.Asset, request.Size });
				continue;
			}
			// A first load or reload since queueing is no error either: the asset is processed as it is now.
			request.Version = assets.GetVersion(request.Asset);
			if (const auto found = m_State->Results.find(State::Key{ request.Asset, request.Size });
				found != m_State->Results.end() && m_State->IsCurrent(found->second, request))
				continue;
			std::vector<AssetHandle> inputs;
			Result<std::optional<ThumbnailResult>> found = m_State->FindWithoutRendering(request, inputs);
			if (found && !found->has_value() && !m_State->LoadInputs(request.Asset))
			{
				m_State->Pending.push_back(request); // revisited once its loads were published
				continue;
			}
			request.Version = assets.GetVersion(request.Asset);
			Result<ThumbnailResult> result = !found ? Result<ThumbnailResult>(std::unexpected(std::move(found).error()))
				: found->has_value()                ? Result<ThumbnailResult>(std::move(**found))
													: m_State->Render(request, inputs);
			++produced;
			if (const Status valid = m_State->Validate(request); !valid)
			{
				if (!failure && valid.error().GetCode() != ErrorCode::Conflict && valid.error().GetCode() != ErrorCode::NotFound)
					failure = valid.error();
				continue; // the asset changed or went while rendering; a later Draw queues it again
			}
			if (!result && !failure)
				failure = result.error();
			m_State->Store(request, std::move(result), inputs);
		}
		return failure ? Status(std::unexpected(*failure)) : Status{};
	}

	Result<std::optional<ThumbnailResult>> ThumbnailCache::Find(const ThumbnailRequest& request) const
	{
		ENGINE_TRY(m_State->Validate(request));
		const auto found = m_State->Results.find(State::Key{ request.Asset, request.Size });
		if (found == m_State->Results.end())
			return std::nullopt;
		const bool current = m_State->IsCurrent(found->second, request);
		if (!found->second.Value)
		{
			if (!current)
				return std::nullopt;
			return std::unexpected(found->second.Value.error());
		}
		ThumbnailResult result = *found->second.Value;
		result.Stale = !current;
		return result;
	}

	void ThumbnailCache::Invalidate(AssetHandle asset)
	{
		std::erase_if(m_State->Results, [asset](const auto& entry)
		{
			return !asset.IsValid() || entry.first.first == asset;
		});
		std::erase_if(m_State->Pending, [asset](const ThumbnailRequest& request)
		{
			return !asset.IsValid() || request.Asset == asset;
		});
	}

	void ThumbnailCache::Revalidate()
	{
		for (auto& entry : m_State->Results)
			entry.second.Revalidate = true;
	}

}
