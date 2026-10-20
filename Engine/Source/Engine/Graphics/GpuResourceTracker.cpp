#include "EnginePCH.h"
#include "Engine/Graphics/GpuResourceTracker.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/Environment.h"
#include "Engine/Platform/Process.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

	struct GpuResourceTracker::LifetimeLog
	{
		std::filesystem::path Path{};
		std::ofstream Stream{};
		uint64_t Sequence = 0; // the lines written so far
	};

	namespace Utils {

		// The Vulkan object type NVRHI reports for an object of `type` (the lifetime log, GpuResourceTracker.h); 0 for none.
		[[nodiscard]] static nvrhi::ObjectType GetNativeObjectType(GpuResourceType type)
		{
			switch (type)
			{
				case GpuResourceType::Texture:          return nvrhi::ObjectTypes::VK_Image;
				case GpuResourceType::StagingTexture:   return nvrhi::ObjectTypes::VK_Buffer;
				case GpuResourceType::GpuBuffer:        return nvrhi::ObjectTypes::VK_Buffer;
				case GpuResourceType::Sampler:          return nvrhi::ObjectTypes::VK_Sampler;
				case GpuResourceType::Shader:           return nvrhi::ObjectTypes::VK_ShaderModule;
				case GpuResourceType::BindingLayout:    return nvrhi::ObjectTypes::VK_DescriptorSetLayout;
				case GpuResourceType::BindingSet:       return nvrhi::ObjectTypes::VK_DescriptorSet;
				case GpuResourceType::GraphicsPipeline: return nvrhi::ObjectTypes::VK_Pipeline;
				case GpuResourceType::ComputePipeline:  return nvrhi::ObjectTypes::VK_Pipeline;
				// None: an input layout or a query has no Vulkan object, a framebuffer has none under dynamic rendering (NVRHI
				// deprecates the type), and a command list's command buffer changes with every open and does not exist before
				// the first, when NVRHI would read it through a null pointer; their addresses identify them.
				case GpuResourceType::InputLayout:
				case GpuResourceType::Framebuffer:
				case GpuResourceType::CommandList:
				case GpuResourceType::EventQuery:
				case GpuResourceType::TimerQuery:
				case GpuResourceType::HostImage:
					return 0;
			}
			return 0;
		}

		// The file of the lifetime log in `directory` (GpuResourceTracker.h), created when missing; empty after one warning
		// when the directory cannot be made.
		[[nodiscard]] static std::filesystem::path MakeLifetimeLogPath(const std::filesystem::path& directory)
		{
			if (const Status created = FileSystem::CreateDirectories(directory); !created)
			{
				ENGINE_CORE_WARN("GPU lifetime log: cannot create {}, so the log is off: {}", FileSystem::PathToUtf8(directory), created.error());
				return {};
			}
			return directory / std::format("GpuLifetime-{}.log", Process::GetCurrentId());
		}

		// The directory ENGINE_GPU_LIFETIME_LOG names; empty when it is unset or empty.
		[[nodiscard]] static std::filesystem::path GetLifetimeLogDirectory()
		{
			const std::optional<std::string> value = ReadEnvironmentVariable(GpuLifetimeLogVariable);
			return value.has_value() ? FileSystem::PathFromUtf8(*value) : std::filesystem::path();
		}

	}

	GpuResourceTracker::GpuResourceTracker()
		: GpuResourceTracker(IsEnabled() ? Utils::GetLifetimeLogDirectory() : std::filesystem::path())
	{
	}

	GpuResourceTracker::GpuResourceTracker(const std::filesystem::path& lifetimeLogDirectory)
	{
		if constexpr (IsEnabled())
		{
			if (lifetimeLogDirectory.empty())
				return;
			std::filesystem::path path = Utils::MakeLifetimeLogPath(lifetimeLogDirectory);
			if (path.empty())
				return;
			Scope<LifetimeLog> log = CreateScope<LifetimeLog>();
			log->Stream.open(path, std::ios::binary | std::ios::app);
			if (!log->Stream)
			{
				ENGINE_CORE_WARN("GPU lifetime log: cannot open {}, so the log is off", FileSystem::PathToUtf8(path));
				return;
			}
			log->Path = std::move(path);
			m_LifetimeLog = std::move(log);
			LogEvent(std::format("open pid={}", Process::GetCurrentId()));
		}
	}

	GpuResourceTracker::~GpuResourceTracker()
	{
		LogEvent("close");
	}

	std::filesystem::path GpuResourceTracker::GetLifetimeLogPath() const
	{
		return m_LifetimeLog != nullptr ? m_LifetimeLog->Path : std::filesystem::path();
	}

	void GpuResourceTracker::LogEvent(std::string_view event, GpuResourceType type, nvrhi::IResource* resource)
	{
		if (m_LifetimeLog == nullptr)
			return;
		const nvrhi::ObjectType nativeType = Utils::GetNativeObjectType(type);
		const uint64_t native = nativeType != 0 && resource != nullptr ? resource->getNativeObject(nativeType).integer : 0;
		LogEvent(std::format("{} {} {} native={:#x}", event, GpuResourceTypeToString(type), static_cast<const void*>(resource), native));
	}

	void GpuResourceTracker::LogEvent(std::string_view event)
	{
		if (m_LifetimeLog == nullptr)
			return;
		LifetimeLog& log = *m_LifetimeLog;
		const auto now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch());
		log.Stream << std::format("{} {} tracker={} {}\n", ++log.Sequence, now.count(), static_cast<const void*>(this), event);
		// Through to the operating system at once, so a crash right after keeps the line (GpuResourceTracker.h).
		log.Stream.flush();
	}

	void GpuResourceTracker::Track(GpuResourceType type, nvrhi::IResource* resource)
	{
		ENGINE_CORE_ASSERT(resource != nullptr, "GpuResourceTracker::Track of a null {}", GpuResourceTypeToString(type));
		if constexpr (IsEnabled())
		{
			if (resource == nullptr)
				return;
			const auto [entry, inserted] = m_Tracked.try_emplace(resource, TrackedResource{ .Type = type, .Resource = resource });
			ENGINE_CORE_ASSERT(inserted, "GpuResourceTracker::Track of a {} it already tracks", GpuResourceTypeToString(type));
			if (inserted)
			{
				++m_Counts[std::to_underlying(type)].Created;
				LogEvent("create", type, resource);
			}
		}
	}

	void GpuResourceTracker::RecordCreated(GpuResourceType type)
	{
		if constexpr (IsEnabled())
		{
			++m_Counts[std::to_underlying(type)].Created;
			LogEvent(std::format("host-create {}", GpuResourceTypeToString(type)));
		}
	}

	void GpuResourceTracker::RecordDestroyed(GpuResourceType type)
	{
		if constexpr (IsEnabled())
		{
			GpuResourceCounts& counts = m_Counts[std::to_underlying(type)];
			ENGINE_CORE_ASSERT(counts.Destroyed < counts.Created, "GpuResourceTracker::RecordDestroyed of a {} that was never created",
				GpuResourceTypeToString(type));
			if (counts.Destroyed < counts.Created)
			{
				++counts.Destroyed;
				LogEvent(std::format("host-destroy {}", GpuResourceTypeToString(type)));
			}
		}
	}

	void GpuResourceTracker::Sweep()
	{
		if constexpr (IsEnabled())
		{
			// Releasing an object can leave the tracker the last owner of an object it referenced, so the pass repeats
			// until it releases nothing. A pass releases its objects in address order rather than the map's, so the lifetime
			// log lists them in a fixed order; the objects of one pass do not depend on each other (the tracker still holds
			// every one of them until it releases it).
			std::vector<nvrhi::IResource*> released;
			do
			{
				released.clear();
				for (const auto& [resource, tracked] : m_Tracked)
				{
					if (tracked.Resource->GetRefCount() <= 1)
						released.push_back(resource);
				}
				std::ranges::sort(released, std::less<>());
				for (nvrhi::IResource* resource : released)
				{
					const auto entry = m_Tracked.find(resource);
					const GpuResourceType type = entry->second.Type;
					// Logged while the object is alive, with its native handle.
					LogEvent("destroy", type, resource);
					// Erasing drops the last reference, which destroys the object (NVRHI defers the Vulkan objects itself).
					m_Tracked.erase(entry);
					++m_Counts[std::to_underlying(type)].Destroyed;
				}
			} while (!released.empty());
		}
	}

	bool GpuResourceTracker::HasOtherReferences(nvrhi::IResource& resource, uint32_t callerReferences) const
	{
		uint64_t ownReferences = callerReferences;
		if constexpr (IsEnabled())
		{
			if (m_Tracked.contains(&resource))
				++ownReferences;
		}
		return resource.GetRefCount() > ownReferences;
	}

	void GpuResourceTracker::ReleaseAll()
	{
		if constexpr (IsEnabled())
		{
			// Every object still held here is a leak (GraphicsDevice::Shutdown reports the counts); the log names each one, in
			// address order.
			if (m_LifetimeLog != nullptr)
			{
				std::vector<std::pair<nvrhi::IResource*, GpuResourceType>> leaked;
				leaked.reserve(m_Tracked.size());
				for (const auto& [resource, tracked] : m_Tracked)
					leaked.emplace_back(resource, tracked.Type);
				std::ranges::sort(leaked, std::less<>(), &std::pair<nvrhi::IResource*, GpuResourceType>::first);
				for (const auto& [resource, type] : leaked)
					LogEvent("leak", type, resource);
			}
			m_Tracked.clear();
		}
	}

	GpuResourceCounts GpuResourceTracker::GetCounts(GpuResourceType type) const
	{
		if constexpr (IsEnabled())
			return m_Counts[std::to_underlying(type)];
		else
			return {};
	}

	uint64_t GpuResourceTracker::GetLiveCount(GpuResourceType type) const
	{
		return GetCounts(type).GetLive();
	}

	uint64_t GpuResourceTracker::GetTotalLiveCount() const
	{
		uint64_t total = 0;
		for (size_t index = 0; index < GpuResourceTypeCount; ++index)
			total += GetLiveCount(static_cast<GpuResourceType>(index));
		return total;
	}

	std::string GpuResourceTracker::DescribeLiveCounts() const
	{
		std::string description;
		for (size_t index = 0; index < GpuResourceTypeCount; ++index)
		{
			const GpuResourceType type = static_cast<GpuResourceType>(index);
			const uint64_t live = GetLiveCount(type);
			if (live == 0)
				continue;
			if (!description.empty())
				description += ", ";
			description += std::format("{}: {}", GpuResourceTypeToString(type), live);
		}
		return description.empty() ? std::string("none") : description;
	}

	std::string_view GpuResourceTypeToString(GpuResourceType type)
	{
		switch (type)
		{
			case GpuResourceType::Texture:          return "Texture";
			case GpuResourceType::StagingTexture:   return "StagingTexture";
			case GpuResourceType::GpuBuffer:        return "GpuBuffer";
			case GpuResourceType::Sampler:          return "Sampler";
			case GpuResourceType::Shader:           return "Shader";
			case GpuResourceType::InputLayout:      return "InputLayout";
			case GpuResourceType::BindingLayout:    return "BindingLayout";
			case GpuResourceType::BindingSet:       return "BindingSet";
			case GpuResourceType::Framebuffer:      return "Framebuffer";
			case GpuResourceType::GraphicsPipeline: return "GraphicsPipeline";
			case GpuResourceType::ComputePipeline:  return "ComputePipeline";
			case GpuResourceType::CommandList:      return "CommandList";
			case GpuResourceType::EventQuery:       return "EventQuery";
			case GpuResourceType::TimerQuery:       return "TimerQuery";
			case GpuResourceType::HostImage:        return "HostImage";
		}

		ENGINE_CORE_ASSERT(false, "Unknown GpuResourceType {}", std::to_underlying(type));
		return "Unknown";
	}

}
