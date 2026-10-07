#include "EnginePCH.h"
#include "Engine/Graphics/DeviceSelection.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <vector>

namespace Engine {

	namespace Utils {

		// Score layout (ScoreDevice): the type tier dominates, then the optional features, then the API version. Each
		// level's largest contribution stays below one unit of the level above it.
		constexpr int32_t DeviceTypeWeight = 100'000'000;
		constexpr int32_t DeviceFeatureWeight = 1'000'000;
		// Within the API version: major (clamped to 9) * 100'000 + minor (clamped to 99) * 1'000 + patch (clamped to 999).
		constexpr uint32_t MaxScoredApiMajor = 9;
		constexpr uint32_t MaxScoredApiMinor = 99;
		constexpr uint32_t MaxScoredApiPatch = 999;

		static int32_t GetDeviceTypeTier(GpuDeviceType type)
		{
			switch (type)
			{
				case GpuDeviceType::Discrete:   return 4;
				case GpuDeviceType::Integrated: return 3;
				case GpuDeviceType::Virtual:    return 2;
				case GpuDeviceType::Other:      return 1;
				case GpuDeviceType::Cpu:        return 0;
			}

			ENGINE_CORE_ASSERT(false, "Unknown GpuDeviceType {}", std::to_underlying(type));
			return 0;
		}

		// The optional features as bits whose weights halve in the documented order, so a feature outranks every
		// combination of the features after it.
		static int32_t GetOptionalFeatureBits(const DeviceCandidate& candidate)
		{
			int32_t bits = 0;
			if (candidate.HostImageCopy)
				bits += 16;
			if (candidate.B10G11R11StorageSupported)
				bits += 8;
			if (candidate.DepthClamp)
				bits += 4;
			if (candidate.DeviceFault)
				bits += 2;
			if (candidate.FillModeNonSolid)
				bits += 1;
			return bits;
		}

		static int32_t GetApiVersionTieBreak(uint32_t apiVersion)
		{
			const uint32_t major = std::min<uint32_t>(VK_API_VERSION_MAJOR(apiVersion), MaxScoredApiMajor);
			const uint32_t minor = std::min<uint32_t>(VK_API_VERSION_MINOR(apiVersion), MaxScoredApiMinor);
			const uint32_t patch = std::min<uint32_t>(VK_API_VERSION_PATCH(apiVersion), MaxScoredApiPatch);
			return static_cast<int32_t>(major * 100'000 + minor * 1'000 + patch);
		}

		static char ToLowerAscii(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		// Whether `text` contains `part`, compared ASCII case-insensitively (device names are ASCII in practice; other
		// bytes compare exactly).
		static bool ContainsIgnoringAsciiCase(std::string_view text, std::string_view part)
		{
			const auto found = std::ranges::search(text, part, [](char left, char right)
			{
				return ToLowerAscii(left) == ToLowerAscii(right);
			});
			return !found.empty() || part.empty();
		}

		// The decimal index an override names, or nullopt when it is not a plain decimal number.
		static std::optional<uint32_t> ParseDeviceIndex(std::string_view text)
		{
			if (text.empty() || !std::ranges::all_of(text, [](char character)
			{
				return character >= '0' && character <= '9';
			}))
				return std::nullopt;
			uint32_t index = 0;
			const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), index);
			if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
				return std::nullopt;
			return index;
		}

		// "'<Name>' (index <Index>)", how messages name a candidate.
		static std::string DescribeCandidate(const DeviceCandidate& candidate)
		{
			return std::format("'{}' (index {}, {})", candidate.Name, candidate.Index, GpuDeviceTypeToString(candidate.Type));
		}

		// Pointers to the candidates in Index order, so ties and name matches never depend on the caller's order.
		static std::vector<const DeviceCandidate*> SortByIndex(std::span<const DeviceCandidate> candidates)
		{
			std::vector<const DeviceCandidate*> sorted;
			sorted.reserve(candidates.size());
			for (const DeviceCandidate& candidate : candidates)
				sorted.push_back(&candidate);
			std::stable_sort(sorted.begin(), sorted.end(), [](const DeviceCandidate* left, const DeviceCandidate* right)
			{
				return left->Index < right->Index;
			});
			return sorted;
		}

		static std::string ListCandidateNames(const std::vector<const DeviceCandidate*>& candidates)
		{
			std::string names;
			for (const DeviceCandidate* candidate : candidates)
			{
				if (!names.empty())
					names += ", ";
				names += DescribeCandidate(*candidate);
			}
			return names.empty() ? std::string("none") : names;
		}

		static Result<DeviceSelection> SelectOverriddenDevice(const std::vector<const DeviceCandidate*>& candidates,
			const DeviceSelectionRequest& request)
		{
			const DeviceCandidate* match = nullptr;
			if (const std::optional<uint32_t> index = ParseDeviceIndex(request.Override))
			{
				const auto found = std::ranges::find_if(candidates, [&index](const DeviceCandidate* candidate)
				{
					return candidate->Index == *index;
				});
				if (found != candidates.end())
					match = *found;
			}
			else
			{
				const auto found = std::ranges::find_if(candidates, [&request](const DeviceCandidate* candidate)
				{
					return ContainsIgnoringAsciiCase(candidate->Name, request.Override);
				});
				if (found != candidates.end())
					match = *found;
			}

			if (match == nullptr)
			{
				return std::unexpected(Error(ErrorCode::NotFound, std::format("the GPU override '{}' matches no device", request.Override))
						.WithHint(std::format("use a device index or part of a device name; devices: {}", ListCandidateNames(candidates))));
			}

			const DeviceRejection rejection = GetDeviceRejection(*match, request.RequirePresent);
			if (rejection != DeviceRejection::None)
			{
				return MakeError(ErrorCode::Unsupported, "the GPU override '{}' selects {}, which is not usable: {}", request.Override,
					DescribeCandidate(*match), DeviceRejectionToString(rejection));
			}

			const std::optional<int32_t> score = ScoreDevice(*match);
			ENGINE_CORE_ASSERT(score.has_value(), "an acceptable device has a score");
			return DeviceSelection{
				.Index = match->Index,
				.Score = score.value_or(0),
				.ChosenByOverride = true,
				.BloomFormat = GetBloomFormat(*match),
			};
		}

	}

	DeviceRejection GetDeviceRejection(const DeviceCandidate& candidate, bool requirePresent)
	{
		const uint32_t major = VK_API_VERSION_MAJOR(candidate.ApiVersion);
		const uint32_t minor = VK_API_VERSION_MINOR(candidate.ApiVersion);
		if (major < 1 || (major == 1 && minor < 3))
			return DeviceRejection::ApiVersionBelow13;
		if (!candidate.DynamicRendering)
			return DeviceRejection::MissingDynamicRendering;
		if (!candidate.Synchronization2)
			return DeviceRejection::MissingSynchronization2;
		if (!candidate.TimelineSemaphore)
			return DeviceRejection::MissingTimelineSemaphore;
		if (!candidate.SamplerAnisotropy)
			return DeviceRejection::MissingSamplerAnisotropy;
		if (!candidate.ImageCubeArray)
			return DeviceRejection::MissingImageCubeArray;
		if (!candidate.ShaderStorageImageExtendedFormats)
			return DeviceRejection::MissingShaderStorageImageExtendedFormats;
		if (!std::ranges::all_of(candidate.StorageFormatSupported, [](bool supported)
		{
			return supported;
		}))
			return DeviceRejection::MissingStorageFormat;
		if (!candidate.HasGraphicsQueue)
			return DeviceRejection::NoGraphicsQueue;
		if (requirePresent && !candidate.HasPresentSupport)
			return DeviceRejection::NoPresentSupport;
		return DeviceRejection::None;
	}

	std::optional<int32_t> ScoreDevice(const DeviceCandidate& candidate)
	{
		if (GetDeviceRejection(candidate, false) != DeviceRejection::None)
			return std::nullopt;
		return Utils::GetDeviceTypeTier(candidate.Type) * Utils::DeviceTypeWeight
			+ Utils::GetOptionalFeatureBits(candidate) * Utils::DeviceFeatureWeight + Utils::GetApiVersionTieBreak(candidate.ApiVersion);
	}

	Result<DeviceSelection> SelectDevice(std::span<const DeviceCandidate> candidates, const DeviceSelectionRequest& request)
	{
		const std::vector<const DeviceCandidate*> sorted = Utils::SortByIndex(candidates);
		if (!request.Override.empty())
			return Utils::SelectOverriddenDevice(sorted, request);

		const DeviceCandidate* best = nullptr;
		int32_t bestScore = 0;
		std::string rejections;
		for (const DeviceCandidate* candidate : sorted)
		{
			const DeviceRejection rejection = GetDeviceRejection(*candidate, request.RequirePresent);
			if (rejection != DeviceRejection::None)
			{
				if (!rejections.empty())
					rejections += ", ";
				rejections += std::format("{}: {}", Utils::DescribeCandidate(*candidate), DeviceRejectionToString(rejection));
				continue;
			}
			const std::optional<int32_t> score = ScoreDevice(*candidate);
			ENGINE_CORE_ASSERT(score.has_value(), "an acceptable device has a score");
			// Strictly greater: on a tie the lower Index, seen first, stays.
			if (score.has_value() && (best == nullptr || *score > bestScore))
			{
				best = candidate;
				bestScore = *score;
			}
		}

		if (best == nullptr)
		{
			if (sorted.empty())
				return MakeError(ErrorCode::Unsupported, "no Vulkan device found");
			return MakeError(ErrorCode::Unsupported, "no Vulkan device meets the engine's requirements: {}", rejections);
		}
		return DeviceSelection{
			.Index = best->Index,
			.Score = bestScore,
			.ChosenByOverride = false,
			.BloomFormat = GetBloomFormat(*best),
		};
	}

	nvrhi::Format GetBloomFormat(const DeviceCandidate& candidate)
	{
		return candidate.B10G11R11StorageSupported ? nvrhi::Format::R11G11B10_FLOAT : nvrhi::Format::RGBA16_FLOAT;
	}

	std::string GetDeviceClass(uint32_t vendorID, uint32_t driverVersion, VkDriverId driverID)
	{
		std::string vendor;
		switch (vendorID)
		{
			case 0x10DE: vendor = "nvidia"; break;
			case 0x1002: vendor = "amd"; break;
			case 0x8086: vendor = "intel"; break;
			case 0x106B: vendor = "apple"; break;
			default:     vendor = std::format("vendor{:x}", vendorID); break;
		}

		// The driver's own version encoding, chosen by the driver that reported it (never by the host OS).
		uint32_t major = 0;
		if (driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY)
			major = (driverVersion >> 22) & 0x3FFu;
		else if (driverID == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS)
			major = driverVersion >> 14;
		else
			major = VK_API_VERSION_MAJOR(driverVersion);

		std::string majorText = std::to_string(major);
		if (majorText.size() >= 2)
			majorText.back() = 'x';
		return std::format("{}-{}", vendor, majorText);
	}

	std::string_view DeviceRejectionToString(DeviceRejection rejection)
	{
		switch (rejection)
		{
			case DeviceRejection::None:                                     return "None";
			case DeviceRejection::ApiVersionBelow13:                        return "ApiVersionBelow13";
			case DeviceRejection::MissingDynamicRendering:                  return "MissingDynamicRendering";
			case DeviceRejection::MissingSynchronization2:                  return "MissingSynchronization2";
			case DeviceRejection::MissingTimelineSemaphore:                 return "MissingTimelineSemaphore";
			case DeviceRejection::MissingSamplerAnisotropy:                 return "MissingSamplerAnisotropy";
			case DeviceRejection::MissingImageCubeArray:                    return "MissingImageCubeArray";
			case DeviceRejection::MissingShaderStorageImageExtendedFormats: return "MissingShaderStorageImageExtendedFormats";
			case DeviceRejection::MissingStorageFormat:                     return "MissingStorageFormat";
			case DeviceRejection::NoGraphicsQueue:                          return "NoGraphicsQueue";
			case DeviceRejection::NoPresentSupport:                         return "NoPresentSupport";
		}

		ENGINE_CORE_ASSERT(false, "Unknown DeviceRejection {}", std::to_underlying(rejection));
		return "Unknown";
	}

	std::string_view GpuDeviceTypeToString(GpuDeviceType type)
	{
		switch (type)
		{
			case GpuDeviceType::Other:      return "Other";
			case GpuDeviceType::Discrete:   return "Discrete";
			case GpuDeviceType::Integrated: return "Integrated";
			case GpuDeviceType::Virtual:    return "Virtual";
			case GpuDeviceType::Cpu:        return "Cpu";
		}

		ENGINE_CORE_ASSERT(false, "Unknown GpuDeviceType {}", std::to_underlying(type));
		return "Unknown";
	}

}
