#include "TestsPCH.h"

#include "Engine/Graphics/DeviceSelection.h"

// Device selection is pure (Architecture §8.1), so it is tested with plain DeviceCandidate tables, without a GPU.

namespace Engine {

	// A candidate that meets every requirement and has no optional feature.
	static DeviceCandidate MakeAcceptableCandidate(uint32_t index, std::string name, GpuDeviceType type)
	{
		DeviceCandidate candidate;
		candidate.Index = index;
		candidate.Name = std::move(name);
		candidate.Type = type;
		candidate.ApiVersion = (1u << 22) | (3u << 12); // VK_MAKE_API_VERSION(0, 1, 3, 0)
		candidate.VendorID = 0x10DE;
		candidate.DriverID = VK_DRIVER_ID_NVIDIA_PROPRIETARY;
		candidate.DynamicRendering = true;
		candidate.Synchronization2 = true;
		candidate.TimelineSemaphore = true;
		candidate.SamplerAnisotropy = true;
		candidate.ImageCubeArray = true;
		candidate.ShaderStorageImageExtendedFormats = true;
		candidate.StorageFormatSupported.fill(true);
		candidate.HasGraphicsQueue = true;
		candidate.HasPresentSupport = true;
		return candidate;
	}

	// `candidate` with one boolean feature switched off or on.
	static DeviceCandidate WithoutFeature(DeviceCandidate candidate, bool DeviceCandidate::* feature)
	{
		candidate.*feature = false;
		return candidate;
	}

	static DeviceCandidate WithFeature(DeviceCandidate candidate, bool DeviceCandidate::* feature)
	{
		candidate.*feature = true;
		return candidate;
	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("DeviceSelection: scoring table" * doctest::skip(true))
		{
			struct Row
			{
				std::string Description{};
				DeviceCandidate Candidate{};
				DeviceRejection ExpectedRejection = DeviceRejection::None;
			};

			const DeviceCandidate base = MakeAcceptableCandidate(0, "Base", GpuDeviceType::Discrete);
			DeviceCandidate api12 = base;
			api12.ApiVersion = (1u << 22) | (2u << 12);
			DeviceCandidate noR16Storage = base;
			noR16Storage.StorageFormatSupported[0] = false;
			DeviceCandidate noR32Storage = base;
			noR32Storage.StorageFormatSupported[5] = false;
			DeviceCandidate twoProblems = WithoutFeature(base, &DeviceCandidate::TimelineSemaphore);
			twoProblems.HasGraphicsQueue = false;

			const std::vector<Row> rows = {
				{ "acceptable", base, DeviceRejection::None },
				{ "API 1.2", api12, DeviceRejection::ApiVersionBelow13 },
				{ "no dynamicRendering", WithoutFeature(base, &DeviceCandidate::DynamicRendering), DeviceRejection::MissingDynamicRendering },
				{ "no synchronization2", WithoutFeature(base, &DeviceCandidate::Synchronization2), DeviceRejection::MissingSynchronization2 },
				{ "no timelineSemaphore", WithoutFeature(base, &DeviceCandidate::TimelineSemaphore), DeviceRejection::MissingTimelineSemaphore },
				{ "no samplerAnisotropy", WithoutFeature(base, &DeviceCandidate::SamplerAnisotropy), DeviceRejection::MissingSamplerAnisotropy },
				{ "no imageCubeArray", WithoutFeature(base, &DeviceCandidate::ImageCubeArray), DeviceRejection::MissingImageCubeArray },
				{ "no shaderStorageImageExtendedFormats", WithoutFeature(base, &DeviceCandidate::ShaderStorageImageExtendedFormats),
					DeviceRejection::MissingShaderStorageImageExtendedFormats },
				{ "R16_FLOAT is not a storage format", noR16Storage, DeviceRejection::MissingStorageFormat },
				{ "R32_UINT is not a storage format", noR32Storage, DeviceRejection::MissingStorageFormat },
				{ "no graphics queue", WithoutFeature(base, &DeviceCandidate::HasGraphicsQueue), DeviceRejection::NoGraphicsQueue },
				{ "two problems report the first", twoProblems, DeviceRejection::MissingTimelineSemaphore },
			};
			for (const Row& row : rows)
			{
				CAPTURE(row.Description);
				CHECK(GetDeviceRejection(row.Candidate, false) == row.ExpectedRejection);
				CHECK(ScoreDevice(row.Candidate).has_value() == (row.ExpectedRejection == DeviceRejection::None));
			}

			// Present support is required only for windowed processes.
			const DeviceCandidate headlessOnly = WithoutFeature(base, &DeviceCandidate::HasPresentSupport);
			CHECK(GetDeviceRejection(headlessOnly, false) == DeviceRejection::None);
			CHECK(GetDeviceRejection(headlessOnly, true) == DeviceRejection::NoPresentSupport);

			// Missing B10G11R11 storage is accepted: Bloom falls back to RGBA16_FLOAT.
			CHECK(GetBloomFormat(base) == nvrhi::Format::RGBA16_FLOAT);
			const DeviceCandidate bloom = WithFeature(base, &DeviceCandidate::B10G11R11StorageSupported);
			CHECK(GetBloomFormat(bloom) == nvrhi::Format::R11G11B10_FLOAT);

			// Ranking: discrete > integrated > CPU whatever the features; within a type, more optional features win, in the
			// documented weight order.
			DeviceCandidate integrated = MakeAcceptableCandidate(1, "Integrated", GpuDeviceType::Integrated);
			integrated.HostImageCopy = true;
			integrated.B10G11R11StorageSupported = true;
			integrated.DepthClamp = true;
			integrated.DeviceFault = true;
			integrated.FillModeNonSolid = true;
			const DeviceCandidate cpu = MakeAcceptableCandidate(2, "Cpu", GpuDeviceType::Cpu);
			const std::optional<int32_t> discreteScore = ScoreDevice(base);
			const std::optional<int32_t> integratedScore = ScoreDevice(integrated);
			const std::optional<int32_t> cpuScore = ScoreDevice(cpu);
			REQUIRE(discreteScore.has_value());
			REQUIRE(integratedScore.has_value());
			REQUIRE(cpuScore.has_value());
			CHECK(*discreteScore > *integratedScore);
			CHECK(*integratedScore > *cpuScore);

			const std::optional<int32_t> hostCopyScore = ScoreDevice(WithFeature(base, &DeviceCandidate::HostImageCopy));
			const std::optional<int32_t> storageScore = ScoreDevice(WithFeature(base, &DeviceCandidate::B10G11R11StorageSupported));
			const std::optional<int32_t> depthClampScore = ScoreDevice(WithFeature(base, &DeviceCandidate::DepthClamp));
			const std::optional<int32_t> deviceFaultScore = ScoreDevice(WithFeature(base, &DeviceCandidate::DeviceFault));
			const std::optional<int32_t> wireframeScore = ScoreDevice(WithFeature(base, &DeviceCandidate::FillModeNonSolid));
			REQUIRE(hostCopyScore.has_value());
			REQUIRE(storageScore.has_value());
			REQUIRE(depthClampScore.has_value());
			REQUIRE(deviceFaultScore.has_value());
			REQUIRE(wireframeScore.has_value());
			CHECK(*hostCopyScore > *storageScore);
			CHECK(*storageScore > *depthClampScore);
			CHECK(*depthClampScore > *deviceFaultScore);
			CHECK(*deviceFaultScore > *wireframeScore);
			CHECK(*wireframeScore > *discreteScore);

			// A higher API version breaks a remaining tie.
			DeviceCandidate api14 = base;
			api14.ApiVersion = (1u << 22) | (4u << 12);
			const std::optional<int32_t> api14Score = ScoreDevice(api14);
			REQUIRE(api14Score.has_value());
			CHECK(*api14Score > *discreteScore);
		}

		TEST_CASE("DeviceSelection: the best acceptable device wins, the lowest index on a tie" * doctest::skip(true))
		{
			const std::vector<DeviceCandidate> candidates = {
				MakeAcceptableCandidate(0, "Integrated GPU", GpuDeviceType::Integrated),
				MakeAcceptableCandidate(1, "Discrete GPU A", GpuDeviceType::Discrete),
				MakeAcceptableCandidate(2, "Discrete GPU B", GpuDeviceType::Discrete),
			};
			const Result<DeviceSelection> selection = SelectDevice(candidates, {});
			REQUIRE_MESSAGE(selection.has_value(), selection.error().ToString());
			CHECK(selection->Index == 1);
			CHECK_FALSE(selection->ChosenByOverride);
			CHECK(selection->Score == *ScoreDevice(candidates[1]));

			// No acceptable candidate: Unsupported, listing every candidate with its rejection.
			std::vector<DeviceCandidate> rejected = candidates;
			for (DeviceCandidate& candidate : rejected)
				candidate.TimelineSemaphore = false;
			const Result<DeviceSelection> none = SelectDevice(rejected, {});
			REQUIRE_FALSE(none.has_value());
			CHECK(none.error().GetCode() == ErrorCode::Unsupported);
			CHECK(none.error().ToString().contains("Discrete GPU B"));
			CHECK(none.error().ToString().contains("MissingTimelineSemaphore"));
		}

		TEST_CASE("DeviceSelection: --gpu and ENGINE_GPU pick by index or by a case-insensitive name part" * doctest::skip(true))
		{
			std::vector<DeviceCandidate> candidates = {
				MakeAcceptableCandidate(0, "NVIDIA GeForce RTX 5070 Ti Laptop GPU", GpuDeviceType::Discrete),
				MakeAcceptableCandidate(1, "Intel(R) Graphics", GpuDeviceType::Integrated),
				MakeAcceptableCandidate(2, "llvmpipe (LLVM 19)", GpuDeviceType::Cpu),
			};

			const Result<DeviceSelection> byIndex = SelectDevice(candidates, { .Override = "2" });
			REQUIRE_MESSAGE(byIndex.has_value(), byIndex.error().ToString());
			CHECK(byIndex->Index == 2);
			CHECK(byIndex->ChosenByOverride);

			const Result<DeviceSelection> byName = SelectDevice(candidates, { .Override = "intel" });
			REQUIRE_MESSAGE(byName.has_value(), byName.error().ToString());
			CHECK(byName->Index == 1);

			const Result<DeviceSelection> unknown = SelectDevice(candidates, { .Override = "Radeon" });
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::NotFound);

			// An overridden device must still be acceptable.
			candidates[2].DynamicRendering = false;
			const Result<DeviceSelection> rejected = SelectDevice(candidates, { .Override = "llvmpipe" });
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::Unsupported);
			CHECK(rejected.error().ToString().contains("MissingDynamicRendering"));

			// The present requirement applies to the override too.
			const Result<DeviceSelection> windowed = SelectDevice(candidates, { .RequirePresent = true, .Override = "0" });
			CHECK(windowed.has_value());
		}

		TEST_CASE("DeviceSelection: the device class names the vendor and the driver major" * doctest::skip(true))
		{
			// The encoding follows the driver that reported the version, never the OS the test runs on, so every case
			// gives the same class on Windows, Linux and macOS.
			// NVIDIA's proprietary driver packs major.minor into bits 31-22 and 21-14: 581.57.
			const uint32_t nvidia58157 = (581u << 22) | (57u << 14);
			CHECK(GetDeviceClass(0x10DE, nvidia58157, VK_DRIVER_ID_NVIDIA_PROPRIETARY) == "nvidia-58x");
			CHECK(GetDeviceClass(0x10DE, (566u << 22) | (14u << 14), VK_DRIVER_ID_NVIDIA_PROPRIETARY) == "nvidia-56x");
			// AMD uses VK_MAKE_API_VERSION (2.0.300): a one-digit major is kept as it is.
			CHECK(GetDeviceClass(0x1002, VK_MAKE_API_VERSION(0, 2, 0, 300), VK_DRIVER_ID_AMD_PROPRIETARY) == "amd-2");
			// Intel's Windows driver packs bits 31-14: 101.6979.
			CHECK(GetDeviceClass(0x8086, (101u << 14) | 6979u, VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS) == "intel-10x");
			// Mesa's drivers, on Intel or NVIDIA hardware, use VK_MAKE_API_VERSION: Mesa 24.2.8 and 25.0.1.
			CHECK(GetDeviceClass(0x8086, VK_MAKE_API_VERSION(0, 24, 2, 8), VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA) == "intel-2x");
			CHECK(GetDeviceClass(0x10DE, VK_MAKE_API_VERSION(0, 25, 0, 1), VK_DRIVER_ID_MESA_NVK) == "nvidia-2x");
			// MoltenVK, an unknown vendor, and an unknown driver ID (0) use VK_API_VERSION_MAJOR too.
			CHECK(GetDeviceClass(0x106B, VK_MAKE_API_VERSION(0, 1, 2, 11), VK_DRIVER_ID_MOLTENVK) == "apple-1");
			CHECK(GetDeviceClass(0x1234, VK_MAKE_API_VERSION(0, 1, 0, 0), VkDriverId{}) == "vendor1234-1");
		}

		TEST_CASE("DeviceSelection: the rejection and type names are their enumerator names" * doctest::skip(true))
		{
			CHECK(DeviceRejectionToString(DeviceRejection::None) == "None");
			CHECK(DeviceRejectionToString(DeviceRejection::MissingStorageFormat) == "MissingStorageFormat");
			CHECK(DeviceRejectionToString(DeviceRejection::NoPresentSupport) == "NoPresentSupport");
			CHECK(GpuDeviceTypeToString(GpuDeviceType::Discrete) == "Discrete");
			CHECK(GpuDeviceTypeToString(GpuDeviceType::Cpu) == "Cpu");
		}
	}

}
