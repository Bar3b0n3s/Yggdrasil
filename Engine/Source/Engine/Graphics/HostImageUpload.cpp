#include "EnginePCH.h"
#include "Engine/Graphics/HostImageUpload.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GpuDiagnostics.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <utility>

// Both upload paths of Architecture §8.1 path 1. The host-copy path calls Vulkan through the dispatcher's C entry points,
// which report failures as VkResult values, and leaves each image in SHADER_READ_ONLY_OPTIMAL. NVRHI learns that layout
// through TextureDesc::keepInitialState with initialState ShaderResource (both paths): every command list that uses the
// texture starts from ShaderResource and returns it there, so copies out of it (Readback) work, which a permanent
// state would refuse.

namespace Engine {

	namespace {

		// The memory layout of one subresource's texels as the caller provides them.
		struct SubresourceLayout
		{
			uint32_t Width = 1;
			uint32_t Height = 1;
			uint32_t Depth = 1;
			size_t RowPitch = 0;   // bytes between rows of texel blocks
			size_t DepthPitch = 0; // bytes between depth slices
			size_t TightRowSize = 0;
			size_t RowCount = 0; // rows of texel blocks
		};

	}

	namespace Utils {

		// Device memory blocks for host-copied images: small, so an engine with few textures wastes little, and large
		// enough to keep the allocation count far below maxMemoryAllocationCount (§8.14 item 5).
		constexpr VkDeviceSize HostImageBlockSize = 16ull * 1024 * 1024;
		// Images larger than this get a dedicated allocation instead of a block's range.
		constexpr VkDeviceSize MaxSuballocatedImageSize = HostImageBlockSize / 4;

		static std::string DescribeUploadTexture(const nvrhi::TextureDesc& desc)
		{
			return std::format("'{}' ({}x{} {})", desc.debugName.empty() ? std::string("unnamed") : desc.debugName, desc.width, desc.height,
				nvrhi::getFormatInfo(desc.format).name);
		}

		static uint32_t GetMipExtent(uint32_t extent, uint32_t mipLevel)
		{
			return std::max(extent >> mipLevel, 1u);
		}

		static bool IsCube(nvrhi::TextureDimension dimension)
		{
			return dimension == nvrhi::TextureDimension::TextureCube || dimension == nvrhi::TextureDimension::TextureCubeArray;
		}

		static bool Is1D(nvrhi::TextureDimension dimension)
		{
			return dimension == nvrhi::TextureDimension::Texture1D || dimension == nvrhi::TextureDimension::Texture1DArray;
		}

		// The image the host-copy path creates for `desc`: its type and create flags, and the usage of every host-copied
		// image (sampled, copied from by Readback, written by the host).
		static VkImageType GetHostImageType(const nvrhi::TextureDesc& desc)
		{
			if (desc.dimension == nvrhi::TextureDimension::Texture3D)
				return VK_IMAGE_TYPE_3D;
			return Is1D(desc.dimension) ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
		}

		static VkImageCreateFlags GetHostImageFlags(const nvrhi::TextureDesc& desc)
		{
			return IsCube(desc.dimension) ? static_cast<VkImageCreateFlags>(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) : 0u;
		}

		constexpr VkImageUsageFlags HostImageUsage =
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT;

		// Whether `a * b` fits size_t.
		static bool IsProductRepresentable(size_t a, size_t b)
		{
			return a == 0 || b <= std::numeric_limits<size_t>::max() / a;
		}

		static SubresourceLayout GetSubresourceLayout(const nvrhi::TextureDesc& desc, const TextureSubresourceData& subresource)
		{
			const nvrhi::FormatInfo& format = nvrhi::getFormatInfo(desc.format);
			SubresourceLayout layout;
			layout.Width = GetMipExtent(desc.width, subresource.MipLevel);
			layout.Height = GetMipExtent(desc.height, subresource.MipLevel);
			layout.Depth = desc.dimension == nvrhi::TextureDimension::Texture3D ? GetMipExtent(desc.depth, subresource.MipLevel) : 1;
			const size_t columns = (layout.Width + format.blockSize - 1) / format.blockSize;
			layout.RowCount = (layout.Height + format.blockSize - 1) / format.blockSize;
			layout.TightRowSize = columns * format.bytesPerBlock;
			layout.RowPitch = subresource.RowPitch != 0 ? subresource.RowPitch : layout.TightRowSize;
			// A default depth pitch that overflows stays 0, which ValidateUpload rejects before anyone uses the layout.
			const bool sliceFits = IsProductRepresentable(layout.RowPitch, layout.RowCount);
			const size_t defaultDepthPitch = sliceFits ? layout.RowPitch * layout.RowCount : 0;
			layout.DepthPitch = subresource.DepthPitch != 0 ? subresource.DepthPitch : defaultDepthPitch;
			return layout;
		}

		// The checks of one subresource's layout against its data (InvalidArgument for each problem). Every product is
		// checked for overflow: a caller's pitch is external input, and a wrapped size would let a short span pass.
		static Status ValidateSubresourceLayout(const std::string& name, const nvrhi::FormatInfo& format, const TextureSubresourceData& subresource,
			const SubresourceLayout& layout)
		{
			if (layout.RowPitch < layout.TightRowSize)
			{
				return MakeError(ErrorCode::InvalidArgument, "texture {} mip {}, slice {}: row pitch {} is below the row size {}", name,
					subresource.MipLevel, subresource.ArraySlice, layout.RowPitch, layout.TightRowSize);
			}
			if (!IsProductRepresentable(layout.RowPitch, layout.RowCount))
			{
				return MakeError(ErrorCode::InvalidArgument, "texture {} mip {}, slice {}: row pitch {} times {} rows overflows", name,
					subresource.MipLevel, subresource.ArraySlice, layout.RowPitch, layout.RowCount);
			}
			if (layout.DepthPitch < layout.RowPitch * layout.RowCount)
			{
				return MakeError(ErrorCode::InvalidArgument, "texture {} mip {}, slice {}: depth pitch {} is below {} rows of {} bytes", name,
					subresource.MipLevel, subresource.ArraySlice, layout.DepthPitch, layout.RowCount, layout.RowPitch);
			}
			if (!IsProductRepresentable(layout.DepthPitch, layout.Depth))
			{
				return MakeError(ErrorCode::InvalidArgument, "texture {} mip {}, slice {}: depth pitch {} times {} slices overflows", name,
					subresource.MipLevel, subresource.ArraySlice, layout.DepthPitch, layout.Depth);
			}
			// The host copy addresses rows in texels and slices in rows, both 32-bit (VkMemoryToImageCopy).
			const uint64_t rowLength = static_cast<uint64_t>(layout.RowPitch / format.bytesPerBlock) * format.blockSize;
			const uint64_t sliceRows = static_cast<uint64_t>(layout.DepthPitch / layout.RowPitch) * format.blockSize;
			if (rowLength > std::numeric_limits<uint32_t>::max() || sliceRows > std::numeric_limits<uint32_t>::max())
			{
				return MakeError(ErrorCode::InvalidArgument, "texture {} mip {}, slice {}: row pitch {} or depth pitch {} is too large to address",
					name, subresource.MipLevel, subresource.ArraySlice, layout.RowPitch, layout.DepthPitch);
			}
			const size_t required = layout.DepthPitch * layout.Depth;
			if (subresource.Data.size() < required)
			{
				return MakeError(ErrorCode::InvalidArgument, "texture {} mip {}, slice {}: {} bytes of data, {} needed", name, subresource.MipLevel,
					subresource.ArraySlice, subresource.Data.size(), required);
			}
			return {};
		}

		// The checks every upload makes before creating anything (InvalidArgument for each problem).
		static Status ValidateUpload(const nvrhi::TextureDesc& desc, std::span<const TextureSubresourceData> subresources)
		{
			const std::string name = DescribeUploadTexture(desc);
			if (desc.isRenderTarget || desc.isUAV)
				return MakeError(ErrorCode::InvalidArgument, "texture {} is a render target or UAV, not an immutable texture", name);
			if (desc.isVirtual || desc.isTiled || desc.isTypeless || desc.sampleCount != 1)
				return MakeError(ErrorCode::InvalidArgument, "texture {} is virtual, tiled, typeless or multisampled, not an immutable texture", name);
			if (desc.width == 0 || desc.height == 0 || desc.depth == 0 || desc.arraySize == 0 || desc.mipLevels == 0)
				return MakeError(ErrorCode::InvalidArgument, "texture {} has a zero extent, array size or mip count", name);
			if (desc.format == nvrhi::Format::UNKNOWN)
				return MakeError(ErrorCode::InvalidArgument, "texture {} has no format", name);

			switch (desc.dimension)
			{
				case nvrhi::TextureDimension::Texture1D:
				case nvrhi::TextureDimension::Texture2D:
				case nvrhi::TextureDimension::Texture3D:
					if (desc.arraySize != 1)
						return MakeError(ErrorCode::InvalidArgument, "texture {} is not an array but has {} array slices", name, desc.arraySize);
					break;
				case nvrhi::TextureDimension::Texture1DArray:
				case nvrhi::TextureDimension::Texture2DArray:
					break;
				case nvrhi::TextureDimension::TextureCube:
				case nvrhi::TextureDimension::TextureCubeArray:
					if (desc.arraySize % 6 != 0)
						return MakeError(ErrorCode::InvalidArgument, "cube texture {} has {} array slices, not a multiple of 6", name, desc.arraySize);
					break;
				default:
					return MakeError(ErrorCode::InvalidArgument, "texture {} has a dimension an immutable texture cannot have", name);
			}
			if (desc.dimension != nvrhi::TextureDimension::Texture3D && desc.depth != 1)
				return MakeError(ErrorCode::InvalidArgument, "texture {} is not 3D but has depth {}", name, desc.depth);
			if (Is1D(desc.dimension) && desc.height != 1)
				return MakeError(ErrorCode::InvalidArgument, "texture {} is 1D but has height {}", name, desc.height);

			const uint32_t largest = std::max({ desc.width, desc.height, desc.dimension == nvrhi::TextureDimension::Texture3D ? desc.depth : 1u });
			const uint32_t maxMipLevels = static_cast<uint32_t>(std::bit_width(largest));
			if (desc.mipLevels > maxMipLevels)
				return MakeError(ErrorCode::InvalidArgument, "texture {} has {} mips, at most {} fit its size", name, desc.mipLevels, maxMipLevels);

			// Every (mip, slice) exactly once. The count is compared first, so the bookkeeping below is bounded by the data the
			// caller really passed, whatever array size the desc claims (at most 32 mips times 2^32 slices fits size_t).
			const nvrhi::FormatInfo& format = nvrhi::getFormatInfo(desc.format);
			const size_t expected = static_cast<size_t>(desc.mipLevels) * desc.arraySize;
			if (subresources.size() != expected)
			{
				return MakeError(ErrorCode::InvalidArgument, "texture {} needs data for {} subresources ({} mips, {} slices), got {}", name, expected,
					desc.mipLevels, desc.arraySize, subresources.size());
			}
			std::vector<bool> seen(expected, false);
			for (const TextureSubresourceData& subresource : subresources)
			{
				if (subresource.MipLevel >= desc.mipLevels || subresource.ArraySlice >= desc.arraySize)
				{
					return MakeError(ErrorCode::InvalidArgument, "texture {} has no subresource mip {}, slice {}", name, subresource.MipLevel,
						subresource.ArraySlice);
				}
				const size_t index = static_cast<size_t>(subresource.ArraySlice) * desc.mipLevels + subresource.MipLevel;
				if (seen[index])
				{
					return MakeError(ErrorCode::InvalidArgument, "texture {} has the data of mip {}, slice {} twice", name, subresource.MipLevel,
						subresource.ArraySlice);
				}
				seen[index] = true;
				ENGINE_TRY(ValidateSubresourceLayout(name, format, subresource, GetSubresourceLayout(desc, subresource)));
			}
			return {};
		}

		// The first memory type `memoryTypeBits` allows that is device-local, or any allowed one (a device whose memory is
		// all host-visible, such as a CPU implementation, has no other).
		static std::optional<uint32_t> FindMemoryType(const VkPhysicalDeviceMemoryProperties& properties, uint32_t memoryTypeBits)
		{
			std::optional<uint32_t> anyAllowed;
			for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
			{
				if ((memoryTypeBits & (1u << index)) == 0)
					continue;
				if ((properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)
					return index;
				if (!anyAllowed.has_value())
					anyAllowed = index;
			}
			return anyAllowed;
		}

		static VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
		{
			return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
		}

	}

	HostImageUpload::HostImageUpload(GraphicsDevice& device)
		: m_Device(&device)
	{
		const vk::detail::DispatchLoaderDynamic& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
		dispatcher.vkGetPhysicalDeviceMemoryProperties(m_Device->GetVulkanPhysicalDevice(), &m_MemoryProperties);
		m_IsHostImageCopyAvailable = m_Device->GetInfo().HostImageCopy;
		if (m_IsHostImageCopyAvailable && (dispatcher.vkCopyMemoryToImage == nullptr || dispatcher.vkTransitionImageLayout == nullptr))
		{
			ENGINE_CORE_WARN("Host image copy is disabled: the device exposes hostImageCopy without vkCopyMemoryToImage and vkTransitionImageLayout");
			m_IsHostImageCopyAvailable = false;
		}
	}

	HostImageUpload::~HostImageUpload()
	{
		// The device waited for idle and retired its command lists before (its destructor), so no submission uses these.
		for (HostImage& image : m_Images)
		{
			const bool leaked = m_Device->GetResourceTracker().HasOtherReferences(*image.Texture, 1);
			if (leaked)
				ENGINE_CORE_ERROR("Host-copied texture '{}' is still referenced when its device is destroyed", image.Texture->getDesc().debugName);
			image.Texture = nullptr;
			DestroyImage(image.Image, image.Memory, leaked);
		}
		m_Images.clear();
		for (const PendingRelease& release : m_PendingReleases)
			DestroyImage(release.Image, release.Memory, false);
		m_PendingReleases.clear();
		ENGINE_CORE_ASSERT(m_MemoryBlocks.empty(), "HostImageUpload memory blocks outlive their images");
	}

	bool HostImageUpload::IsHostImageCopyAvailable() const
	{
		return m_IsHostImageCopyAvailable;
	}

	bool HostImageUpload::SupportsHostImageCopy(nvrhi::Format format) const
	{
		if (!m_IsHostImageCopyAvailable)
			return false;
		const size_t index = static_cast<size_t>(format);
		if (format == nvrhi::Format::UNKNOWN || index >= m_FormatSupport.size())
			return false;
		if (m_FormatSupport[index] != FormatSupport::Unknown)
			return m_FormatSupport[index] == FormatSupport::Supported;

		const vk::detail::DispatchLoaderDynamic& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
		const VkPhysicalDevice physicalDevice = m_Device->GetVulkanPhysicalDevice();
		const VkFormat vulkanFormat = nvrhi::vulkan::convertFormat(format);
		const nvrhi::FormatInfo& info = nvrhi::getFormatInfo(format);

		bool supported = vulkanFormat != VK_FORMAT_UNDEFINED && !info.hasDepth && !info.hasStencil;
		if (supported)
		{
			VkFormatProperties3 properties3{};
			properties3.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;
			VkFormatProperties2 properties{};
			properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
			properties.pNext = &properties3;
			dispatcher.vkGetPhysicalDeviceFormatProperties2(physicalDevice, vulkanFormat, &properties);
			constexpr VkFormatFeatureFlags2 RequiredFeatures =
				VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
			supported = (properties3.optimalTilingFeatures & RequiredFeatures) == RequiredFeatures;
		}
		m_FormatSupport[index] = supported ? FormatSupport::Supported : FormatSupport::Unsupported;
		return supported;
	}

	bool HostImageUpload::SupportsHostImageCopy(const nvrhi::TextureDesc& desc) const
	{
		if (!SupportsHostImageCopy(desc.format))
			return false;

		// The image itself, which the format features alone do not guarantee: Vulkan forbids creating an image the
		// device's image format properties do not cover (VUID-VkImageCreateInfo-imageCreateMaxMipLevels-02251).
		VkPhysicalDeviceImageFormatInfo2 imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
		imageInfo.format = nvrhi::vulkan::convertFormat(desc.format);
		imageInfo.type = Utils::GetHostImageType(desc);
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = Utils::HostImageUsage;
		imageInfo.flags = Utils::GetHostImageFlags(desc);
		VkImageFormatProperties2 imageProperties{};
		imageProperties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
		const VkResult result =
			VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceImageFormatProperties2(m_Device->GetVulkanPhysicalDevice(), &imageInfo, &imageProperties);
		if (result != VK_SUCCESS)
			return false;
		const VkImageFormatProperties& limits = imageProperties.imageFormatProperties;
		return desc.width <= limits.maxExtent.width && desc.height <= limits.maxExtent.height && desc.depth <= limits.maxExtent.depth
			&& desc.mipLevels <= limits.maxMipLevels && desc.arraySize <= limits.maxArrayLayers
			&& (limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0;
	}

	Result<TextureUpload> HostImageUpload::CreateTexture(const nvrhi::TextureDesc& desc, std::span<const TextureSubresourceData> subresources,
		TextureUploadPath preferredPath)
	{
		ENGINE_TRY(Utils::ValidateUpload(desc, subresources));

		// Immutable and sampled: NVRHI keeps the texture in ShaderResource between command lists (see the file comment).
		nvrhi::TextureDesc immutable = desc;
		immutable.isShaderResource = true;
		immutable.initialState = nvrhi::ResourceStates::ShaderResource;
		immutable.keepInitialState = true;

		if (preferredPath == TextureUploadPath::HostImageCopy && SupportsHostImageCopy(desc))
			return CreateHostCopiedTexture(immutable, subresources);
		return CreateStagedTexture(immutable, subresources);
	}

	Result<TextureUpload> HostImageUpload::CreateStagedTexture(const nvrhi::TextureDesc& desc, std::span<const TextureSubresourceData> subresources)
	{
		// Not an immediate command list: NVRHI's validation allows one open immediate list at a time, and the scene renderer
		// uploads the textures and environments a frame needs while the frame's own list is open (GpuResourceCache, M8).
		ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle commandList,
			m_Device->CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
		ENGINE_TRY_ASSIGN(nvrhi::TextureHandle texture, m_Device->CreateTexture(desc));

		// writeTexture copies the texels into NVRHI's upload buffer while recording; closing the list returns the texture
		// from CopyDest to its kept initial state.
		commandList->open();
		for (const TextureSubresourceData& subresource : subresources)
		{
			const SubresourceLayout layout = Utils::GetSubresourceLayout(desc, subresource);
			commandList->writeTexture(texture, subresource.ArraySlice, subresource.MipLevel, subresource.Data.data(), layout.RowPitch,
				layout.DepthPitch);
		}
		commandList->close();
		m_Device->ExecuteCommandList(*commandList);
		return TextureUpload{ .Texture = std::move(texture), .Path = TextureUploadPath::Staging };
	}

	Result<TextureUpload> HostImageUpload::CreateHostCopiedTexture(const nvrhi::TextureDesc& desc,
		std::span<const TextureSubresourceData> subresources)
	{
		const std::string name = Utils::DescribeUploadTexture(desc);
		if (m_Device->GetDiagnostics().ShouldFailTextureCreation(desc))
		{
			return MakeError(ErrorCode::Gpu, "cannot create texture {}: out of device memory (injected by --gpu-inject-fault={})", name,
				GpuFaultToString(GpuFault::OomTexture));
		}

		// The command list that tells NVRHI the image's state comes first, so a failure leaves nothing behind. Not an immediate
		// one, for the reason CreateStagedTexture gives.
		ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle commandList,
			m_Device->CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));

		const vk::detail::DispatchLoaderDynamic& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
		const VkDevice device = m_Device->GetVulkanDevice();
		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.flags = Utils::GetHostImageFlags(desc);
		imageInfo.imageType = Utils::GetHostImageType(desc);
		imageInfo.format = nvrhi::vulkan::convertFormat(desc.format);
		imageInfo.extent = { desc.width, desc.height, desc.depth };
		imageInfo.mipLevels = desc.mipLevels;
		imageInfo.arrayLayers = desc.arraySize;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = Utils::HostImageUsage;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkImage image = VK_NULL_HANDLE;
		const VkResult created = dispatcher.vkCreateImage(device, &imageInfo, nullptr, &image);
		if (created != VK_SUCCESS)
			return MakeError(ErrorCode::Gpu, "cannot create texture {}: vkCreateImage failed: {}", name, VkResultToString(created));

		VkMemoryDedicatedRequirements dedicated{};
		dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
		VkMemoryRequirements2 requirements{};
		requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
		requirements.pNext = &dedicated;
		VkImageMemoryRequirementsInfo2 requirementsInfo{};
		requirementsInfo.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
		requirementsInfo.image = image;
		dispatcher.vkGetImageMemoryRequirements2(device, &requirementsInfo, &requirements);
		const bool wantsDedicated = dedicated.prefersDedicatedAllocation == VK_TRUE || dedicated.requiresDedicatedAllocation == VK_TRUE;
		Result<MemoryAllocation> memory = AllocateMemory(requirements.memoryRequirements, wantsDedicated, image);
		if (!memory.has_value())
		{
			dispatcher.vkDestroyImage(device, image, nullptr);
			return std::unexpected(std::move(memory).error().WithContext(std::format("while creating texture {}", name)));
		}

		// Any failure from here on destroys the image and returns its memory.
		const auto fail = [this, device, image, &memory, &name](std::string_view call, VkResult result) -> std::unexpected<Error>
		{
			FreeMemory(*memory);
			VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyImage(device, image, nullptr);
			return MakeError(ErrorCode::Gpu, "cannot create texture {}: {} failed: {}", name, call, VkResultToString(result));
		};

		const VkResult bound = dispatcher.vkBindImageMemory(device, image, memory->Block->Memory, memory->Range.Offset);
		if (bound != VK_SUCCESS)
			return fail("vkBindImageMemory", bound);

		// Straight into the final layout, which the device accepts as a host copy destination (GraphicsDevice checks it).
		VkImageSubresourceRange allSubresources{};
		allSubresources.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		allSubresources.baseMipLevel = 0;
		allSubresources.levelCount = desc.mipLevels;
		allSubresources.baseArrayLayer = 0;
		allSubresources.layerCount = desc.arraySize;
		VkHostImageLayoutTransitionInfo transition{};
		transition.sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO;
		transition.image = image;
		transition.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		transition.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		transition.subresourceRange = allSubresources;
		const VkResult transitioned = dispatcher.vkTransitionImageLayout(device, 1, &transition);
		if (transitioned != VK_SUCCESS)
			return fail("vkTransitionImageLayout", transitioned);

		// One region per subresource. Pitches the copy cannot express in texels are repacked tightly first.
		const nvrhi::FormatInfo& format = nvrhi::getFormatInfo(desc.format);
		std::vector<std::vector<std::byte>> repacked;
		repacked.reserve(subresources.size()); // the regions point into these buffers
		std::vector<VkMemoryToImageCopy> regions;
		regions.reserve(subresources.size());
		for (const TextureSubresourceData& subresource : subresources)
		{
			const SubresourceLayout layout = Utils::GetSubresourceLayout(desc, subresource);
			const void* texels = subresource.Data.data();
			size_t rowPitch = layout.RowPitch;
			size_t depthPitch = layout.DepthPitch;
			if (rowPitch % format.bytesPerBlock != 0 || depthPitch % rowPitch != 0)
			{
				std::vector<std::byte>& tight = repacked.emplace_back(layout.TightRowSize * layout.RowCount * layout.Depth);
				for (size_t slice = 0; slice < layout.Depth; ++slice)
				{
					for (size_t row = 0; row < layout.RowCount; ++row)
					{
						std::memcpy(tight.data() + (slice * layout.RowCount + row) * layout.TightRowSize,
							subresource.Data.data() + slice * depthPitch + row * rowPitch, layout.TightRowSize);
					}
				}
				texels = tight.data();
				rowPitch = layout.TightRowSize;
				depthPitch = layout.TightRowSize * layout.RowCount;
			}
			VkMemoryToImageCopy& region = regions.emplace_back();
			region.sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY;
			region.pHostPointer = texels;
			// In texels: row pitch in blocks times the block width, and rows per slice times the block height.
			region.memoryRowLength = static_cast<uint32_t>(rowPitch / format.bytesPerBlock * format.blockSize);
			region.memoryImageHeight = static_cast<uint32_t>(depthPitch / rowPitch * format.blockSize);
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.mipLevel = subresource.MipLevel;
			region.imageSubresource.baseArrayLayer = subresource.ArraySlice;
			region.imageSubresource.layerCount = 1;
			region.imageOffset = { 0, 0, 0 };
			region.imageExtent = { layout.Width, layout.Height, layout.Depth };
		}
		VkCopyMemoryToImageInfo copyInfo{};
		copyInfo.sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO;
		copyInfo.dstImage = image;
		copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		copyInfo.regionCount = static_cast<uint32_t>(regions.size());
		copyInfo.pRegions = regions.data();
		const VkResult copied = dispatcher.vkCopyMemoryToImage(device, &copyInfo);
		if (copied != VK_SUCCESS)
			return fail("vkCopyMemoryToImage", copied);

		Result<nvrhi::TextureHandle> texture = m_Device->CreateHandleForNativeTexture(image, desc);
		if (!texture.has_value())
		{
			FreeMemory(*memory);
			dispatcher.vkDestroyImage(device, image, nullptr);
			return std::unexpected(std::move(texture).error());
		}
		m_Device->GetResourceTracker().RecordCreated(GpuResourceType::HostImage);
		m_Images.push_back(HostImage{ .Texture = *texture, .Image = image, .Memory = *memory });

		// NVRHI assumes an untracked texture with keepInitialState starts undefined until a submitted command list has
		// used it. This list declares the layout the host transition left, and its submission makes NVRHI start every
		// later list from ShaderResource. Queue submission also makes the host writes visible to the device.
		commandList->open();
		commandList->beginTrackingTextureState(*texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		commandList->close();
		m_Device->ExecuteCommandList(*commandList);
		return TextureUpload{ .Texture = std::move(*texture), .Path = TextureUploadPath::HostImageCopy };
	}

	void HostImageUpload::CollectGarbage()
	{
		// Images only this upload still references: their wrapper goes now, the VkImage after the last submission so far.
		const GpuResourceTracker& tracker = m_Device->GetResourceTracker();
		for (auto image = m_Images.begin(); image != m_Images.end();)
		{
			if (tracker.HasOtherReferences(*image->Texture, 1))
			{
				++image;
				continue;
			}
			m_PendingReleases.push_back(
				PendingRelease{ .Image = image->Image, .Memory = image->Memory, .SubmissionID = m_Device->GetLastSubmissionID() });
			image = m_Images.erase(image);
		}

		if (m_PendingReleases.empty())
			return;
		const uint64_t completed = m_Device->GetCompletedSubmissionID();
		for (auto release = m_PendingReleases.begin(); release != m_PendingReleases.end();)
		{
			if (release->SubmissionID > completed)
			{
				++release;
				continue;
			}
			DestroyImage(release->Image, release->Memory, false);
			release = m_PendingReleases.erase(release);
		}
	}

	size_t HostImageUpload::GetHostImageCount() const
	{
		return m_Images.size() + m_PendingReleases.size();
	}

	size_t HostImageUpload::GetPendingReleaseCount() const
	{
		return m_PendingReleases.size();
	}

	Result<HostImageUpload::MemoryAllocation> HostImageUpload::AllocateMemory(const VkMemoryRequirements& requirements, bool dedicated, VkImage image)
	{
		const std::optional<uint32_t> memoryType = Utils::FindMemoryType(m_MemoryProperties, requirements.memoryTypeBits);
		if (!memoryType.has_value())
			return MakeError(ErrorCode::Gpu, "no memory type can hold the image (memory type bits 0x{:x})", requirements.memoryTypeBits);
		const vk::detail::DispatchLoaderDynamic& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
		const VkDevice device = m_Device->GetVulkanDevice();

		if (dedicated || requirements.size > Utils::MaxSuballocatedImageSize)
		{
			VkMemoryDedicatedAllocateInfo dedicatedInfo{};
			dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
			dedicatedInfo.image = image;
			VkMemoryAllocateInfo allocateInfo{};
			allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			allocateInfo.pNext = &dedicatedInfo;
			allocateInfo.allocationSize = requirements.size;
			allocateInfo.memoryTypeIndex = *memoryType;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			const VkResult result = dispatcher.vkAllocateMemory(device, &allocateInfo, nullptr, &memory);
			if (result != VK_SUCCESS)
				return MakeError(ErrorCode::Gpu, "vkAllocateMemory of {} bytes failed: {}", requirements.size, VkResultToString(result));
			Scope<MemoryBlock>& block = m_MemoryBlocks.emplace_back(CreateScope<MemoryBlock>());
			block->Memory = memory;
			block->Size = requirements.size;
			block->MemoryTypeIndex = *memoryType;
			block->IsDedicated = true;
			block->AllocationCount = 1;
			return MemoryAllocation{ .Block = block.get(), .Range = { .Offset = 0, .Size = requirements.size } };
		}

		// First fit in an existing block of the memory type, else in a new block.
		const auto tryAllocate = [&requirements](MemoryBlock& block) -> std::optional<MemoryRange>
		{
			for (size_t index = 0; index < block.FreeRanges.size(); ++index)
			{
				const MemoryRange available = block.FreeRanges[index];
				const VkDeviceSize offset = Utils::AlignUp(available.Offset, requirements.alignment);
				if (offset + requirements.size > available.Offset + available.Size)
					continue;
				// The alignment padding before and the remainder after stay free.
				const MemoryRange before{ .Offset = available.Offset, .Size = offset - available.Offset };
				const MemoryRange after{ .Offset = offset + requirements.size, .Size = available.Offset + available.Size - offset - requirements.size };
				block.FreeRanges.erase(block.FreeRanges.begin() + static_cast<std::ptrdiff_t>(index));
				auto position = block.FreeRanges.begin() + static_cast<std::ptrdiff_t>(index);
				if (after.Size > 0)
					position = block.FreeRanges.insert(position, after);
				if (before.Size > 0)
					block.FreeRanges.insert(position, before);
				++block.AllocationCount;
				return MemoryRange{ .Offset = offset, .Size = requirements.size };
			}
			return std::nullopt;
		};
		for (const Scope<MemoryBlock>& block : m_MemoryBlocks)
		{
			if (block->IsDedicated || block->MemoryTypeIndex != *memoryType)
				continue;
			if (const std::optional<MemoryRange> range = tryAllocate(*block))
				return MemoryAllocation{ .Block = block.get(), .Range = *range };
		}

		VkMemoryAllocateInfo allocateInfo{};
		allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocateInfo.allocationSize = Utils::HostImageBlockSize;
		allocateInfo.memoryTypeIndex = *memoryType;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		const VkResult result = dispatcher.vkAllocateMemory(device, &allocateInfo, nullptr, &memory);
		if (result != VK_SUCCESS)
		{
			return MakeError(ErrorCode::Gpu, "vkAllocateMemory of a {} byte block failed: {}", Utils::HostImageBlockSize,
				VkResultToString(result));
		}
		Scope<MemoryBlock>& block = m_MemoryBlocks.emplace_back(CreateScope<MemoryBlock>());
		block->Memory = memory;
		block->Size = Utils::HostImageBlockSize;
		block->MemoryTypeIndex = *memoryType;
		block->FreeRanges.push_back({ .Offset = 0, .Size = Utils::HostImageBlockSize });
		const std::optional<MemoryRange> range = tryAllocate(*block);
		ENGINE_CORE_ASSERT(range.has_value(), "an image of at most a quarter block fits an empty block");
		if (!range.has_value())
			return MakeError(ErrorCode::Gpu, "an image of {} bytes does not fit a new memory block", requirements.size);
		return MemoryAllocation{ .Block = block.get(), .Range = *range };
	}

	void HostImageUpload::FreeMemory(const MemoryAllocation& allocation)
	{
		MemoryBlock* block = allocation.Block;
		ENGINE_CORE_ASSERT(block != nullptr && block->AllocationCount > 0, "HostImageUpload::FreeMemory of an unknown allocation");
		if (block == nullptr)
			return;
		--block->AllocationCount;
		if (!block->IsDedicated)
		{
			// Back into the sorted free list, merged with its neighbours.
			std::vector<MemoryRange>& ranges = block->FreeRanges;
			const auto next = std::ranges::lower_bound(ranges, allocation.Range.Offset, {}, &MemoryRange::Offset);
			auto inserted = ranges.insert(next, allocation.Range);
			if (inserted + 1 != ranges.end() && inserted->Offset + inserted->Size == (inserted + 1)->Offset)
			{
				inserted->Size += (inserted + 1)->Size;
				ranges.erase(inserted + 1);
			}
			if (inserted != ranges.begin() && (inserted - 1)->Offset + (inserted - 1)->Size == inserted->Offset)
			{
				(inserted - 1)->Size += inserted->Size;
				ranges.erase(inserted);
			}
		}
		if (block->AllocationCount > 0)
			return;

		// An empty block returns its memory.
		VULKAN_HPP_DEFAULT_DISPATCHER.vkFreeMemory(m_Device->GetVulkanDevice(), block->Memory, nullptr);
		const auto owner = std::ranges::find_if(m_MemoryBlocks, [block](const Scope<MemoryBlock>& candidate)
		{
			return candidate.get() == block;
		});
		ENGINE_CORE_ASSERT(owner != m_MemoryBlocks.end(), "HostImageUpload::FreeMemory of a block it does not own");
		if (owner != m_MemoryBlocks.end())
			m_MemoryBlocks.erase(owner);
	}

	void HostImageUpload::DestroyImage(VkImage image, const MemoryAllocation& memory, bool leaked)
	{
		VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyImage(m_Device->GetVulkanDevice(), image, nullptr);
		FreeMemory(memory);
		if (!leaked)
			m_Device->GetResourceTracker().RecordDestroyed(GpuResourceType::HostImage);
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
