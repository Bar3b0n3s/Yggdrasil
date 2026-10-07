#include "EnginePCH.h"
#include "Engine/Renderer/GpuResourceCache.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include <compare>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Every mirror is an entry keyed by (handle, version). An asset that is missing, failed to load or has another type gets
// an entry that shares the placeholder's GPU objects (IsPlaceholder; AssetManager::GetOrPlaceholder reported the problem).
// An asset whose upload failed gets one too, and the entry remembers the failure, so the upload is not retried every frame
// and the diagnostic is reported once; a new version of the asset is a new entry and tries again. NVRHI handles are
// reference-counted, so dropping an entry releases its objects only when no other entry shares them, and NVRHI defers the
// destruction until the command lists using them have completed (§8.14 item 2).

namespace Engine {

	namespace {

		struct MirrorKey
		{
			AssetHandle Handle{};
			uint64_t Version = 0;

			std::strong_ordering operator<=>(const MirrorKey&) const = default;
			bool operator==(const MirrorKey&) const = default;
		};

		// The services an upload and its failure report need.
		struct UploadContext
		{
			GraphicsDevice* Device = nullptr; // documented back-references of the cache
			AssetManager* Assets = nullptr;
			size_t* UploadFailures = nullptr;
		};

	}

	namespace Utils {

		// The readable name of `handle` for debug names and diagnostics: its reference path, or its 16 hex digits.
		static std::string GetAssetName(const AssetManager& assets, AssetHandle handle)
		{
			std::string path = assets.GetReferencePath(handle);
			return path.empty() ? handle.ToString() : path;
		}

		static Result<nvrhi::Format> ToNvrhiFormat(TextureFormat format)
		{
			switch (format)
			{
				case TextureFormat::RGBA8Unorm: return nvrhi::Format::RGBA8_UNORM;
				case TextureFormat::RGBA8Srgb:  return nvrhi::Format::SRGBA8_UNORM;
				case TextureFormat::R8Unorm:    return nvrhi::Format::R8_UNORM;
			}
			return MakeError(ErrorCode::InvalidArgument, "unknown texture format {}", std::to_underlying(format));
		}

		// Creates a vertex or index buffer of `size` bytes, kept in its shader-readable state between command lists, and
		// records its upload into `commandList`.
		static Result<nvrhi::BufferHandle> CreateGeometryBuffer(GraphicsDevice& device, nvrhi::ICommandList& commandList, const void* data,
			size_t size, bool isIndexBuffer, std::string debugName)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = size;
			desc.isVertexBuffer = !isIndexBuffer;
			desc.isIndexBuffer = isIndexBuffer;
			desc.initialState = isIndexBuffer ? nvrhi::ResourceStates::IndexBuffer : nvrhi::ResourceStates::VertexBuffer;
			desc.keepInitialState = true;
			desc.debugName = std::move(debugName);
			ENGINE_TRY_ASSIGN(nvrhi::BufferHandle buffer, device.CreateBuffer(desc));
			commandList.writeBuffer(buffer, data, size);
			return buffer;
		}

	}

	namespace {

		// The mesh half of the cache: a vertex and an index buffer filled by one command list (§8.14 item 1).
		struct MeshTraits
		{
			using Data = MeshData;
			using Mirror = GpuMesh;
			static constexpr std::string_view Kind = "mesh";

			[[nodiscard]] static bool HasGpuObjects(const GpuMesh& mirror) { return mirror.VertexBuffer != nullptr && mirror.IndexBuffer != nullptr; }

			[[nodiscard]] static Result<GpuMesh> Upload(GraphicsDevice& device, const MeshData& mesh, const std::string& name)
			{
				if (mesh.Vertices.empty() || mesh.Indices.empty())
					return MakeError(ErrorCode::InvalidArgument, "mesh '{}' has no vertices or no indices", name);
				ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle commandList, device.CreateCommandList());
				commandList->open();
				Result<nvrhi::BufferHandle> vertices = Utils::CreateGeometryBuffer(device, *commandList, mesh.Vertices.data(),
					mesh.Vertices.size() * sizeof(MeshVertex), false, std::format("{} (vertices)", name));
				Result<nvrhi::BufferHandle> indices = vertices
					? Utils::CreateGeometryBuffer(device, *commandList, mesh.Indices.data(), mesh.Indices.size() * sizeof(uint32_t), true,
						  std::format("{} (indices)", name))
					: Result<nvrhi::BufferHandle>(nvrhi::BufferHandle());
				// The list is closed either way; it is executed only when both buffers exist.
				commandList->close();
				if (!vertices)
					return std::unexpected(std::move(vertices).error());
				if (!indices)
					return std::unexpected(std::move(indices).error());
				device.ExecuteCommandList(*commandList);

				GpuMesh mirror;
				mirror.VertexBuffer = std::move(*vertices);
				mirror.IndexBuffer = std::move(*indices);
				mirror.VertexCount = static_cast<uint32_t>(mesh.Vertices.size());
				mirror.IndexCount = static_cast<uint32_t>(mesh.Indices.size());
				mirror.Submeshes.reserve(mesh.Submeshes.size());
				for (const MeshSubmesh& submesh : mesh.Submeshes)
				{
					mirror.Submeshes.push_back({
						.IndexOffset = submesh.IndexOffset,
						.IndexCount = submesh.IndexCount,
						.MaterialSlot = submesh.MaterialSlot,
						.Bounds = submesh.Bounds,
					});
				}
				mirror.DefaultMaterials.reserve(mesh.Slots.size());
				for (const MeshMaterialSlot& slot : mesh.Slots)
					mirror.DefaultMaterials.push_back(slot.DefaultMaterial);
				mirror.Bounds = mesh.Bounds;
				return mirror;
			}
		};

		// The texture half: an immutable sampled texture with every mip, through HostImageUpload (host image copy where the
		// device allows it, staging otherwise).
		struct TextureTraits
		{
			using Data = TextureData;
			using Mirror = GpuTexture;
			static constexpr std::string_view Kind = "texture";

			[[nodiscard]] static bool HasGpuObjects(const GpuTexture& mirror) { return mirror.Texture != nullptr; }

			[[nodiscard]] static Result<GpuTexture> Upload(GraphicsDevice& device, const TextureData& texture, const std::string& name)
			{
				if (texture.Mips.empty())
					return MakeError(ErrorCode::InvalidArgument, "texture '{}' has no mip levels", name);
				ENGINE_TRY_ASSIGN(const nvrhi::Format format, Utils::ToNvrhiFormat(texture.Format));
				nvrhi::TextureDesc desc;
				desc.width = texture.Width;
				desc.height = texture.Height;
				desc.mipLevels = static_cast<uint32_t>(texture.Mips.size());
				desc.format = format;
				desc.dimension = nvrhi::TextureDimension::Texture2D;
				desc.debugName = name;
				std::vector<TextureSubresourceData> subresources;
				subresources.reserve(texture.Mips.size());
				for (uint32_t level = 0; level < desc.mipLevels; ++level)
				{
					subresources.push_back({
						.MipLevel = level,
						.ArraySlice = 0,
						.Data = GetMipPixels(texture, level),
						.RowPitch = 0,
						.DepthPitch = 0,
					});
				}
				ENGINE_TRY_ASSIGN(TextureUpload upload, device.GetHostImageUpload().CreateTexture(desc, subresources));
				GpuTexture mirror;
				mirror.Texture = std::move(upload.Texture);
				mirror.Path = upload.Path;
				return mirror;
			}
		};

		// The mirrors of one asset type, keyed by (handle, version).
		template<typename Traits>
		class MirrorTable
		{
		public:
			using Data = typename Traits::Data;
			using Mirror = typename Traits::Mirror;

			// The mirror of `handle` at its current version; null gives the placeholder's own mirror without a diagnostic
			// (GpuResourceCache.h).
			[[nodiscard]] const Mirror& Get(const UploadContext& context, AssetHandle handle)
			{
				if (!handle.IsValid())
					return Resolve(context, GetPlaceholderHandle(Data::StaticType), false);
				return Resolve(context, handle, true);
			}

			void CollectStale(const AssetManager& assets, bool releaseUnused)
			{
				std::erase_if(m_Entries, [&assets, releaseUnused](const auto& item)
				{
					return assets.GetVersion(item.first.Handle) != item.first.Version || (releaseUnused && !item.second.IsUsed);
				});
				for (auto& [key, entry] : m_Entries)
					entry.IsUsed = false;
			}

			void Clear() { m_Entries.clear(); }

			[[nodiscard]] size_t GetCount() const { return m_Entries.size(); }
		private:
			struct Entry
			{
				Mirror Value{};
				std::string UploadError{}; // why this asset's own upload failed; empty when it did not
				bool IsUsed = false;       // got through Get since the last CollectStale
				bool IsReported = false;   // the upload failure was reported to the asset manager
			};
		private:
			// `isDirect`: the caller asked for `handle` itself, not for the placeholder of another asset; only a direct use
			// reports an upload failure under the asset's own handle.
			const Mirror& Resolve(const UploadContext& context, AssetHandle handle, bool isDirect)
			{
				AssetManager& assets = *context.Assets;
				const AssetRef<Data> data = assets.GetOrPlaceholder<Data>(handle);
				const MirrorKey key = { .Handle = handle, .Version = assets.GetVersion(handle) };
				if (const auto found = m_Entries.find(key); found != m_Entries.end())
				{
					Entry& entry = found->second;
					entry.IsUsed = true;
					if (entry.Value.IsPlaceholder)
						MarkPlaceholderUsed(assets);
					if (isDirect)
						ReportFailure(context, handle, entry);
					return entry.Value;
				}

				const AssetHandle placeholder = GetPlaceholderHandle(Data::StaticType);
				const bool isPlaceholderAsset = handle == placeholder;
				Entry entry;
				entry.IsUsed = true;
				if (!isPlaceholderAsset && data == AssetCast<Data>(assets.GetPlaceholder(Data::StaticType)))
				{
					// Missing, failed or of another type: GetOrPlaceholder recorded the diagnostic.
					entry.Value = Resolve(context, placeholder, false);
					entry.Value.IsPlaceholder = true;
				}
				else
				{
					const std::string name = Utils::GetAssetName(assets, handle);
					Result<Mirror> uploaded = Traits::Upload(*context.Device, *data, name);
					if (uploaded)
					{
						entry.Value = std::move(*uploaded);
					}
					else
					{
						// The placeholder's mirror stands in; when the placeholder's own upload failed, its GPU objects stay null.
						++*context.UploadFailures;
						entry.UploadError = uploaded.error().ToString();
						if (!isPlaceholderAsset)
							entry.Value = Resolve(context, placeholder, false);
						entry.Value.IsPlaceholder = true;
					}
				}
				entry.Value.Version = key.Version;
				const auto inserted = m_Entries.insert_or_assign(key, std::move(entry)).first;
				if (isDirect)
					ReportFailure(context, handle, inserted->second);
				return inserted->second.Value;
			}

			void MarkPlaceholderUsed(const AssetManager& assets)
			{
				const AssetHandle placeholder = GetPlaceholderHandle(Data::StaticType);
				if (const auto found = m_Entries.find({ .Handle = placeholder, .Version = assets.GetVersion(placeholder) }); found != m_Entries.end())
					found->second.IsUsed = true;
			}

			// Reports the failed upload of `handle` once (§8.14 item 7): ASSET_UPLOAD_FAILED, Error, logged once by the asset
			// manager.
			void ReportFailure(const UploadContext& context, AssetHandle handle, Entry& entry)
			{
				if (entry.UploadError.empty() || entry.IsReported)
					return;
				entry.IsReported = true;
				AssetManager& assets = *context.Assets;
				const std::string name = Utils::GetAssetName(assets, handle);
				const AssetHandle placeholder = GetPlaceholderHandle(Data::StaticType);
				const std::string placeholderName = Utils::GetAssetName(assets, placeholder);
				std::string fallback;
				if (handle == placeholder)
					fallback = std::format("it is the {} placeholder, so draws that need it are skipped", Traits::Kind);
				else if (!Traits::HasGpuObjects(entry.Value))
					fallback = std::format("its placeholder '{}' could not be uploaded either, so draws using it are skipped", placeholderName);
				else
					fallback = std::format("its placeholder '{}' is drawn instead", placeholderName);

				// The failure belongs to this process's device, not to the project: the code is runtime-only and does not
				// count for AssetManager::HasErrorDiagnostics.
				AssetDiagnostic diagnostic;
				diagnostic.Severity = DiagnosticSeverity::Error;
				diagnostic.Code = std::string(AssetUploadFailedCode);
				diagnostic.Asset = handle;
				diagnostic.Path = name.substr(0, name.find('#'));
				diagnostic.Message = std::format("cannot upload {} '{}' to the GPU: {}; {}", Traits::Kind, name, entry.UploadError, fallback);
				diagnostic.Hint = "the upload is tried again when the asset changes; when the device is out of memory, use smaller or fewer "
								  "textures";
				assets.ReportDiagnostic(std::move(diagnostic));
			}
		private:
			std::map<MirrorKey, Entry> m_Entries; // node-based: a returned reference survives insertions
		};

	}

	struct GpuResourceCache::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		AssetManager* Assets = nullptr;   // documented back-reference
		MirrorTable<MeshTraits> Meshes;
		MirrorTable<TextureTraits> Textures;
		size_t UploadFailures = 0;

		[[nodiscard]] UploadContext GetContext() { return { .Device = Device, .Assets = Assets, .UploadFailures = &UploadFailures }; }
	};

	GpuResourceCache::GpuResourceCache(GraphicsDevice& device, AssetManager& assets)
		: m_State(CreateScope<State>())
	{
		m_State->Device = &device;
		m_State->Assets = &assets;
	}

	GpuResourceCache::~GpuResourceCache() = default;

	const GpuMesh& GpuResourceCache::GetMesh(AssetHandle handle)
	{
		return m_State->Meshes.Get(m_State->GetContext(), handle);
	}

	const GpuTexture& GpuResourceCache::GetTexture(AssetHandle handle)
	{
		return m_State->Textures.Get(m_State->GetContext(), handle);
	}

	void GpuResourceCache::CollectStale(bool releaseUnused)
	{
		// A mirror whose version differs from the asset's current one is stale; besides older versions, that covers the
		// versions a dry run published and then rolled back (AssetManager::RestoreSharedState).
		m_State->Meshes.CollectStale(*m_State->Assets, releaseUnused);
		m_State->Textures.CollectStale(*m_State->Assets, releaseUnused);
	}

	void GpuResourceCache::Clear()
	{
		m_State->Meshes.Clear();
		m_State->Textures.Clear();
	}

	GpuResourceCacheStats GpuResourceCache::GetStats() const
	{
		return {
			.MeshCount = m_State->Meshes.GetCount(),
			.TextureCount = m_State->Textures.GetCount(),
			.UploadFailures = m_State->UploadFailures,
		};
	}

}
