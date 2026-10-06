#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Immutable texture uploads (Architecture §8.1 "Vulkan 1.4 policy", path 1): Vulkan 1.4 host image copy where the device
// and the format allow it, NVRHI's writeTexture through a command list otherwise. GpuResourceCache (M6) creates asset
// textures and baked environment cubes through it; the test "Texture upload: host-copy and staging paths read back
// identically" proves both paths produce the same texels for every texture format the engine uses.

namespace Engine {

	class GraphicsDevice;

	// The pixels of one subresource of a texture being created.
	struct TextureSubresourceData
	{
		uint32_t MipLevel = 0;
		uint32_t ArraySlice = 0; // array layer, or cube face (6 per cube) in face order +X, -X, +Y, -Y, +Z, -Z
		// The texels, rows top first. Must hold at least DepthPitch * depth bytes of the mip (InvalidArgument otherwise).
		std::span<const std::byte> Data{};
		// Bytes between rows; 0 means tightly packed (the format's row size at this mip). Block-compressed formats count
		// rows of blocks.
		size_t RowPitch = 0;
		// Bytes between depth slices of a 3D texture; 0 means RowPitch times the row count.
		size_t DepthPitch = 0;
	};

	enum class TextureUploadPath : uint8_t
	{
		// A native VkImage with VK_IMAGE_USAGE_HOST_TRANSFER_BIT in device-local memory from the upload's block allocator,
		// written with vkCopyMemoryToImage and put into its final layout with vkTransitionImageLayout, without a staging
		// buffer or command list; wrapped with createHandleForNativeTexture and setPermanentTextureState. NVRHI does not own
		// the image: HostImageUpload destroys it through a deferred-release queue keyed by the last submission ID.
		HostImageCopy,
		// NVRHI's writeTexture through a command list submitted on the graphics queue, then setPermanentTextureState.
		Staging
	};

	// The texture an upload created, and the path that created it.
	struct TextureUpload
	{
		nvrhi::TextureHandle Texture{};
		TextureUploadPath Path = TextureUploadPath::Staging;
	};

	// Owned by GraphicsDevice (GetHostImageUpload). Not copyable or movable. Main thread only (§4.11).
	//
	// Host-copied images are released like NVRHI releases its own objects: CollectGarbage (from
	// GraphicsDevice::RunGarbageCollection, once per frame, after NVRHI's runGarbageCollection) finds the images whose
	// texture handle nobody besides the upload references (GpuResourceTracker::HasOtherReferences(texture, 1); the
	// wrapper itself is untracked, GraphicsDevice::CreateHandleForNativeTexture), drops the handle, which destroys the
	// wrapper, queues the VkImage with the device's last submission ID, and destroys the VkImage and returns its memory
	// once the graphics queue's timeline has passed that ID. GpuResourceTracker counts them as GpuResourceType::HostImage
	// (RecordCreated when the image is created, RecordDestroyed when it is destroyed).
	class HostImageUpload
	{
	public:
		// `device` is a documented back-reference: the device owns the upload and outlives it.
		explicit HostImageUpload(GraphicsDevice& device);
		// Drops every remaining texture handle, then destroys every host-copied image and the block allocator's memory. The
		// device has waited for idle and retired its command lists before (its destructor does), so no submission still uses
		// them; a handle someone else still holds is a leak, reported by its live count.
		~HostImageUpload();

		HostImageUpload(const HostImageUpload&) = delete;
		HostImageUpload& operator=(const HostImageUpload&) = delete;

		// Whether the device enabled hostImageCopy: API 1.4 and the feature (false under --vulkan-api=1.3).
		[[nodiscard]] bool IsHostImageCopyAvailable() const;

		// Whether `format` can take the host-copy path: IsHostImageCopyAvailable and the format's optimal-tiling features
		// include VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT and VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT. The answer is
		// queried once per format and cached.
		[[nodiscard]] bool SupportsHostImageCopy(nvrhi::Format format) const;

		// Creates an immutable sampled texture with `desc` and fills it from `subresources`, which must cover every mip
		// level of every array slice exactly once. Takes the host-copy path when `preferredPath` is HostImageCopy and
		// SupportsHostImageCopy(desc.format), the staging path otherwise; the returned TextureUpload::Path says which. The
		// texture is in its permanent ShaderResource state when this returns, and later GPU work on the graphics queue sees
		// its contents. Errors: InvalidArgument for a desc that is a render target, a UAV or has zero extent, for missing,
		// duplicated or out-of-range subresources, and for data smaller than the subresource; Gpu when creation or the host
		// copy fails (including --gpu-inject-fault=oom-texture, GpuDiagnostics::ShouldFailTextureCreation, on both paths),
		// naming desc.debugName. Never crashes on a failed creation (§8.14 item 7).
		[[nodiscard]] Result<TextureUpload> CreateTexture(const nvrhi::TextureDesc& desc, std::span<const TextureSubresourceData> subresources,
			TextureUploadPath preferredPath = TextureUploadPath::HostImageCopy);

		// The deferred-release step (see the class comment).
		void CollectGarbage();

		// Host-copied images that exist: in use, or queued for release.
		[[nodiscard]] size_t GetHostImageCount() const;
		// Host-copied images queued for release whose submission has not completed yet.
		[[nodiscard]] size_t GetPendingReleaseCount() const;
	};

	// "HostImageCopy" or "Staging".
	[[nodiscard]] std::string_view TextureUploadPathToString(TextureUploadPath path);

}
