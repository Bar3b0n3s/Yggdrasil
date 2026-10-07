#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <nvrhi/nvrhi.h>
#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Physical-device selection (Architecture §8.1): pure functions over plain structs, unit-tested without a GPU
// ("DeviceSelection: scoring table"). GraphicsDevice::Create fills one DeviceCandidate per physical device from
// vkGetPhysicalDeviceProperties2/Features2/FormatProperties and queue-family queries, then calls SelectDevice.

namespace Engine {

	enum class GpuDeviceType : uint8_t
	{
		Other,
		Discrete,
		Integrated,
		Virtual,
		Cpu
	};

	// The storage-image formats the passes write (§8.1, §8.4 [vk::image_format]): each needs
	// VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, or the device is rejected. Bloom's B10G11R11_UFLOAT is optional (BloomFormat).
	inline constexpr std::array<nvrhi::Format, 6> RequiredStorageFormats = {
		nvrhi::Format::R16_FLOAT,
		nvrhi::Format::R8_UNORM,
		nvrhi::Format::RG16_FLOAT,
		nvrhi::Format::RGBA16_FLOAT,
		nvrhi::Format::RGBA8_UNORM,
		nvrhi::Format::R32_UINT,
	};

	// What selection needs to know about one physical device, as plain data.
	struct DeviceCandidate
	{
		uint32_t Index = 0; // position in vkEnumeratePhysicalDevices order (--gpu=<index>)
		std::string Name{}; // VkPhysicalDeviceProperties::deviceName
		GpuDeviceType Type = GpuDeviceType::Other;
		uint32_t ApiVersion = 0; // VkPhysicalDeviceProperties::apiVersion (VK_MAKE_API_VERSION encoding)
		uint32_t VendorID = 0;
		uint32_t DeviceID = 0;
		uint32_t DriverVersion = 0; // VkPhysicalDeviceProperties::driverVersion, in the driver's own encoding (GetDeviceClass)
		VkDriverId DriverID{};      // VkPhysicalDeviceDriverProperties::driverID (core in Vulkan 1.2); 0 when unknown

		// Required features (§8.1); a device without any of them is rejected.
		bool DynamicRendering = false;
		bool Synchronization2 = false;
		bool TimelineSemaphore = false;
		bool SamplerAnisotropy = false;
		bool ImageCubeArray = false;
		bool ShaderStorageImageExtendedFormats = false;
		// VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT per entry of RequiredStorageFormats, in the same order.
		std::array<bool, RequiredStorageFormats.size()> StorageFormatSupported{};
		// A queue family with graphics (and compute) support.
		bool HasGraphicsQueue = false;
		// That family can present to the window's surface (glfwGetPhysicalDevicePresentationSupport). Only required when
		// the selection is for a windowed process.
		bool HasPresentSupport = false;

		// Optional features (§8.1): each enables a path, its absence a documented fallback.
		bool B10G11R11StorageSupported = false; // Bloom storage format, else RGBA16_FLOAT
		bool DepthClamp = false;                // shadow pancaking, else the light near plane is extended
		bool FillModeNonSolid = false;          // the wireframe debug view
		bool HostImageCopy = false;             // VkPhysicalDeviceVulkan14Features::hostImageCopy (needs ApiVersion >= 1.4)
		bool DeviceFault = false;               // VK_EXT_device_fault with the deviceFault feature (crash reports)
		bool PortabilitySubset = false;         // VK_KHR_portability_subset is exposed and must be enabled (MoltenVK)
	};

	// Why a candidate was rejected; None for an accepted one.
	enum class DeviceRejection : uint8_t
	{
		None,
		ApiVersionBelow13,
		MissingDynamicRendering,
		MissingSynchronization2,
		MissingTimelineSemaphore,
		MissingSamplerAnisotropy,
		MissingImageCubeArray,
		MissingShaderStorageImageExtendedFormats,
		MissingStorageFormat, // one of RequiredStorageFormats lacks VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT
		NoGraphicsQueue,
		NoPresentSupport
	};

	struct DeviceSelectionRequest
	{
		// The process presents to a window: HasPresentSupport becomes required.
		bool RequirePresent = false;
		// GraphicsSpecification::GpuOverride, or the ENGINE_GPU value when that is empty; empty: no override.
		std::string Override{};
	};

	struct DeviceSelection
	{
		uint32_t Index = 0;            // DeviceCandidate::Index of the chosen device
		int32_t Score = 0;             // ScoreDevice of the chosen device
		bool ChosenByOverride = false; // the override picked it
		nvrhi::Format BloomFormat = nvrhi::Format::R11G11B10_FLOAT;
	};

	// The first requirement `candidate` misses, checked in DeviceRejection order; None when it meets all of them.
	// `requirePresent` adds NoPresentSupport.
	[[nodiscard]] DeviceRejection GetDeviceRejection(const DeviceCandidate& candidate, bool requirePresent);

	// The score of an acceptable candidate; nullopt when GetDeviceRejection(candidate, false) is not None. Ranking (§8.1):
	// every discrete device scores above every integrated one, which scores above every virtual, other and CPU device;
	// within a type, more optional features score higher (HostImageCopy, B10G11R11 storage, DepthClamp, DeviceFault,
	// FillModeNonSolid, in that weight order), and a higher ApiVersion breaks a remaining tie. Pure and deterministic.
	[[nodiscard]] std::optional<int32_t> ScoreDevice(const DeviceCandidate& candidate);

	// Picks the device to use. With an override: a decimal index selects the candidate with that Index, anything else
	// the first candidate (in Index order) whose Name contains it, compared ASCII case-insensitively; an overridden device
	// must still be acceptable. Without one: the acceptable candidate with the highest score, the lowest Index on a tie.
	// Errors: Unsupported when no candidate is acceptable, its message listing each candidate with its DeviceRejection;
	// NotFound when the override matches no candidate (listing the names); Unsupported when the override matches a
	// rejected device (naming the rejection).
	[[nodiscard]] Result<DeviceSelection> SelectDevice(std::span<const DeviceCandidate> candidates, const DeviceSelectionRequest& request);

	// The storage format Bloom uses on `candidate` (§8.1, §8.3 pass 10): R11G11B10_FLOAT when B10G11R11StorageSupported,
	// otherwise RGBA16_FLOAT (a pipeline permutation; GraphicsDevice logs the fallback once).
	[[nodiscard]] nvrhi::Format GetBloomFormat(const DeviceCandidate& candidate);

	// The golden-image device class (§15.4): "<vendor>-<major>", where vendor is "nvidia" (0x10DE), "amd" (0x1002),
	// "intel" (0x8086), "apple" (0x106B) or "vendor<lower-case hex id>", and major is the driver major version decoded with
	// the encoding of the driver that reports it, chosen by `driverID` (VkPhysicalDeviceDriverProperties::driverID) rather
	// than by the operating system the function runs on: VK_DRIVER_ID_NVIDIA_PROPRIETARY bits 31-22,
	// VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS bits 31-14, every other driver (Mesa's, AMD's, MoltenVK, an unknown 0)
	// VK_API_VERSION_MAJOR. The major's last digit is replaced by 'x' when it has two or more digits, so the minor driver
	// updates of one series share their goldens. Examples: NVIDIA 581.57 gives "nvidia-58x", Intel's Windows driver
	// 101.6979 "intel-10x", Mesa 24.2.8 on Intel "intel-2x", AMD 2.0.300 "amd-2". Pure and the same on every platform.
	[[nodiscard]] std::string GetDeviceClass(uint32_t vendorID, uint32_t driverVersion, VkDriverId driverID);

	// "None", "ApiVersionBelow13", ...
	[[nodiscard]] std::string_view DeviceRejectionToString(DeviceRejection rejection);

	// "Other", "Discrete", "Integrated", "Virtual" or "Cpu".
	[[nodiscard]] std::string_view GpuDeviceTypeToString(GpuDeviceType type);

}
