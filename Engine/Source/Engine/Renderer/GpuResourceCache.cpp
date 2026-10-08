#include "EnginePCH.h"
#include "Engine/Renderer/GpuResourceCache.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Shared/MaterialConstants.h"

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <format>
#include <map>
#include <span>
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
//
// Materials (M8) are keyed the same way, but a mirror also depends on its textures: every GetMaterial resolves the five
// slots through GetTexture (which marks those mirrors used and uploads a texture's new version), and a mirror whose resolved
// textures changed takes them with a new Generation, keeping its constants buffer (the constants depend on the material's
// version only, which is the key). Generations come from one counter of the cache that only increases. Environments (M8)
// have no placeholder (§7.2): an entry remembers a failed load or upload for its (handle, version), so the failure is
// reported once and nothing is retried until the asset's version changes.

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

		// The constants of `material` (Shared/MaterialConstants.h).
		static MaterialConstants MakeMaterialConstants(const MaterialData& material)
		{
			MaterialConstants constants{};
			constants.BaseColor = material.BaseColor;
			constants.Emissive = material.Emissive * material.EmissiveStrength;
			constants.Metallic = material.Metallic;
			constants.Roughness = material.Roughness;
			constants.NormalScale = material.NormalScale;
			constants.OcclusionStrength = material.OcclusionStrength;
			constants.AlphaCutoff = material.AlphaCutoff;
			constants.UVScale = material.UVScale;
			constants.UVOffset = material.UVOffset;
			constants.AlphaMode = static_cast<uint32_t>(std::to_underlying(material.AlphaMode));
			constants.Flags = material.EmissiveMap.IsValid() ? MaterialFlagEmissiveMap : 0U;
			return constants;
		}

		// A constant buffer holding `constants`, kept in the ConstantBuffer state between command lists and written through
		// one command list executed at once.
		static Result<nvrhi::BufferHandle> CreateMaterialConstants(GraphicsDevice& device, const MaterialConstants& constants, std::string debugName)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = sizeof(MaterialConstants);
			desc.isConstantBuffer = true;
			desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
			desc.keepInitialState = true;
			desc.debugName = std::move(debugName);
			ENGINE_TRY_ASSIGN(nvrhi::BufferHandle buffer, device.CreateBuffer(desc));
			// Not an immediate command list: the scene renderer resolves its materials while the frame's immediate list is open.
			ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle commandList,
				device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
			commandList->open();
			commandList->writeBuffer(buffer, &constants, sizeof(constants));
			commandList->close();
			device.ExecuteCommandList(*commandList);
			return buffer;
		}

		// An immutable RGBA16_FLOAT cube texture with every mip of `cube` (CubeMapData's layout), through HostImageUpload.
		// Errors: InvalidArgument for a cube whose sizes do not describe its texels; those of HostImageUpload::CreateTexture.
		static Result<nvrhi::TextureHandle> UploadCube(GraphicsDevice& device, const CubeMapData& cube, std::string debugName)
		{
			const auto fullMipCount = static_cast<uint32_t>(std::bit_width(cube.FaceSize));
			if (cube.FaceSize == 0 || cube.MipCount == 0 || cube.MipCount > fullMipCount
				|| cube.Texels.size() != ComputeCubeMapByteSize(cube.FaceSize, cube.MipCount))
			{
				return MakeError(ErrorCode::InvalidArgument, "the cube '{}' of face size {} and {} mips does not hold its {} bytes of texels", debugName,
					cube.FaceSize, cube.MipCount, cube.Texels.size());
			}
			nvrhi::TextureDesc desc;
			desc.width = cube.FaceSize;
			desc.height = cube.FaceSize;
			desc.arraySize = CubeMapData::FaceCount;
			desc.mipLevels = cube.MipCount;
			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.dimension = nvrhi::TextureDimension::TextureCube;
			desc.debugName = std::move(debugName);
			std::vector<TextureSubresourceData> subresources;
			subresources.reserve(static_cast<size_t>(cube.MipCount) * CubeMapData::FaceCount);
			size_t offset = 0;
			for (uint32_t level = 0; level < cube.MipCount; ++level)
			{
				const size_t edge = std::max<size_t>(1, cube.FaceSize >> level);
				const size_t faceBytes = edge * edge * CubeMapData::BytesPerTexel;
				for (uint32_t face = 0; face < CubeMapData::FaceCount; ++face)
				{
					subresources.push_back({
						.MipLevel = level,
						.ArraySlice = face,
						.Data = std::span<const std::byte>(cube.Texels.data() + offset, faceBytes),
						.RowPitch = 0,
						.DepthPitch = 0,
					});
					offset += faceBytes;
				}
			}
			ENGINE_TRY_ASSIGN(TextureUpload upload, device.GetHostImageUpload().CreateTexture(desc, subresources));
			return std::move(upload.Texture);
		}

		// The GPU mirror of `environment`: both cubes and the SH9.
		static Result<GpuEnvironment> UploadEnvironment(GraphicsDevice& device, const EnvironmentData& environment, const std::string& name)
		{
			GpuEnvironment mirror;
			ENGINE_TRY_ASSIGN(mirror.Skybox, UploadCube(device, environment.Skybox, std::format("{} (skybox)", name)));
			ENGINE_TRY_ASSIGN(mirror.Specular, UploadCube(device, environment.Specular, std::format("{} (specular)", name)));
			mirror.SkyboxMipCount = environment.Skybox.MipCount;
			mirror.SpecularMipCount = environment.Specular.MipCount;
			mirror.IrradianceSH9 = environment.IrradianceSH9;
			return mirror;
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
				// Not an immediate command list: the scene renderer resolves its meshes inside the frame (§8.3 pass 1), while the
				// frame's immediate list is open, and NVRHI's validation allows one open immediate list at a time.
				ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle commandList,
					device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
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

	namespace {

		// One material mirror (M8).
		struct MaterialEntry
		{
			GpuMaterial Value{};
			std::string UploadError{}; // why the constants buffer could not be created; empty when it was
			bool IsUsed = false;       // got through GetMaterial since the last CollectStale
			bool IsReported = false;   // the upload failure was reported to the asset manager
		};

		// One environment mirror (M8), or the remembered failure of its (handle, version).
		struct EnvironmentEntry
		{
			GpuEnvironment Value{};
			bool IsValid = false; // the cubes were uploaded
			bool IsUsed = false;
		};

		// Removes the entries of `entries` (materials or environments) whose version is no longer their asset's, and with
		// `releaseUnused` those not used since the previous call, then starts the next period.
		template<typename Entry>
		void CollectStaleEntries(std::map<MirrorKey, Entry>& entries, const AssetManager& assets, bool releaseUnused)
		{
			std::erase_if(entries, [&assets, releaseUnused](const auto& item)
			{
				return assets.GetVersion(item.first.Handle) != item.first.Version || (releaseUnused && !item.second.IsUsed);
			});
			for (auto& [key, entry] : entries)
				entry.IsUsed = false;
		}

	}

	struct GpuResourceCache::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		AssetManager* Assets = nullptr;   // documented back-reference
		MirrorTable<MeshTraits> Meshes;
		MirrorTable<TextureTraits> Textures;
		std::map<MirrorKey, MaterialEntry> Materials; // node-based: a returned reference survives insertions
		std::map<MirrorKey, EnvironmentEntry> Environments;
		size_t UploadFailures = 0;
		uint64_t NextGeneration = 1; // GpuMaterial::Generation: only increases

		[[nodiscard]] UploadContext GetContext() { return { .Device = Device, .Assets = Assets, .UploadFailures = &UploadFailures }; }
		// The texture of `slot`, or of the built-in `fallback` when the slot is empty, through the texture mirrors.
		[[nodiscard]] nvrhi::TextureHandle ResolveSlot(const TypedAssetHandle<AssetType::Texture>& slot, AssetHandle fallback);
		// Reports the failed constants buffer of material `handle` once (§8.14 item 7).
		void ReportMaterialFailure(AssetHandle handle, MaterialEntry& entry);
		// Reports why environment `handle` has no mirror (GpuResourceCache.h): once per (handle, version), as its entry is made.
		void ReportEnvironmentFailure(AssetHandle handle, std::string code, std::string message, std::string hint);
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

	nvrhi::TextureHandle GpuResourceCache::State::ResolveSlot(const TypedAssetHandle<AssetType::Texture>& slot, AssetHandle fallback)
	{
		return Textures.Get(GetContext(), slot.IsValid() ? slot.GetHandle() : fallback).Texture;
	}

	void GpuResourceCache::State::ReportMaterialFailure(AssetHandle handle, MaterialEntry& entry)
	{
		if (entry.UploadError.empty() || entry.IsReported)
			return;
		entry.IsReported = true;
		const std::string name = Utils::GetAssetName(*Assets, handle);
		AssetDiagnostic diagnostic;
		diagnostic.Severity = DiagnosticSeverity::Error;
		diagnostic.Code = std::string(AssetUploadFailedCode);
		diagnostic.Asset = handle;
		diagnostic.Path = name.substr(0, name.find('#'));
		diagnostic.Message = std::format("cannot create the GPU constants of material '{}': {}; draws using it are skipped", name, entry.UploadError);
		diagnostic.Hint = "the creation is tried again when the material changes; when the device is out of memory, use fewer materials or textures";
		Assets->ReportDiagnostic(std::move(diagnostic));
	}

	void GpuResourceCache::State::ReportEnvironmentFailure(AssetHandle handle, std::string code, std::string message, std::string hint)
	{
		const std::string name = Utils::GetAssetName(*Assets, handle);
		AssetDiagnostic diagnostic;
		diagnostic.Severity = DiagnosticSeverity::Error;
		diagnostic.Code = std::move(code);
		diagnostic.Asset = handle;
		diagnostic.Path = name.substr(0, name.find('#'));
		diagnostic.Message = std::move(message);
		diagnostic.Hint = std::move(hint);
		Assets->ReportDiagnostic(std::move(diagnostic));
	}

	const GpuMaterial& GpuResourceCache::GetMaterial(AssetHandle handle)
	{
		State& state = *m_State;
		AssetManager& assets = *state.Assets;
		// A null handle is the Default material (a MeshRenderer slot without a material), without a diagnostic.
		const AssetHandle resolved = handle.IsValid() ? handle : BuiltinAssetHandles::DefaultMaterial;
		const AssetRef<MaterialData> material = assets.GetOrPlaceholder<MaterialData>(resolved);
		const bool isPlaceholder =
			resolved != BuiltinAssetHandles::ErrorMaterial && material == AssetCast<MaterialData>(assets.GetPlaceholder(AssetType::Material));

		// The five maps, empty slots bound to White, FlatNormal or Black (§8.4), each texture at its current version.
		const std::array<nvrhi::TextureHandle, 5> textures = {
			state.ResolveSlot(material->BaseColorMap, BuiltinAssetHandles::WhiteTexture),
			state.ResolveSlot(material->MetallicRoughnessMap, BuiltinAssetHandles::WhiteTexture),
			state.ResolveSlot(material->NormalMap, BuiltinAssetHandles::FlatNormalTexture),
			state.ResolveSlot(material->OcclusionMap, BuiltinAssetHandles::WhiteTexture),
			state.ResolveSlot(material->EmissiveMap, BuiltinAssetHandles::BlackTexture),
		};
		const auto assignTextures = [&textures](GpuMaterial& mirror)
		{
			mirror.BaseColorMap = textures[0];
			mirror.MetallicRoughnessMap = textures[1];
			mirror.NormalMap = textures[2];
			mirror.OcclusionMap = textures[3];
			mirror.EmissiveMap = textures[4];
		};

		const MirrorKey key = { .Handle = resolved, .Version = assets.GetVersion(resolved) };
		if (const auto found = state.Materials.find(key); found != state.Materials.end())
		{
			MaterialEntry& entry = found->second;
			entry.IsUsed = true;
			const GpuMaterial& mirror = entry.Value;
			const std::array<nvrhi::TextureHandle, 5> current = {
				mirror.BaseColorMap,
				mirror.MetallicRoughnessMap,
				mirror.NormalMap,
				mirror.OcclusionMap,
				mirror.EmissiveMap,
			};
			if (current != textures)
			{
				// A texture changed (a new version, or its placeholder replaced by the asset): same constants, new textures.
				assignTextures(entry.Value);
				entry.Value.Generation = state.NextGeneration++;
			}
			return entry.Value;
		}

		MaterialEntry entry;
		entry.IsUsed = true;
		assignTextures(entry.Value);
		entry.Value.AlphaMode = material->AlphaMode;
		entry.Value.AlphaCutoff = material->AlphaCutoff;
		entry.Value.DoubleSided = material->DoubleSided;
		entry.Value.Version = key.Version;
		entry.Value.Generation = state.NextGeneration++;
		entry.Value.IsPlaceholder = isPlaceholder;
		Result<nvrhi::BufferHandle> constants = Utils::CreateMaterialConstants(*state.Device, Utils::MakeMaterialConstants(*material),
			std::format("{} (constants)", Utils::GetAssetName(assets, resolved)));
		if (constants.has_value())
		{
			entry.Value.Constants = std::move(*constants);
		}
		else
		{
			++state.UploadFailures;
			entry.UploadError = constants.error().ToString();
		}
		MaterialEntry& inserted = state.Materials.insert_or_assign(key, std::move(entry)).first->second;
		state.ReportMaterialFailure(resolved, inserted);
		return inserted.Value;
	}

	const GpuEnvironment* GpuResourceCache::GetEnvironment(AssetHandle handle)
	{
		if (!handle.IsValid())
			return nullptr;
		State& state = *m_State;
		AssetManager& assets = *state.Assets;
		// Load first: it publishes the first version (GetVersion is 0 before it), and remembers a failure itself (§7.2).
		const Result<AssetRef<Asset>> loaded = assets.Load(handle);
		const MirrorKey key = { .Handle = handle, .Version = assets.GetVersion(handle) };
		if (const auto found = state.Environments.find(key); found != state.Environments.end())
		{
			found->second.IsUsed = true;
			return found->second.IsValid ? &found->second.Value : nullptr;
		}

		EnvironmentEntry entry;
		entry.IsUsed = true;
		const std::string id = handle.ToString();
		if (!loaded.has_value())
		{
			// GetOrPlaceholder's codes (AssetManager.h), so every manager reports alike.
			const Error& error = loaded.error();
			if (error.GetCode() == ErrorCode::NotFound)
			{
				state.ReportEnvironmentFailure(handle, std::string(AssetMissingCode),
					std::format("environment {} is missing; the view is lit with its fallback colour", id), "restore the asset, or assign another one");
			}
			else if (error.GetCode() == ErrorCode::InvalidArgument)
			{
				state.ReportEnvironmentFailure(handle, std::string(AssetTypeMismatchCode),
					std::format("asset {} cannot be used as an Environment; the view is lit with its fallback colour", id),
					"assign an asset of type Environment");
			}
			else
			{
				state.ReportEnvironmentFailure(handle, std::string(AssetImportFailedCode),
					std::format("environment {} failed to load; the view is lit with its fallback colour: {}", id,
						Error(error).WithHint(std::string()).ToString()),
					error.GetHint());
			}
		}
		else if (const AssetRef<EnvironmentData> environment = AssetCast<EnvironmentData>(*loaded))
		{
			const std::string name = Utils::GetAssetName(assets, handle);
			Result<GpuEnvironment> uploaded = Utils::UploadEnvironment(*state.Device, *environment, name);
			if (uploaded.has_value())
			{
				entry.Value = std::move(*uploaded);
				entry.IsValid = true;
			}
			else
			{
				++state.UploadFailures;
				state.ReportEnvironmentFailure(handle, std::string(AssetUploadFailedCode),
					std::format("cannot upload environment '{}' to the GPU: {}; the view is lit with its fallback colour", name, uploaded.error().ToString()),
					"the upload is tried again when the asset changes; when the device is out of memory, use smaller environments");
			}
		}
		else
		{
			state.ReportEnvironmentFailure(handle, std::string(AssetTypeMismatchCode),
				std::format("asset {} is a {}, not an Environment; the view is lit with its fallback colour", id,
					AssetTypeToString(assets.GetAssetType(handle))),
				"assign an asset of type Environment");
		}
		entry.Value.Version = key.Version;
		EnvironmentEntry& inserted = state.Environments.insert_or_assign(key, std::move(entry)).first->second;
		return inserted.IsValid ? &inserted.Value : nullptr;
	}

	void GpuResourceCache::CollectStale(bool releaseUnused)
	{
		// A mirror whose version differs from the asset's current one is stale; besides older versions, that covers the
		// versions a dry run published and then rolled back (AssetManager::RestoreSharedState).
		CollectStaleEntries(m_State->Materials, *m_State->Assets, releaseUnused);
		CollectStaleEntries(m_State->Environments, *m_State->Assets, releaseUnused);
		m_State->Meshes.CollectStale(*m_State->Assets, releaseUnused);
		m_State->Textures.CollectStale(*m_State->Assets, releaseUnused);
	}

	void GpuResourceCache::Clear()
	{
		m_State->Materials.clear();
		m_State->Environments.clear();
		m_State->Meshes.Clear();
		m_State->Textures.Clear();
	}

	GpuResourceCacheStats GpuResourceCache::GetStats() const
	{
		return {
			.MeshCount = m_State->Meshes.GetCount(),
			.TextureCount = m_State->Textures.GetCount(),
			.UploadFailures = m_State->UploadFailures,
			.MaterialCount = m_State->Materials.size(),
			.EnvironmentCount = m_State->Environments.size(),
		};
	}

}
