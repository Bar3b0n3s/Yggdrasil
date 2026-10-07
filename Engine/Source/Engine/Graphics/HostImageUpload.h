#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <nvrhi/nvrhi.h>
#include <vulkan/vulkan_core.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

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
		// rows of blocks. At least the row size; a pitch whose rows, or a depth pitch whose slices, cannot be addressed
		// (the byte count overflows, or the row length in texels or the rows per slice exceed 32 bits) is InvalidArgument.
		size_t RowPitch = 0;
		// Bytes between depth slices of a 3D texture; 0 means RowPitch times the row count.
		size_t DepthPitch = 0;
	};

	enum class TextureUploadPath : uint8_t
	{
		// A native VkImage with VK_IMAGE_USAGE_HOST_TRANSFER_BIT in device-local memory from the upload's block allocator,
		// written with vkCopyMemoryToImage and put into its final layout with vkTransitionImageLayout, without a staging
		// buffer or command list; wrapped with createHandleForNativeTexture as a keepInitialState texture whose initial state
		// is ShaderResource. NVRHI does not own the image: HostImageUpload destroys it through a deferred-release queue keyed
		// by the last submission ID.
		HostImageCopy,
		// NVRHI's writeTexture through a command list submitted on the graphics queue, into a keepInitialState texture whose
		// initial state is ShaderResource. Neither path uses setPermanentTextureState: NVRHI refuses to copy out of a texture
		// in a permanent state, which Readback (and the test comparing the two paths) needs.
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

		// Whether `format` can take the host-copy path at all: IsHostImageCopyAvailable and the format's optimal-tiling
		// features include VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT, VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT and
		// VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT. The answer is queried once per format and cached.
		[[nodiscard]] bool SupportsHostImageCopy(nvrhi::Format format) const;
		// Whether CreateTexture takes the host-copy path for `desc`: SupportsHostImageCopy(desc.format), and
		// vkGetPhysicalDeviceImageFormatProperties2 accepts the image the path creates (its image type, the cube flag of
		// cube textures, sampled, transfer-source and host-transfer usage) with limits that hold desc's extent, mip count
		// and array size. Vulkan forbids creating an image outside those limits, so any other desc takes the staging path.
		[[nodiscard]] bool SupportsHostImageCopy(const nvrhi::TextureDesc& desc) const;

		// Creates an immutable sampled texture with `desc` and fills it from `subresources`, which must cover every mip
		// level of every array slice exactly once. Takes the host-copy path when `preferredPath` is HostImageCopy and
		// SupportsHostImageCopy(desc), the staging path otherwise; the returned TextureUpload::Path says which. The texture
		// is in the ShaderResource state when this returns (keepInitialState: every command list that uses it starts from
		// ShaderResource and returns it there), and later GPU work on the graphics queue sees its contents. Errors:
		// InvalidArgument for a desc that is a render target, a UAV, has zero extent or a 1D texture whose height is not 1,
		// for missing, duplicated or out-of-range subresources, for data smaller than the subresource and for pitches that
		// cannot be addressed (TextureSubresourceData::RowPitch); Gpu when creation or the host copy fails (including
		// --gpu-inject-fault=oom-texture, GpuDiagnostics::ShouldFailTextureCreation, on both paths), naming desc.debugName.
		// Never crashes on a failed creation (§8.14 item 7).
		[[nodiscard]] Result<TextureUpload> CreateTexture(const nvrhi::TextureDesc& desc, std::span<const TextureSubresourceData> subresources,
			TextureUploadPath preferredPath = TextureUploadPath::HostImageCopy);

		// The deferred-release step (see the class comment).
		void CollectGarbage();

		// Host-copied images that exist: in use, or queued for release.
		[[nodiscard]] size_t GetHostImageCount() const;
		// Host-copied images queued for release whose submission has not completed yet.
		[[nodiscard]] size_t GetPendingReleaseCount() const;
	private:
		// A byte range of a MemoryBlock.
		struct MemoryRange
		{
			VkDeviceSize Offset = 0;
			VkDeviceSize Size = 0;
		};

		// One block of device-local memory that host-copied images are suballocated from, or the dedicated allocation of a
		// single large image. FreeRanges is sorted by offset and never holds two adjacent ranges.
		struct MemoryBlock
		{
			VkDeviceMemory Memory = VK_NULL_HANDLE;
			VkDeviceSize Size = 0;
			uint32_t MemoryTypeIndex = 0;
			bool IsDedicated = false;
			uint32_t AllocationCount = 0;
			std::vector<MemoryRange> FreeRanges;
		};

		// Where one image's memory lives. Block is a non-owning pointer into m_MemoryBlocks, valid while the block exists.
		struct MemoryAllocation
		{
			MemoryBlock* Block = nullptr;
			MemoryRange Range{};
		};

		// An image in use: the texture handle keeps the NVRHI wrapper alive while anyone else references it.
		struct HostImage
		{
			nvrhi::TextureHandle Texture{};
			VkImage Image = VK_NULL_HANDLE;
			MemoryAllocation Memory{};
		};

		// An image nobody references any more, destroyed once the graphics queue has completed SubmissionID.
		struct PendingRelease
		{
			VkImage Image = VK_NULL_HANDLE;
			MemoryAllocation Memory{};
			uint64_t SubmissionID = 0;
		};

		// The cached answer of SupportsHostImageCopy for one format.
		enum class FormatSupport : uint8_t
		{
			Unknown,
			Unsupported,
			Supported
		};
	private:
		[[nodiscard]] Result<TextureUpload> CreateStagedTexture(const nvrhi::TextureDesc& desc,
			std::span<const TextureSubresourceData> subresources);
		[[nodiscard]] Result<TextureUpload> CreateHostCopiedTexture(const nvrhi::TextureDesc& desc,
			std::span<const TextureSubresourceData> subresources);
		// Suballocates (or, for a large image, allocates) device-local memory that satisfies `requirements`.
		[[nodiscard]] Result<MemoryAllocation> AllocateMemory(const VkMemoryRequirements& requirements, bool dedicated, VkImage image);
		void FreeMemory(const MemoryAllocation& allocation);
		// Destroys the image and returns its memory, and counts it as destroyed unless `leaked` (a handle that is still held
		// elsewhere keeps its HostImage live count, so the device's leak report names it).
		void DestroyImage(VkImage image, const MemoryAllocation& memory, bool leaked);
	private:
		GraphicsDevice* m_Device = nullptr; // documented back-reference: owns and outlives the upload
		bool m_IsHostImageCopyAvailable = false;
		// Per nvrhi::Format; a cache, filled by the const SupportsHostImageCopy.
		mutable std::array<FormatSupport, static_cast<size_t>(nvrhi::Format::COUNT)> m_FormatSupport{};
		VkPhysicalDeviceMemoryProperties m_MemoryProperties{};
		std::vector<Scope<MemoryBlock>> m_MemoryBlocks;
		std::vector<HostImage> m_Images;
		std::vector<PendingRelease> m_PendingReleases;
	};

	// "HostImageCopy" or "Staging".
	[[nodiscard]] std::string_view TextureUploadPathToString(TextureUploadPath path);

}
