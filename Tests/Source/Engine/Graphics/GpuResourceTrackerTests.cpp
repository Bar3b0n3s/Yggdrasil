#include "TestsPCH.h"

#include "Engine/Graphics/GpuResourceTracker.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Platform/Process.h"
#include "Support/DeathTest.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	// The lifetime log's environment variable: a tracker built without a directory reads it (GpuResourceTracker.h).
	ENGINE_DEATH_TEST("Graphics/LifetimeLogFromEnvironment")
	{
		GpuResourceTracker tracker;
		tracker.RecordCreated(GpuResourceType::HostImage);
		tracker.RecordDestroyed(GpuResourceType::HostImage);
	}

	namespace {

		// The lines of the lifetime log at `path`, each split at its spaces: sequence, time, tracker=..., the event and its
		// fields.
		std::vector<std::vector<std::string>> ReadLifetimeLog(const std::filesystem::path& path)
		{
			const Result<std::string> text = FileSystem::ReadText(path);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			std::vector<std::vector<std::string>> lines;
			std::istringstream stream(*text);
			std::string line;
			while (std::getline(stream, line))
			{
				std::vector<std::string> fields;
				std::istringstream words(line);
				std::string word;
				while (words >> word)
					fields.push_back(word);
				lines.push_back(std::move(fields));
			}
			return lines;
		}

		// The events of `lines`: each line from its fourth field on, after checking that the sequences count from 1 and that
		// every line has `trackerField`.
		std::vector<std::string> GetLifetimeEvents(const std::vector<std::vector<std::string>>& lines, const std::string& trackerField)
		{
			std::vector<std::string> events;
			for (size_t index = 0; index < lines.size(); ++index)
			{
				const std::vector<std::string>& fields = lines[index];
				REQUIRE(fields.size() >= 4);
				CHECK(fields[0] == std::to_string(index + 1));
				CHECK(fields[2] == trackerField);
				std::string event = fields[3];
				for (size_t field = 4; field < fields.size(); ++field)
					event += " " + fields[field];
				events.push_back(std::move(event));
			}
			return events;
		}

	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("GpuResourceTracker: live counts return to zero" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			GpuResourceTracker& tracker = device.GetResourceTracker();
			const uint64_t baseline = tracker.GetTotalLiveCount();
			{
				// A target (two textures and a framebuffer), a buffer, a sampler and a command list that a submission used.
				Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 64, .Height = 32, .Depth = true });
				REQUIRE_MESSAGE(target.has_value(), target.error().ToString());
				nvrhi::BufferDesc bufferDesc;
				bufferDesc.byteSize = 1024;
				bufferDesc.debugName = "LeakCheck";
				Result<nvrhi::BufferHandle> buffer = device.CreateBuffer(bufferDesc);
				REQUIRE_MESSAGE(buffer.has_value(), buffer.error().ToString());
				Result<nvrhi::SamplerHandle> sampler = device.CreateSampler(nvrhi::SamplerDesc());
				REQUIRE_MESSAGE(sampler.has_value(), sampler.error().ToString());
				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
				(*commandList)->open();
				target->Clear(**commandList);
				(*commandList)->close();
				device.ExecuteCommandList(**commandList);
				CHECK(tracker.GetLiveCount(GpuResourceType::Texture) >= 2);
				CHECK(tracker.GetLiveCount(GpuResourceType::GpuBuffer) >= 1);
				CHECK(tracker.GetTotalLiveCount() > baseline);
			}
			// Dropped handles are counted as destroyed at the next garbage collection: NVRHI retires the submission's
			// references first, and the sweep then releases the framebuffer and, in the same call, the textures it held.
			device.WaitForIdle();
			device.RunGarbageCollection();
			INFO("live: ", tracker.DescribeLiveCounts());
			CHECK(tracker.GetTotalLiveCount() == baseline);
			// The fixture's destruction then checks zero live objects with nothing left at all.
		}

		TEST_CASE("GpuResourceTracker: HasOtherReferences discounts the tracker's own reference"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			const GpuResourceTracker& tracker = device.GetResourceTracker();
			nvrhi::TextureDesc desc;
			desc.width = 16;
			desc.height = 16;
			desc.format = nvrhi::Format::RGBA8_UNORM;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			desc.debugName = "OwnershipCheck";
			Result<nvrhi::TextureHandle> texture = device.CreateTexture(desc);
			REQUIRE_MESSAGE(texture.has_value(), texture.error().ToString());

			// Only this handle (and the tracker, which does not count) references the texture.
			CHECK_FALSE(tracker.HasOtherReferences(**texture, 1));
			{
				// A second handle is another reference, unless the caller counts it as its own.
				const nvrhi::TextureHandle copy = *texture;
				CHECK(tracker.HasOtherReferences(**texture, 1));
				CHECK_FALSE(tracker.HasOtherReferences(**texture, 2));
			}
			CHECK_FALSE(tracker.HasOtherReferences(**texture, 1));

			// A submission that used the texture references it until NVRHI retires it. The upload is writeTexture, which
			// records the reference; NVRHI's clearTextureFloat records none.
			const std::vector<std::byte> texels(static_cast<size_t>(desc.width) * desc.height * 4);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			(*commandList)->open();
			(*commandList)->writeTexture(*texture, 0, 0, texels.data(), static_cast<size_t>(desc.width) * 4);
			(*commandList)->close();
			device.ExecuteCommandList(**commandList);
			CHECK(tracker.HasOtherReferences(**texture, 1));
			device.WaitForIdle();
			device.RunGarbageCollection();
			CHECK_FALSE(tracker.HasOtherReferences(**texture, 1));
		}

		TEST_CASE("GpuResourceTracker: recorded host images count as created and destroyed")
		{
			GpuResourceTracker tracker;
			tracker.RecordCreated(GpuResourceType::HostImage);
			tracker.RecordCreated(GpuResourceType::HostImage);
			tracker.RecordDestroyed(GpuResourceType::HostImage);
			const GpuResourceCounts counts = tracker.GetCounts(GpuResourceType::HostImage);
			CHECK(counts.Created == 2);
			CHECK(counts.Destroyed == 1);
			CHECK(counts.GetLive() == 1);
			CHECK(tracker.GetLiveCount(GpuResourceType::HostImage) == 1);
			CHECK(tracker.GetTotalLiveCount() == 1);
			CHECK(tracker.DescribeLiveCounts() == "HostImage: 1");
			tracker.RecordDestroyed(GpuResourceType::HostImage);
			CHECK(tracker.GetTotalLiveCount() == 0);
			CHECK(tracker.DescribeLiveCounts() == "none");
		}

		TEST_CASE("GpuResourceTracker: the lifetime log brackets each tracker's events in the process's file")
		{
			// R-GPUCRASH (M18): one file per process, appended to by every tracker, one flushed line per event.
			Test::TempDirectory directory("GpuLifetimeLog");
			std::filesystem::path path;
			std::string trackerField;
			{
				GpuResourceTracker tracker(directory.GetPath() / "Logs");
				path = tracker.GetLifetimeLogPath();
				REQUIRE_FALSE(path.empty());
				CHECK(path == directory.GetPath() / "Logs" / std::format("GpuLifetime-{}.log", Process::GetCurrentId()));
				trackerField = std::format("tracker={}", static_cast<const void*>(&tracker));
				tracker.RecordCreated(GpuResourceType::HostImage);
				// Written through at once: the line is in the file while the tracker lives.
				CHECK(ReadLifetimeLog(path).size() == 2);
				tracker.RecordDestroyed(GpuResourceType::HostImage);
			}
			const std::vector<std::vector<std::string>> lines = ReadLifetimeLog(path);
			CHECK(GetLifetimeEvents(lines, trackerField)
				== std::vector<std::string>{ std::format("open pid={}", Process::GetCurrentId()), "host-create HostImage", "host-destroy HostImage", "close" });
			// A second tracker appends to the same file; one without a directory writes nothing.
			{
				const GpuResourceTracker second(directory.GetPath() / "Logs");
				CHECK(second.GetLifetimeLogPath() == path);
				const GpuResourceTracker off{ std::filesystem::path() };
				CHECK(off.GetLifetimeLogPath().empty());
			}
			CHECK(ReadLifetimeLog(path).size() == lines.size() + 2);
		}

		TEST_CASE("GpuResourceTracker: a lifetime log it cannot write warns once and leaves the log off, and the tracker counts")
		{
			// GpuResourceTracker.h: a directory that cannot be created, or a log file that cannot be opened, logs one warning
			// and leaves the log off; the tracker counts as before either way.
			Test::TempDirectory directory("GpuLifetimeLogUnusable");
			REQUIRE(FileSystem::WriteFileAtomic(directory.GetPath() / "File.txt", AsBytes(std::string_view("not a directory")), { .KeepBackup = false })
					.has_value());
			// The log file's name taken by a directory.
			const std::filesystem::path occupied = directory.GetPath() / "Occupied";
			REQUIRE(FileSystem::CreateDirectories(occupied / std::format("GpuLifetime-{}.log", Process::GetCurrentId())).has_value());
			for (const std::filesystem::path& logDirectory : { directory.GetPath() / "File.txt" / "Logs", occupied })
			{
				CAPTURE(FileSystem::PathToUtf8(logDirectory));
				Test::ExpectLog warning(LogLevel::Warn, "GPU lifetime log: cannot");
				GpuResourceTracker tracker(logDirectory);
				CHECK(warning.GetMatchCount() == 1);
				CHECK(tracker.GetLifetimeLogPath().empty());
				tracker.RecordCreated(GpuResourceType::HostImage);
				CHECK(tracker.GetCounts(GpuResourceType::HostImage).Created == 1);
				CHECK(tracker.GetLiveCount(GpuResourceType::HostImage) == 1);
				tracker.RecordDestroyed(GpuResourceType::HostImage);
				CHECK(tracker.GetTotalLiveCount() == 0);
			}
			CHECK_FALSE(FileSystem::Exists(directory.GetPath() / "File.txt" / "Logs"));
		}

		TEST_CASE("GpuResourceTracker: ENGINE_GPU_LIFETIME_LOG turns the lifetime log on in its directory")
		{
			Test::TempDirectory directory("GpuLifetimeLogEnvironment");
			ProcessSpecification specification = Test::MakeTestsChildSpecification({ "--death-test=Graphics/LifetimeLogFromEnvironment" });
			specification.Environment.emplace_back(std::string(GpuLifetimeLogVariable), FileSystem::PathToUtf8(directory.GetPath()));
			const Result<ProcessResult> child = Process::Run(specification, std::chrono::seconds(120));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stderr: ", child->StandardError);
			CHECK(child->ExitCode == ExitCode::Failed); // the body returns without dying
			const Result<std::vector<std::filesystem::path>> files = FileSystem::ListDirectory(directory.GetPath());
			REQUIRE_MESSAGE(files.has_value(), files.error().ToString());
			REQUIRE(files->size() == 1);
			CHECK(files->front().filename().string().starts_with("GpuLifetime-"));
			std::vector<std::string> events;
			for (const std::vector<std::string>& fields : ReadLifetimeLog(files->front()))
			{
				REQUIRE(fields.size() >= 4);
				events.push_back(fields[3]);
			}
			CHECK(events == std::vector<std::string>{ "open", "host-create", "host-destroy", "close" });
		}

		TEST_CASE("GpuResourceTracker: the lifetime log names each object's creation, destruction and leak with its native handle"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			nvrhi::IDevice* nvrhiDevice = gpu.GetDevice().GetNvrhiDevice();
			Test::TempDirectory directory("GpuLifetimeObjects");
			std::filesystem::path path;
			std::string trackerField;
			std::string samplerAddress;
			std::string bufferAddress;
			std::string commandListAddress;
			{
				// Objects made through NVRHI directly, so this tracker alone tracks them (the device's tracks its own).
				GpuResourceTracker tracker(directory.GetPath());
				path = tracker.GetLifetimeLogPath();
				trackerField = std::format("tracker={}", static_cast<const void*>(&tracker));
				nvrhi::SamplerHandle sampler = nvrhiDevice->createSampler(nvrhi::SamplerDesc());
				REQUIRE(sampler != nullptr);
				samplerAddress = std::format("{}", static_cast<const void*>(sampler.Get()));
				tracker.Track(GpuResourceType::Sampler, sampler.Get());
				sampler = nullptr;
				tracker.Sweep();
				nvrhi::BufferDesc bufferDesc;
				bufferDesc.byteSize = 256;
				bufferDesc.debugName = "LifetimeLog";
				nvrhi::BufferHandle buffer = nvrhiDevice->createBuffer(bufferDesc);
				REQUIRE(buffer != nullptr);
				bufferAddress = std::format("{}", static_cast<const void*>(buffer.Get()));
				tracker.Track(GpuResourceType::GpuBuffer, buffer.Get());
				// A command list that was never opened has no command buffer: its line names none, and asking NVRHI for one
				// would read through its null current buffer.
				nvrhi::CommandListHandle commandList = nvrhiDevice->createCommandList();
				REQUIRE(commandList != nullptr);
				commandListAddress = std::format("{}", static_cast<const void*>(commandList.Get()));
				tracker.Track(GpuResourceType::CommandList, commandList.Get());
				commandList = nullptr;
				tracker.Sweep();
				// Still held here, so ReleaseAll gives it up as a leak.
				tracker.ReleaseAll();
			}
			const std::vector<std::string> events = GetLifetimeEvents(ReadLifetimeLog(path), trackerField);
			REQUIRE(events.size() == 8);
			CHECK(events[1].starts_with("create Sampler " + samplerAddress + " native=0x"));
			CHECK_FALSE(events[1].ends_with("native=0x0"));
			CHECK(events[2].starts_with("destroy Sampler " + samplerAddress + " native=0x"));
			CHECK(events[3].starts_with("create GpuBuffer " + bufferAddress + " native=0x"));
			CHECK_FALSE(events[3].ends_with("native=0x0"));
			CHECK(events[4] == "create CommandList " + commandListAddress + " native=0x0");
			CHECK(events[5] == "destroy CommandList " + commandListAddress + " native=0x0");
			CHECK(events[6].starts_with("leak GpuBuffer " + bufferAddress + " native=0x"));
			CHECK(events[7] == "close");
		}

		TEST_CASE("GpuResourceTracker: every type has its enumerator name")
		{
			CHECK(GpuResourceTypeToString(GpuResourceType::Texture) == "Texture");
			CHECK(GpuResourceTypeToString(GpuResourceType::GpuBuffer) == "GpuBuffer");
			CHECK(GpuResourceTypeToString(GpuResourceType::GraphicsPipeline) == "GraphicsPipeline");
			CHECK(GpuResourceTypeToString(GpuResourceType::HostImage) == "HostImage");
			for (size_t index = 0; index < GpuResourceTypeCount; ++index)
				CHECK_FALSE(GpuResourceTypeToString(static_cast<GpuResourceType>(index)).empty());
			CHECK(GpuResourceTracker::IsEnabled()); // Tests never build Dist
		}
	}

}
