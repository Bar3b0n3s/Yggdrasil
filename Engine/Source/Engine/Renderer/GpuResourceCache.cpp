#include "EnginePCH.h"
#include "Engine/Renderer/GpuResourceCache.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Asset/IrradianceVolumeData.h"
#include "Engine/Asset/LutData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureCompression.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/CommandListRecording.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Shared/MaterialConstants.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <format>
#include <limits>
#include <map>
#include <optional>
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
// textures changed takes them with a new Generation, keeping its constants buffer, unless the flags the textures imply
// changed (M23: a BC5 normal map's MaterialFlagNormalMapTwoChannel), which the constants hold: then it writes new constants
// into a new buffer. Every other constant depends on the material's version only, which is the key. Generations come from
// one counter of the cache that only increases. Environments (M8)
// have no placeholder (§7.2): an entry remembers a failed load or upload for its (handle, version), so the failure is
// reported once and nothing is retried until the asset's version changes. LUTs and heightfields (M26) are kept as
// environments are.

namespace Engine {

	namespace {

		struct MirrorKey
		{
			AssetHandle Handle{};
			uint64_t Version = 0;

			std::strong_ordering operator<=>(const MirrorKey&) const = default;
			bool operator==(const MirrorKey&) const = default;
		};

		// M23 (C23.1, C-MESH2; Docs/Decisions/0034-m23-contract.md decision 9): the cache's one shared default attribute
		// stream, the vertex binding 1 of every mesh without MeshData version 2's attribute stream: Count entries of
		// MeshVertexAttributes' defaults (TexCoord1 (0, 0), opaque white), as many as the largest such mesh has vertices. All
		// its entries are equal, so every such mesh binds it at offset 0. A larger mesh replaces it with a larger one; the
		// mirrors that hold the smaller one keep it alive until they are released.
		struct DefaultAttributeStream
		{
			nvrhi::BufferHandle Buffer{};
			uint32_t Count = 0;
		};

		// The services an upload and its failure report need.
		struct UploadContext
		{
			GraphicsDevice* Device = nullptr; // documented back-references of the cache
			AssetManager* Assets = nullptr;
			size_t* UploadFailures = nullptr;
			// M23 (decision 15): whether the cache logged its block-compressed textures' path (once per cache).
			bool* BlockPathLogged = nullptr;
			// M23 (decision 9): the cache's shared default attribute stream, which a mesh upload may grow.
			DefaultAttributeStream* DefaultAttributes = nullptr;
		};

		// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decision 43): whether GpuResourceCache::TrimUnused releases the mirror
		// `key`: a replaced version (as CollectStale), or the mirror of an asset the manager no longer has loaded (released or
		// unknown), unless it is a built-in (the placeholders among them) or a stand-in sharing its placeholder's objects
		// (`isPlaceholder`), which frees nothing. A loaded, current asset's mirror stays: the next Render would upload it again.
		[[nodiscard]] bool IsTrimmable(const AssetManager& assets, const MirrorKey& key, bool isPlaceholder)
		{
			if (assets.GetVersion(key.Handle) != key.Version)
				return true;
			return !IsBuiltinAssetHandle(key.Handle) && !isPlaceholder && assets.GetState(key.Handle) == AssetState::Unloaded;
		}

	}

	namespace Utils {

		// The readable name of `handle` for debug names and diagnostics: its reference path, or its 16 hex digits.
		static std::string GetAssetName(const AssetManager& assets, AssetHandle handle)
		{
			std::string path = assets.GetReferencePath(handle);
			return path.empty() ? handle.ToString() : path;
		}

		// Whether an upload that just failed on `device` is the asset's own failure (§8.14 item 7: counted, reported once and
		// not retried until the asset changes). On a lost device it is not (M18, Docs/Decisions/0029-m18-contract.md decision
		// 28: a Gpu error on a lost device is the loss, never out-of-memory): it is neither counted nor reported, and the
		// recovery's ReleaseGpuObjects drops the stand-in mirror, so the next Get uploads the asset again.
		[[nodiscard]] static bool IsUploadFailure(const GraphicsDevice& device)
		{
			return !device.IsLost();
		}

		// The GPU format a texture of `format` uploads as, its blocks included (a device with BC support).
		static Result<nvrhi::Format> ToNvrhiFormat(TextureFormat format)
		{
			switch (format)
			{
				case TextureFormat::RGBA8Unorm: return nvrhi::Format::RGBA8_UNORM;
				case TextureFormat::RGBA8Srgb:  return nvrhi::Format::SRGBA8_UNORM;
				case TextureFormat::R8Unorm:    return nvrhi::Format::R8_UNORM;
				case TextureFormat::BC1Unorm:   return nvrhi::Format::BC1_UNORM;
				case TextureFormat::BC1Srgb:    return nvrhi::Format::BC1_UNORM_SRGB;
				case TextureFormat::BC4Unorm:   return nvrhi::Format::BC4_UNORM;
				case TextureFormat::BC5Unorm:   return nvrhi::Format::BC5_UNORM;
				case TextureFormat::BC7Unorm:   return nvrhi::Format::BC7_UNORM;
				case TextureFormat::BC7Srgb:    return nvrhi::Format::BC7_UNORM_SRGB;
			}
			return MakeError(ErrorCode::InvalidArgument, "unknown texture format {}", std::to_underlying(format));
		}

		// M23 (decision 15): the GPU format the decode fallback uploads a block-compressed `format` as, on a device without BC
		// support: what a sampler returns from the BC format, at 8 bits per channel.
		static Result<nvrhi::Format> ToFallbackFormat(TextureFormat format)
		{
			switch (format)
			{
				case TextureFormat::BC1Unorm:
				case TextureFormat::BC5Unorm:
				case TextureFormat::BC7Unorm:   return nvrhi::Format::RGBA8_UNORM;
				case TextureFormat::BC1Srgb:
				case TextureFormat::BC7Srgb:    return nvrhi::Format::SRGBA8_UNORM;
				case TextureFormat::BC4Unorm:   return nvrhi::Format::R8_UNORM;
				case TextureFormat::RGBA8Unorm:
				case TextureFormat::RGBA8Srgb:
				case TextureFormat::R8Unorm:    break;
			}
			return MakeError(ErrorCode::InvalidArgument, "texture format {} is not block-compressed", TextureFormatToString(format));
		}

		// The decode fallback's texels of one level: DecompressTextureBlocks' RGBA8, or for BC4 its red channel alone, which an
		// R8_UNORM texture samples as (r, 0, 0, 1), as the GPU samples BC4. Errors: those of DecompressTextureBlocks.
		static Result<Buffer> DecodeLevel(TextureBlockCodec codec, const TextureMip& mip, std::span<const std::byte> blocks)
		{
			ENGINE_TRY_ASSIGN(Buffer texels, DecompressTextureBlocks(codec, mip.Width, mip.Height, blocks));
			if (codec != TextureBlockCodec::BC4)
				return texels;
			Buffer red(texels.size() / 4);
			for (size_t texel = 0; texel < red.size(); ++texel)
				red[texel] = texels[texel * 4];
			return red;
		}

		// Logs, once per cache, the path its block-compressed textures take on its device.
		static void LogBlockPath(const UploadContext& context, bool decode)
		{
			if (*context.BlockPathLogged)
				return;
			*context.BlockPathLogged = true;
			if (decode)
			{
				ENGINE_CORE_INFO("GpuResourceCache: the device has no BC texture support, so block-compressed textures and environment cubes "
								 "are decompressed at upload (the decode fallback: RGBA8, R8 or RGBA16F, more GPU memory)");
			}
			else
			{
				ENGINE_CORE_INFO("GpuResourceCache: block-compressed textures upload as their BC blocks");
			}
		}

		// Creates a vertex or index buffer of `size` bytes, kept in its shader-readable state between command lists, and
		// records its upload into `commandList`. M23 (C23.3): `rawViews` lets a deformable mesh's vertex buffer be the
		// Skinning pass's raw source too (SkinningPass.h).
		static Result<nvrhi::BufferHandle> CreateGeometryBuffer(GraphicsDevice& device, nvrhi::ICommandList& commandList, const void* data,
			size_t size, bool isIndexBuffer, std::string debugName, bool rawViews = false)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = size;
			desc.isVertexBuffer = !isIndexBuffer;
			desc.isIndexBuffer = isIndexBuffer;
			desc.canHaveRawViews = rawViews;
			desc.initialState = isIndexBuffer ? nvrhi::ResourceStates::IndexBuffer : nvrhi::ResourceStates::VertexBuffer;
			desc.keepInitialState = true;
			desc.debugName = std::move(debugName);
			ENGINE_TRY_ASSIGN(nvrhi::BufferHandle buffer, device.CreateBuffer(desc));
			commandList.writeBuffer(buffer, data, size);
			return buffer;
		}

		// M23 (C23.3, decision 27): a raw buffer of `size` bytes the Skinning pass reads (a mesh's skin or morph deltas), kept
		// in ShaderResource between command lists; the caller records its contents.
		static Result<nvrhi::BufferHandle> CreateDeformBuffer(GraphicsDevice& device, uint64_t size, std::string debugName)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = size;
			desc.canHaveRawViews = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = std::move(debugName);
			return device.CreateBuffer(desc);
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
			// M23 (Material version 2, decision 10): the maps that sample the second UV set.
			const std::array<std::pair<TextureCoordinateSet, uint32_t>, 5> uvSets = {
				std::pair(material.BaseColorUVSet, MaterialFlagBaseColorUV1),
				std::pair(material.MetallicRoughnessUVSet, MaterialFlagMetallicRoughnessUV1),
				std::pair(material.NormalUVSet, MaterialFlagNormalUV1),
				std::pair(material.OcclusionUVSet, MaterialFlagOcclusionUV1),
				std::pair(material.EmissiveUVSet, MaterialFlagEmissiveUV1),
			};
			for (const auto& [set, flag] : uvSets)
			{
				if (set == TextureCoordinateSet::UV1)
					constants.Flags |= flag;
			}
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
			CommandListRecording recording(device, *commandList);
			commandList->writeBuffer(buffer, &constants, sizeof(constants));
			recording.Execute();
			return buffer;
		}

		// An immutable cube texture with every mip of `cube` (CubeMapData's layout), through HostImageUpload: RGBA16_FLOAT for
		// an Rgba16Float cube; for a BC6HUfloat cube (M23, Docs/Decisions/0034-m23-contract.md decisions 14 and 15) its blocks
		// as BC6H_UFLOAT on a device with BC support, or on a device without it each face level decompressed with
		// DecompressTextureBlocks to RGBA16_FLOAT (the decode fallback, logged once per cache with the textures' path).
		// Errors: InvalidArgument for a cube whose format or sizes do not describe its texels; those of
		// DecompressTextureBlocks, naming the level and face; those of HostImageUpload::CreateTexture.
		static Result<nvrhi::TextureHandle> UploadCube(const UploadContext& context, const CubeMapData& cube, std::string debugName)
		{
			GraphicsDevice& device = *context.Device;
			const auto fullMipCount = static_cast<uint32_t>(std::bit_width(cube.FaceSize));
			const bool blocks = cube.Format == CubeMapFormat::BC6HUfloat;
			if ((!blocks && cube.Format != CubeMapFormat::Rgba16Float) || cube.FaceSize == 0 || cube.MipCount == 0 || cube.MipCount > fullMipCount
				|| cube.Texels.size() != ComputeCubeMapByteSize(cube.FaceSize, cube.MipCount, cube.Format))
			{
				return MakeError(ErrorCode::InvalidArgument, "the {} cube '{}' of face size {} and {} mips does not hold its {} bytes of texels",
					CubeMapFormatToString(cube.Format), debugName, cube.FaceSize, cube.MipCount, cube.Texels.size());
			}
			const bool decode = blocks && !device.GetInfo().TextureCompressionBC;
			if (blocks)
				LogBlockPath(context, decode);
			nvrhi::TextureDesc desc;
			desc.width = cube.FaceSize;
			desc.height = cube.FaceSize;
			desc.arraySize = CubeMapData::FaceCount;
			desc.mipLevels = cube.MipCount;
			desc.format = blocks && !decode ? nvrhi::Format::BC6H_UFLOAT : nvrhi::Format::RGBA16_FLOAT;
			desc.dimension = nvrhi::TextureDimension::TextureCube;
			desc.debugName = std::move(debugName);
			// The decode fallback's face levels, which the subresources view until the upload returns.
			std::vector<Buffer> decoded;
			std::vector<TextureSubresourceData> subresources;
			subresources.reserve(static_cast<size_t>(cube.MipCount) * CubeMapData::FaceCount);
			if (decode)
				decoded.reserve(static_cast<size_t>(cube.MipCount) * CubeMapData::FaceCount);
			size_t offset = 0;
			for (uint32_t level = 0; level < cube.MipCount; ++level)
			{
				const uint32_t edge = std::max(1U, cube.FaceSize >> level);
				const size_t faceBytes = ComputeCubeMapFaceByteSize(cube.FaceSize, level, cube.Format);
				for (uint32_t face = 0; face < CubeMapData::FaceCount; ++face)
				{
					std::span<const std::byte> data(cube.Texels.data() + offset, faceBytes);
					if (decode)
					{
						Result<Buffer> texels = DecompressTextureBlocks(TextureBlockCodec::BC6H, edge, edge, data);
						if (!texels.has_value())
						{
							return std::unexpected(std::move(texels).error().WithContext(
								std::format("while decoding mip level {}, face {} of the cube '{}'", level, face, desc.debugName)));
						}
						decoded.push_back(std::move(*texels));
						data = decoded.back();
					}
					subresources.push_back({
						.MipLevel = level,
						.ArraySlice = face,
						.Data = data,
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
		static Result<GpuEnvironment> UploadEnvironment(const UploadContext& context, const EnvironmentData& environment, const std::string& name)
		{
			GpuEnvironment mirror;
			ENGINE_TRY_ASSIGN(mirror.Skybox, UploadCube(context, environment.Skybox, std::format("{} (skybox)", name)));
			ENGINE_TRY_ASSIGN(mirror.Specular, UploadCube(context, environment.Specular, std::format("{} (specular)", name)));
			mirror.SkyboxMipCount = environment.Skybox.MipCount;
			mirror.SpecularMipCount = environment.Specular.MipCount;
			mirror.IrradianceSH9 = environment.IrradianceSH9;
			return mirror;
		}

		// M26 (C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 16, stream Q-B): the GPU mirror of `volume`, its
		// probes' coefficients as float4s (xyz the coefficient, w 0) in the asset's order, written by one command list executed
		// at once (not the immediate list: the scene renderer resolves its probes inside the frame, as its meshes). A volume
		// whose counts are outside the format's limits or disagree with its probes (one no loader made, since every loader
		// validates it) is refused, so the shader never reads past the buffer.
		static Result<GpuIrradianceVolume> UploadIrradianceVolume(const UploadContext& context, const IrradianceVolumeData& volume, const std::string& name)
		{
			const auto isCount = [](uint32_t count)
			{
				return count >= MinIrradianceProbesPerAxis && count <= MaxIrradianceProbesPerAxis;
			};
			const size_t probeCount = static_cast<size_t>(volume.CountX) * volume.CountY * volume.CountZ;
			if (!isCount(volume.CountX) || !isCount(volume.CountY) || !isCount(volume.CountZ) || volume.Probes.size() != probeCount)
			{
				return MakeError(ErrorCode::InvalidArgument, "the irradiance volume '{}' of {} x {} x {} probes holds {} probes", name, volume.CountX, volume.CountY,
					volume.CountZ, volume.Probes.size());
			}
			std::vector<glm::vec4> coefficients;
			coefficients.reserve(probeCount * std::tuple_size_v<decltype(IrradianceProbe::SH9)>);
			for (const IrradianceProbe& probe : volume.Probes)
			{
				for (const glm::vec3& coefficient : probe.SH9)
					coefficients.emplace_back(coefficient, 0.0f);
			}
			GraphicsDevice& device = *context.Device;
			nvrhi::BufferDesc desc;
			desc.byteSize = coefficients.size() * sizeof(glm::vec4);
			desc.structStride = sizeof(glm::vec4);
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = std::format("{} (probes)", name);
			ENGINE_TRY_ASSIGN(nvrhi::BufferHandle buffer, device.CreateBuffer(desc));
			ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle commandList,
				device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
			CommandListRecording recording(device, *commandList);
			commandList->writeBuffer(buffer, coefficients.data(), desc.byteSize);
			recording.Execute();
			return GpuIrradianceVolume{ .Probes = std::move(buffer), .CountX = volume.CountX, .CountY = volume.CountY, .CountZ = volume.CountZ };
		}

		// M26 (C26.1, R-LUT; Docs/Decisions/0037-m26-contract.md decision 15, stream X-B): the GPU mirror of `lut` (GpuLut): its
		// entries as a Size^3 RGBA16_FLOAT 3D texture, x red, y green and z blue (the table's red-fastest order is the texture's
		// row-major order), alpha 1. Each value is rounded to the nearest binary16 (glm's integer conversion, identical on every
		// run and configuration), clamped first to binary16's finite range so trilinear filtering never meets an infinity.
		static Result<GpuLut> UploadLut(const UploadContext& context, const LutData& lut, const std::string& name)
		{
			constexpr float MaxHalf = 65504.0f;
			const uint16_t one = glm::packHalf1x16(1.0f);
			std::vector<uint16_t> texels;
			texels.reserve(lut.Values.size() * 4);
			for (const glm::vec3& value : lut.Values)
			{
				for (glm::length_t channel = 0; channel < 3; ++channel)
					texels.push_back(glm::packHalf1x16(std::clamp(value[channel], -MaxHalf, MaxHalf)));
				texels.push_back(one);
			}
			nvrhi::TextureDesc desc;
			desc.dimension = nvrhi::TextureDimension::Texture3D;
			desc.width = lut.Size;
			desc.height = lut.Size;
			desc.depth = lut.Size;
			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.isShaderResource = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = name;
			const std::array<TextureSubresourceData, 1> subresources = { { { .MipLevel = 0,
				.ArraySlice = 0,
				.Data = std::as_bytes(std::span<const uint16_t>(texels)),
				.RowPitch = static_cast<size_t>(lut.Size) * 4 * sizeof(uint16_t),
				.DepthPitch = static_cast<size_t>(lut.Size) * lut.Size * 4 * sizeof(uint16_t) } } };
			ENGINE_TRY_ASSIGN(TextureUpload upload, context.Device->GetHostImageUpload().CreateTexture(desc, subresources));
			GpuLut mirror;
			mirror.Texture = std::move(upload.Texture);
			mirror.Size = lut.Size;
			mirror.DomainMin = lut.DomainMin;
			mirror.DomainMax = lut.DomainMax;
			return mirror;
		}

		// M26 (C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 22, stream T-C): the GPU mirror of `heightfield`
		// (GpuHeightfield): its samples as a Resolution^2 R16_UNORM texture, sample (x, z) at texel (x, z), so a shader's texel
		// is sample / MaxHeightfieldSample, as Asset/TerrainGeometry.h places it. A heightfield whose resolution is outside the
		// format's limits or disagrees with its samples (one no loader made, since every loader validates it) is refused, so
		// the shader never reads past the texture.
		static Result<GpuHeightfield> UploadHeightfield(const UploadContext& context, const HeightfieldData& heightfield, const std::string& name)
		{
			const uint32_t resolution = heightfield.Resolution;
			if (resolution < MinHeightfieldResolution || resolution > MaxHeightfieldResolution
				|| heightfield.Heights.size() != static_cast<size_t>(resolution) * resolution)
			{
				return MakeError(ErrorCode::InvalidArgument, "the heightfield '{}' of {} samples a side holds {} samples", name, resolution,
					heightfield.Heights.size());
			}
			nvrhi::TextureDesc desc;
			desc.width = resolution;
			desc.height = resolution;
			desc.format = nvrhi::Format::R16_UNORM;
			desc.isShaderResource = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = name;
			const std::array<TextureSubresourceData, 1> subresources = { { { .MipLevel = 0,
				.ArraySlice = 0,
				.Data = std::as_bytes(std::span<const uint16_t>(heightfield.Heights)),
				.RowPitch = static_cast<size_t>(resolution) * sizeof(uint16_t) } } };
			ENGINE_TRY_ASSIGN(TextureUpload upload, context.Device->GetHostImageUpload().CreateTexture(desc, subresources));
			return GpuHeightfield{ .Heights = std::move(upload.Texture), .Resolution = resolution };
		}

	}

	namespace {

		// The mesh half of the cache: a vertex and an index buffer, and the attribute stream (M23: the mesh's own, or the
		// shared default stream, grown when needed), filled by one command list (§8.14 item 1). M23 (C23.3, decision 27): a
		// mesh with a skin or morph targets also gets its skin and morph buffers, its vertex buffer is a raw buffer too (the
		// Skinning pass's source), and its deform bounds are computed once (MeshDeform.h).
		struct MeshTraits
		{
			using Data = MeshData;
			using Mirror = GpuMesh;
			static constexpr std::string_view Kind = "mesh";

			[[nodiscard]] static bool HasGpuObjects(const GpuMesh& mirror)
			{
				return mirror.VertexBuffer != nullptr && mirror.AttributeBuffer != nullptr && mirror.IndexBuffer != nullptr;
			}

			// The attribute stream of `mesh` (binding 1): its own buffer, written by `commandList`; or the shared default
			// stream, a new one of the mesh's vertex count written by `commandList` when the current one is shorter. `grown`
			// receives that new stream, which the caller makes the shared one once the list has executed.
			[[nodiscard]] static Result<nvrhi::BufferHandle> CreateAttributeBuffer(const UploadContext& context, nvrhi::ICommandList& commandList,
				const MeshData& mesh, const std::string& name, DefaultAttributeStream& grown)
			{
				if (!mesh.Attributes.empty())
				{
					return Utils::CreateGeometryBuffer(*context.Device, commandList, mesh.Attributes.data(),
						mesh.Attributes.size() * sizeof(MeshVertexAttributes), false, std::format("{} (attributes)", name));
				}
				const DefaultAttributeStream& shared = *context.DefaultAttributes;
				const auto count = static_cast<uint32_t>(mesh.Vertices.size());
				if (shared.Buffer != nullptr && shared.Count >= count)
					return shared.Buffer;
				const std::vector<MeshVertexAttributes> defaults(count);
				ENGINE_TRY_ASSIGN(grown.Buffer, Utils::CreateGeometryBuffer(*context.Device, commandList, defaults.data(), defaults.size() * sizeof(MeshVertexAttributes), false, "GpuResourceCache.DefaultAttributes"));
				grown.Count = count;
				return grown.Buffer;
			}

			[[nodiscard]] static Result<GpuMesh> Upload(const UploadContext& context, const MeshData& mesh, const std::string& name)
			{
				GraphicsDevice& device = *context.Device;
				if (mesh.Vertices.empty() || mesh.Indices.empty())
					return MakeError(ErrorCode::InvalidArgument, "mesh '{}' has no vertices or no indices", name);
				// Not an immediate command list: the scene renderer resolves its meshes inside the frame (§8.3 pass 1), while the
				// frame's immediate list is open, and NVRHI's validation allows one open immediate list at a time.
				// M23 (C23.3): the Skinning pass addresses a deformable mesh's bytes with 32-bit offsets (Passes/Skinning.slang).
				const bool skinned = !mesh.Skin.empty() && mesh.SkinJointCount > 0;
				const bool morphed = !mesh.MorphTargets.empty();
				const uint64_t vertexBytes = static_cast<uint64_t>(mesh.Vertices.size()) * sizeof(MeshVertex);
				const uint64_t morphTargetBytes = static_cast<uint64_t>(mesh.Vertices.size()) * sizeof(MeshMorphDelta);
				const uint64_t morphBytes = mesh.MorphTargets.size() * morphTargetBytes;
				if ((skinned || morphed) && std::max(vertexBytes, morphBytes) > std::numeric_limits<uint32_t>::max())
					return MakeError(ErrorCode::InvalidArgument, "mesh '{}' is too large to deform: its vertices or morph deltas exceed 4 GiB", name);
				ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle commandList,
					device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
				CommandListRecording recording(device, *commandList);
				Result<nvrhi::BufferHandle> vertices = Utils::CreateGeometryBuffer(device, *commandList, mesh.Vertices.data(),
					mesh.Vertices.size() * sizeof(MeshVertex), false, std::format("{} (vertices)", name), skinned || morphed);
				Result<nvrhi::BufferHandle> indices = vertices
					? Utils::CreateGeometryBuffer(device, *commandList, mesh.Indices.data(), mesh.Indices.size() * sizeof(uint32_t), true,
						  std::format("{} (indices)", name))
					: Result<nvrhi::BufferHandle>(nvrhi::BufferHandle());
				DefaultAttributeStream grown;
				Result<nvrhi::BufferHandle> attributes =
					indices ? CreateAttributeBuffer(context, *commandList, mesh, name, grown) : Result<nvrhi::BufferHandle>(nvrhi::BufferHandle());
				Result<nvrhi::BufferHandle> skin = nvrhi::BufferHandle();
				if (attributes && skinned)
				{
					skin = Utils::CreateDeformBuffer(device, mesh.Skin.size() * sizeof(MeshSkinVertex), std::format("{} (skin)", name));
					if (skin)
						commandList->writeBuffer(*skin, mesh.Skin.data(), mesh.Skin.size() * sizeof(MeshSkinVertex));
				}
				Result<nvrhi::BufferHandle> morphs = nvrhi::BufferHandle();
				if (attributes && skin && morphed)
				{
					// Target by target, each of one delta per vertex.
					morphs = Utils::CreateDeformBuffer(device, morphBytes, std::format("{} (morph targets)", name));
					for (size_t target = 0; morphs && target < mesh.MorphTargets.size(); ++target)
						commandList->writeBuffer(*morphs, mesh.MorphTargets[target].Deltas.data(), morphTargetBytes, target * morphTargetBytes);
				}
				// The list is executed whatever the buffers' results: a list that never executes keeps itself, and the buffer
				// it wrote, alive (CommandListRecording), which would fail the next device loss's recovery. Without every
				// buffer its writes are discarded with them.
				recording.Execute();
				if (!vertices)
					return std::unexpected(std::move(vertices).error());
				if (!indices)
					return std::unexpected(std::move(indices).error());
				if (!attributes)
					return std::unexpected(std::move(attributes).error());
				if (!skin)
					return std::unexpected(std::move(skin).error());
				if (!morphs)
					return std::unexpected(std::move(morphs).error());
				if (grown.Buffer != nullptr)
					*context.DefaultAttributes = std::move(grown);

				GpuMesh mirror;
				mirror.VertexBuffer = std::move(*vertices);
				mirror.AttributeBuffer = std::move(*attributes);
				mirror.IndexBuffer = std::move(*indices);
				if (skinned || morphed)
				{
					mirror.SkinBuffer = std::move(*skin);
					mirror.MorphBuffer = std::move(*morphs);
					mirror.SkinJointCount = skinned ? mesh.SkinJointCount : 0;
					mirror.MorphTargetCount = static_cast<uint32_t>(mesh.MorphTargets.size());
					mirror.DeformBounds = ComputeMeshDeformBounds(mesh);
				}
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
		// device allows it, staging otherwise). M23 (decision 15): a block-compressed texture uploads its blocks, or on a
		// device without BC support its decompressed levels (the decode fallback).
		struct TextureTraits
		{
			using Data = TextureData;
			using Mirror = GpuTexture;
			static constexpr std::string_view Kind = "texture";

			[[nodiscard]] static bool HasGpuObjects(const GpuTexture& mirror) { return mirror.Texture != nullptr; }

			[[nodiscard]] static Result<GpuTexture> Upload(const UploadContext& context, const TextureData& texture, const std::string& name)
			{
				GraphicsDevice& device = *context.Device;
				if (texture.Mips.empty())
					return MakeError(ErrorCode::InvalidArgument, "texture '{}' has no mip levels", name);
				const std::optional<TextureBlockCodec> codec = GetTextureBlockCodec(texture.Format);
				const bool decode = codec.has_value() && !device.GetInfo().TextureCompressionBC;
				if (codec.has_value())
					Utils::LogBlockPath(context, decode);
				ENGINE_TRY_ASSIGN(const nvrhi::Format format, decode ? Utils::ToFallbackFormat(texture.Format) : Utils::ToNvrhiFormat(texture.Format));
				// The decode fallback's levels, which the subresources view until the upload returns.
				std::vector<Buffer> decoded;
				if (decode)
				{
					decoded.reserve(texture.Mips.size());
					for (uint32_t level = 0; level < texture.Mips.size(); ++level)
					{
						Result<Buffer> texels = Utils::DecodeLevel(*codec, texture.Mips[level], GetMipPixels(texture, level));
						if (!texels.has_value())
							return std::unexpected(std::move(texels).error().WithContext(std::format("while decoding mip level {} of texture '{}'", level, name)));
						decoded.push_back(std::move(*texels));
					}
				}
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
						.Data = decode ? std::span<const std::byte>(decoded[level]) : GetMipPixels(texture, level),
						.RowPitch = 0,
						.DepthPitch = 0,
					});
				}
				ENGINE_TRY_ASSIGN(TextureUpload upload, device.GetHostImageUpload().CreateTexture(desc, subresources));
				GpuTexture mirror;
				mirror.Texture = std::move(upload.Texture);
				mirror.Path = upload.Path;
				mirror.Format = texture.Format;
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

			// M18 (C18.4): GpuResourceCache::TrimUnused for this table (IsTrimmable); the use flags of CollectStale's period
			// stay as they are.
			void TrimUnused(const AssetManager& assets)
			{
				std::erase_if(m_Entries, [&assets](const auto& item)
				{
					return IsTrimmable(assets, item.first, item.second.Value.IsPlaceholder);
				});
			}

			void Clear() { m_Entries.clear(); }

			[[nodiscard]] size_t GetCount() const { return m_Entries.size(); }

			// Whether a mirror of the table satisfies `predicate` (M23: whether a mesh still binds the shared default stream).
			template<typename Predicate>
			[[nodiscard]] bool AnyMirror(Predicate predicate) const
			{
				return std::ranges::any_of(m_Entries, [&predicate](const auto& item)
				{
					return predicate(item.second.Value);
				});
			}
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
					Result<Mirror> uploaded = Traits::Upload(context, *data, name);
					if (uploaded)
					{
						entry.Value = std::move(*uploaded);
					}
					else
					{
						// The placeholder's mirror stands in; when the placeholder's own upload failed, its GPU objects stay null.
						// On a lost device the failure is the loss, not the asset's (Utils::IsUploadFailure).
						if (Utils::IsUploadFailure(*context.Device))
						{
							++*context.UploadFailures;
							entry.UploadError = uploaded.error().ToString();
						}
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

		// M26 (C26.1, stream Q-B): one irradiance volume mirror, or the remembered failure of its (handle, version), as an
		// environment's.
		struct IrradianceVolumeEntry
		{
			GpuIrradianceVolume Value{};
			bool IsValid = false; // the coefficients were uploaded
			bool IsUsed = false;
		};

		// One LUT mirror (M26, stream X-B), or the remembered failure of its (handle, version), as an environment's.
		struct LutEntry
		{
			GpuLut Value{};
			bool IsValid = false; // the 3D texture was uploaded
			bool IsUsed = false;
		};

		// One heightfield mirror (M26, stream T-C), or the remembered failure of its (handle, version), as an environment's.
		struct HeightfieldEntry
		{
			GpuHeightfield Value{};
			bool IsValid = false; // the height texture was uploaded
			bool IsUsed = false;
		};

		// Removes the entries of `entries` (materials, environments, irradiance volumes, LUTs or heightfields) whose version is
		// no longer their asset's, and with `releaseUnused` those not used since the previous call, then starts the next
		// period.
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
		std::map<MirrorKey, IrradianceVolumeEntry> IrradianceVolumes; // M26 (stream Q-B)
		std::map<MirrorKey, LutEntry> Luts;                           // M26, stream X-B
		std::map<MirrorKey, HeightfieldEntry> Heightfields;           // M26, stream T-C
		size_t UploadFailures = 0;
		uint64_t NextGeneration = 1; // GpuMaterial::Generation: only increases
		// M23 (decision 9): the shared default attribute stream of the meshes without one. Dropped with every mirror (Clear,
		// ReleaseGpuObjects); the next mesh upload creates it again.
		DefaultAttributeStream DefaultAttributes{};
		// M23 (decision 15): the Info line naming the block-compressed textures' path was logged. Kept across a device loss:
		// the recovered device has the same specification, so the same path.
		bool BlockPathLogged = false;
		// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 26): the cache's registration with its device.
		GpuRecreationRegistration Recreation{};

		[[nodiscard]] UploadContext GetContext()
		{
			return { .Device = Device,
				.Assets = Assets,
				.UploadFailures = &UploadFailures,
				.BlockPathLogged = &BlockPathLogged,
				.DefaultAttributes = &DefaultAttributes };
		}
		// The texture mirror of `slot`, or of the built-in `fallback` when the slot is empty.
		[[nodiscard]] const GpuTexture& ResolveSlot(const TypedAssetHandle<AssetType::Texture>& slot, AssetHandle fallback);
		// Writes `entry`'s constants: MakeMaterialConstants of `material` with `flags`, into a new buffer, recording a failure
		// as GetMaterial documents (counted and reported once, never on a lost device).
		void WriteMaterialConstants(AssetHandle handle, const MaterialData& material, uint32_t flags, MaterialEntry& entry);
		// Reports the failed constants buffer of material `handle` once (§8.14 item 7).
		void ReportMaterialFailure(AssetHandle handle, MaterialEntry& entry);
		// Reports why `handle` has no mirror, an environment's or an M26 type's (GpuResourceCache.h): once per (handle, version),
		// as its entry is made.
		void ReportMirrorFailure(AssetHandle handle, std::string code, std::string message, std::string hint);
		// M23 (decision 9): drops the shared default attribute stream once no mesh mirror binds it, so a collection or a trim
		// that released every mesh without a stream leaves none of the cache's buffers alive; the next such mesh creates it
		// again.
		void ReleaseUnboundDefaultAttributes();
	};

	GpuResourceCache::GpuResourceCache(GraphicsDevice& device, AssetManager& assets)
		: m_State(CreateScope<State>())
	{
		m_State->Device = &device;
		m_State->Assets = &assets;
		// M18 (C18.3, decisions 26 and 27): released with every mirror; the next Get uploads each one again.
		GpuResourceCache* owner = this;
		GpuRecreationHooks hooks;
		hooks.Name = "GpuResourceCache";
		hooks.Release = [owner]()
		{
			owner->ReleaseGpuObjects();
		};
		// M18 (C18.4, decision 43): stale and unloaded mirrors, when a request ran out of GPU memory.
		hooks.TrimUnused = [owner]()
		{
			owner->TrimUnused();
		};
		m_State->Recreation = device.GetRecreation().Register(std::move(hooks));
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

	const GpuTexture& GpuResourceCache::State::ResolveSlot(const TypedAssetHandle<AssetType::Texture>& slot, AssetHandle fallback)
	{
		return Textures.Get(GetContext(), slot.IsValid() ? slot.GetHandle() : fallback);
	}

	void GpuResourceCache::State::WriteMaterialConstants(AssetHandle handle, const MaterialData& material, uint32_t flags, MaterialEntry& entry)
	{
		MaterialConstants constants = Utils::MakeMaterialConstants(material);
		constants.Flags = flags;
		entry.Value.Flags = flags;
		Result<nvrhi::BufferHandle> created = Utils::CreateMaterialConstants(*Device, constants, std::format("{} (constants)", Utils::GetAssetName(*Assets, handle)));
		if (created.has_value())
		{
			entry.Value.Constants = std::move(*created);
			entry.UploadError.clear();
			return;
		}
		// The previous constants (a rebuild's) hold other flags: draws using the material are skipped instead.
		entry.Value.Constants = nullptr;
		if (Utils::IsUploadFailure(*Device))
		{
			++UploadFailures;
			entry.UploadError = created.error().ToString();
			entry.IsReported = false;
		}
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

	void GpuResourceCache::State::ReportMirrorFailure(AssetHandle handle, std::string code, std::string message, std::string hint)
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

	void GpuResourceCache::State::ReleaseUnboundDefaultAttributes()
	{
		if (DefaultAttributes.Buffer == nullptr)
			return;
		const nvrhi::IBuffer* shared = DefaultAttributes.Buffer.Get();
		const bool bound = Meshes.AnyMirror([shared](const GpuMesh& mirror)
		{
			return mirror.AttributeBuffer.Get() == shared;
		});
		if (!bound)
			DefaultAttributes = {};
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

		// The five maps, empty slots bound to White, FlatNormal or Black (§8.4), each texture at its current version, resolved
		// in slot order.
		const nvrhi::TextureHandle baseColorMap = state.ResolveSlot(material->BaseColorMap, BuiltinAssetHandles::WhiteTexture).Texture;
		const nvrhi::TextureHandle metallicRoughnessMap = state.ResolveSlot(material->MetallicRoughnessMap, BuiltinAssetHandles::WhiteTexture).Texture;
		const GpuTexture& normalMap = state.ResolveSlot(material->NormalMap, BuiltinAssetHandles::FlatNormalTexture);
		// M23 (decision 10): the flags the resolved textures imply. A BC5 normal map holds x and y only, on a device with BC
		// and through the decode fallback alike, so the shader reconstructs z.
		const uint32_t textureFlags = normalMap.Format == TextureFormat::BC5Unorm ? MaterialFlagNormalMapTwoChannel : 0U;
		const std::array<nvrhi::TextureHandle, 5> textures = {
			baseColorMap,
			metallicRoughnessMap,
			normalMap.Texture,
			state.ResolveSlot(material->OcclusionMap, BuiltinAssetHandles::WhiteTexture).Texture,
			state.ResolveSlot(material->EmissiveMap, BuiltinAssetHandles::BlackTexture).Texture,
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
			// The material's own flags are its version's, which the key fixes; only the textures' can change here.
			const uint32_t flags = (mirror.Flags & ~MaterialFlagNormalMapTwoChannel) | textureFlags;
			if (current != textures || flags != mirror.Flags)
			{
				// A texture changed (a new version, or its placeholder replaced by the asset): new textures, the same constants
				// unless the flags the textures imply changed (decision 10), which the constants hold.
				assignTextures(entry.Value);
				if (flags != entry.Value.Flags)
				{
					state.WriteMaterialConstants(resolved, *material, flags, entry);
					state.ReportMaterialFailure(resolved, entry);
				}
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
		state.WriteMaterialConstants(resolved, *material, Utils::MakeMaterialConstants(*material).Flags | textureFlags, entry);
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
				state.ReportMirrorFailure(handle, std::string(AssetMissingCode),
					std::format("environment {} is missing; the view is lit with its fallback colour", id), "restore the asset, or assign another one");
			}
			else if (error.GetCode() == ErrorCode::InvalidArgument)
			{
				state.ReportMirrorFailure(handle, std::string(AssetTypeMismatchCode),
					std::format("asset {} cannot be used as an Environment; the view is lit with its fallback colour", id),
					"assign an asset of type Environment");
			}
			else
			{
				state.ReportMirrorFailure(handle, std::string(AssetImportFailedCode),
					std::format("environment {} failed to load; the view is lit with its fallback colour: {}", id,
						Error(error).WithHint(std::string()).ToString()),
					error.GetHint());
			}
		}
		else if (const AssetRef<EnvironmentData> environment = AssetCast<EnvironmentData>(*loaded))
		{
			const std::string name = Utils::GetAssetName(assets, handle);
			Result<GpuEnvironment> uploaded = Utils::UploadEnvironment(state.GetContext(), *environment, name);
			if (uploaded.has_value())
			{
				entry.Value = std::move(*uploaded);
				entry.IsValid = true;
			}
			else if (Utils::IsUploadFailure(*state.Device))
			{
				++state.UploadFailures;
				state.ReportMirrorFailure(handle, std::string(AssetUploadFailedCode),
					std::format("cannot upload environment '{}' to the GPU: {}; the view is lit with its fallback colour", name, uploaded.error().ToString()),
					"the upload is tried again when the asset changes; when the device is out of memory, use smaller environments");
			}
		}
		else
		{
			state.ReportMirrorFailure(handle, std::string(AssetTypeMismatchCode),
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
		CollectStaleEntries(m_State->IrradianceVolumes, *m_State->Assets, releaseUnused);
		CollectStaleEntries(m_State->Luts, *m_State->Assets, releaseUnused);
		CollectStaleEntries(m_State->Heightfields, *m_State->Assets, releaseUnused);
		m_State->Meshes.CollectStale(*m_State->Assets, releaseUnused);
		m_State->Textures.CollectStale(*m_State->Assets, releaseUnused);
		m_State->ReleaseUnboundDefaultAttributes();
	}

	void GpuResourceCache::Clear()
	{
		m_State->Materials.clear();
		m_State->Environments.clear();
		m_State->IrradianceVolumes.clear();
		m_State->Luts.clear();
		m_State->Heightfields.clear();
		m_State->Meshes.Clear();
		m_State->Textures.Clear();
		m_State->DefaultAttributes = {};
	}

	void GpuResourceCache::ReleaseGpuObjects()
	{
		// The mirrors and the shared default attribute stream hold every GPU object of the cache: an upload's command list is
		// executed at once and dropped, and the host images of HostImageUpload are the device's, which GraphicsDevice::Recover
		// releases. NextGeneration and UploadFailures stay, so no material generation from before the loss is ever made again.
		Clear();
	}

	void GpuResourceCache::TrimUnused()
	{
		// Only what nothing current needs; the use flags of the host's CollectStale period, the material generation counter
		// and UploadFailures stay, and the shared default attribute stream while a remaining mesh binds it
		// (GpuResourceCache.h).
		const AssetManager& assets = *m_State->Assets;
		std::erase_if(m_State->Materials, [&assets](const auto& item)
		{
			return IsTrimmable(assets, item.first, item.second.Value.IsPlaceholder);
		});
		std::erase_if(m_State->Environments, [&assets](const auto& item)
		{
			return IsTrimmable(assets, item.first, false);
		});
		std::erase_if(m_State->IrradianceVolumes, [&assets](const auto& item)
		{
			return IsTrimmable(assets, item.first, false);
		});
		std::erase_if(m_State->Luts, [&assets](const auto& item)
		{
			return IsTrimmable(assets, item.first, false);
		});
		std::erase_if(m_State->Heightfields, [&assets](const auto& item)
		{
			return IsTrimmable(assets, item.first, false);
		});
		m_State->Meshes.TrimUnused(assets);
		m_State->Textures.TrimUnused(assets);
		m_State->ReleaseUnboundDefaultAttributes();
	}

	GpuResourceCacheStats GpuResourceCache::GetStats() const
	{
		return {
			.MeshCount = m_State->Meshes.GetCount(),
			.TextureCount = m_State->Textures.GetCount(),
			.UploadFailures = m_State->UploadFailures,
			.MaterialCount = m_State->Materials.size(),
			.EnvironmentCount = m_State->Environments.size(),
			.IrradianceVolumeCount = m_State->IrradianceVolumes.size(),
			.LutCount = m_State->Luts.size(),
			.HeightfieldCount = m_State->Heightfields.size(),
		};
	}

	// M26 (C26.1, Docs/Decisions/0037-m26-contract.md decision 16; stream Q-B): as GetEnvironment, an irradiance volume without a
	// placeholder: a missing or failed one lights with the Environment.
	const GpuIrradianceVolume* GpuResourceCache::GetIrradianceVolume(AssetHandle handle)
	{
		if (!handle.IsValid())
			return nullptr;
		State& state = *m_State;
		AssetManager& assets = *state.Assets;
		// Load first: it publishes the first version (GetVersion is 0 before it), and remembers a failure itself (§7.2).
		const Result<AssetRef<Asset>> loaded = assets.Load(handle);
		const MirrorKey key = { .Handle = handle, .Version = assets.GetVersion(handle) };
		if (const auto found = state.IrradianceVolumes.find(key); found != state.IrradianceVolumes.end())
		{
			found->second.IsUsed = true;
			return found->second.IsValid ? &found->second.Value : nullptr;
		}

		IrradianceVolumeEntry entry;
		entry.IsUsed = true;
		const std::string id = handle.ToString();
		if (!loaded.has_value())
		{
			// GetOrPlaceholder's codes (AssetManager.h), so every manager reports alike.
			const Error& error = loaded.error();
			if (error.GetCode() == ErrorCode::NotFound)
			{
				state.ReportMirrorFailure(handle, std::string(AssetMissingCode),
					std::format("irradiance volume {} is missing; its probes light nothing", id), "bake the scene's lighting again, or assign another volume");
			}
			else if (error.GetCode() == ErrorCode::InvalidArgument)
			{
				state.ReportMirrorFailure(handle, std::string(AssetTypeMismatchCode),
					std::format("asset {} cannot be used as an IrradianceVolume; its probes light nothing", id), "assign an asset of type IrradianceVolume");
			}
			else
			{
				state.ReportMirrorFailure(handle, std::string(AssetImportFailedCode),
					std::format("irradiance volume {} failed to load; its probes light nothing: {}", id, Error(error).WithHint(std::string()).ToString()),
					error.GetHint());
			}
		}
		else if (const AssetRef<IrradianceVolumeData> volume = AssetCast<IrradianceVolumeData>(*loaded))
		{
			const std::string name = Utils::GetAssetName(assets, handle);
			Result<GpuIrradianceVolume> uploaded = Utils::UploadIrradianceVolume(state.GetContext(), *volume, name);
			if (uploaded.has_value())
			{
				entry.Value = std::move(*uploaded);
				entry.IsValid = true;
			}
			else if (Utils::IsUploadFailure(*state.Device))
			{
				++state.UploadFailures;
				state.ReportMirrorFailure(handle, std::string(AssetUploadFailedCode),
					std::format("cannot upload irradiance volume '{}' to the GPU: {}; its probes light nothing", name, uploaded.error().ToString()),
					"the upload is tried again when the asset changes; when the device is out of memory, use fewer or smaller volumes");
			}
		}
		else
		{
			state.ReportMirrorFailure(handle, std::string(AssetTypeMismatchCode),
				std::format("asset {} is a {}, not an IrradianceVolume; its probes light nothing", id, AssetTypeToString(assets.GetAssetType(handle))),
				"assign an asset of type IrradianceVolume");
		}
		entry.Value.Version = key.Version;
		IrradianceVolumeEntry& inserted = state.IrradianceVolumes.insert_or_assign(key, std::move(entry)).first->second;
		return inserted.IsValid ? &inserted.Value : nullptr;
	}

	const GpuLut* GpuResourceCache::GetLut(AssetHandle handle)
	{
		// M26 (stream X-B, decision 15): kept as GetEnvironment keeps environments; a LUT has no placeholder, so a failure
		// grades nothing.
		if (!handle.IsValid())
			return nullptr;
		State& state = *m_State;
		AssetManager& assets = *state.Assets;
		// Load first: it publishes the first version (GetVersion is 0 before it), and remembers a failure itself (§7.2).
		const Result<AssetRef<Asset>> loaded = assets.Load(handle);
		const MirrorKey key = { .Handle = handle, .Version = assets.GetVersion(handle) };
		if (const auto found = state.Luts.find(key); found != state.Luts.end())
		{
			found->second.IsUsed = true;
			return found->second.IsValid ? &found->second.Value : nullptr;
		}

		LutEntry entry;
		entry.IsUsed = true;
		const std::string id = handle.ToString();
		if (!loaded.has_value())
		{
			// GetOrPlaceholder's codes (AssetManager.h), so every manager reports alike.
			const Error& error = loaded.error();
			if (error.GetCode() == ErrorCode::NotFound)
			{
				state.ReportMirrorFailure(handle, std::string(AssetMissingCode), std::format("LUT {} is missing; the image is not graded", id),
					"restore the asset, or assign another one");
			}
			else if (error.GetCode() == ErrorCode::InvalidArgument)
			{
				state.ReportMirrorFailure(handle, std::string(AssetTypeMismatchCode),
					std::format("asset {} cannot be used as a LUT; the image is not graded", id), "assign an asset of type Lut");
			}
			else
			{
				state.ReportMirrorFailure(handle, std::string(AssetImportFailedCode),
					std::format("LUT {} failed to load; the image is not graded: {}", id, Error(error).WithHint(std::string()).ToString()),
					error.GetHint());
			}
		}
		else if (const AssetRef<LutData> lut = AssetCast<LutData>(*loaded))
		{
			const std::string name = Utils::GetAssetName(assets, handle);
			Result<GpuLut> uploaded = Utils::UploadLut(state.GetContext(), *lut, name);
			if (uploaded.has_value())
			{
				entry.Value = std::move(*uploaded);
				entry.IsValid = true;
			}
			else if (Utils::IsUploadFailure(*state.Device))
			{
				++state.UploadFailures;
				state.ReportMirrorFailure(handle, std::string(AssetUploadFailedCode),
					std::format("cannot upload LUT '{}' to the GPU: {}; the image is not graded", name, uploaded.error().ToString()),
					"the upload is tried again when the asset changes; when the device is out of memory, use a smaller LUT");
			}
		}
		else
		{
			state.ReportMirrorFailure(handle, std::string(AssetTypeMismatchCode),
				std::format("asset {} is a {}, not a LUT; the image is not graded", id, AssetTypeToString(assets.GetAssetType(handle))),
				"assign an asset of type Lut");
		}
		entry.Value.Version = key.Version;
		LutEntry& inserted = state.Luts.insert_or_assign(key, std::move(entry)).first->second;
		return inserted.IsValid ? &inserted.Value : nullptr;
	}

	const GpuHeightfield* GpuResourceCache::GetHeightfield(AssetHandle handle)
	{
		// M26 (stream T-C, decision 22): kept as GetEnvironment keeps environments; a heightfield has no placeholder, so a
		// failure draws no terrain.
		if (!handle.IsValid())
			return nullptr;
		State& state = *m_State;
		AssetManager& assets = *state.Assets;
		// Load first: it publishes the first version (GetVersion is 0 before it), and remembers a failure itself (§7.2).
		const Result<AssetRef<Asset>> loaded = assets.Load(handle);
		const MirrorKey key = { .Handle = handle, .Version = assets.GetVersion(handle) };
		if (const auto found = state.Heightfields.find(key); found != state.Heightfields.end())
		{
			found->second.IsUsed = true;
			return found->second.IsValid ? &found->second.Value : nullptr;
		}

		HeightfieldEntry entry;
		entry.IsUsed = true;
		const std::string id = handle.ToString();
		if (!loaded.has_value())
		{
			// GetOrPlaceholder's codes (AssetManager.h), so every manager reports alike.
			const Error& error = loaded.error();
			if (error.GetCode() == ErrorCode::NotFound)
			{
				state.ReportMirrorFailure(handle, std::string(AssetMissingCode), std::format("heightfield {} is missing; its terrain is not drawn", id),
					"restore the asset, or assign another heightfield");
			}
			else if (error.GetCode() == ErrorCode::InvalidArgument)
			{
				state.ReportMirrorFailure(handle, std::string(AssetTypeMismatchCode),
					std::format("asset {} cannot be used as a Heightfield; its terrain is not drawn", id), "assign an asset of type Heightfield");
			}
			else
			{
				state.ReportMirrorFailure(handle, std::string(AssetImportFailedCode),
					std::format("heightfield {} failed to load; its terrain is not drawn: {}", id, Error(error).WithHint(std::string()).ToString()),
					error.GetHint());
			}
		}
		else if (const AssetRef<HeightfieldData> heightfield = AssetCast<HeightfieldData>(*loaded))
		{
			const std::string name = Utils::GetAssetName(assets, handle);
			Result<GpuHeightfield> uploaded = Utils::UploadHeightfield(state.GetContext(), *heightfield, name);
			if (uploaded.has_value())
			{
				entry.Value = std::move(*uploaded);
				entry.IsValid = true;
			}
			else if (Utils::IsUploadFailure(*state.Device))
			{
				++state.UploadFailures;
				state.ReportMirrorFailure(handle, std::string(AssetUploadFailedCode),
					std::format("cannot upload heightfield '{}' to the GPU: {}; its terrain is not drawn", name, uploaded.error().ToString()),
					"the upload is tried again when the asset changes; when the device is out of memory, use a smaller heightfield");
			}
		}
		else
		{
			state.ReportMirrorFailure(handle, std::string(AssetTypeMismatchCode),
				std::format("asset {} is a {}, not a Heightfield; its terrain is not drawn", id, AssetTypeToString(assets.GetAssetType(handle))),
				"assign an asset of type Heightfield");
		}
		entry.Value.Version = key.Version;
		HeightfieldEntry& inserted = state.Heightfields.insert_or_assign(key, std::move(entry)).first->second;
		return inserted.IsValid ? &inserted.Value : nullptr;
	}

}
