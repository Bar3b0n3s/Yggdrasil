#include "EnginePCH.h"
#include "Engine/Graphics/GpuResourceTracker.h"

#include "Engine/Core/Assert.h"

#include <format>

namespace Engine {

	GpuResourceTracker::GpuResourceTracker() = default;

	GpuResourceTracker::~GpuResourceTracker() = default;

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
				++m_Counts[std::to_underlying(type)].Created;
		}
	}

	void GpuResourceTracker::RecordCreated(GpuResourceType type)
	{
		if constexpr (IsEnabled())
			++m_Counts[std::to_underlying(type)].Created;
	}

	void GpuResourceTracker::RecordDestroyed(GpuResourceType type)
	{
		if constexpr (IsEnabled())
		{
			GpuResourceCounts& counts = m_Counts[std::to_underlying(type)];
			ENGINE_CORE_ASSERT(counts.Destroyed < counts.Created, "GpuResourceTracker::RecordDestroyed of a {} that was never created",
				GpuResourceTypeToString(type));
			if (counts.Destroyed < counts.Created)
				++counts.Destroyed;
		}
	}

	void GpuResourceTracker::Sweep()
	{
		if constexpr (IsEnabled())
		{
			// Releasing an object can leave the tracker the last owner of an object it referenced, so the pass repeats
			// until it releases nothing.
			bool releasedAny = true;
			while (releasedAny)
			{
				releasedAny = false;
				for (auto entry = m_Tracked.begin(); entry != m_Tracked.end();)
				{
					if (entry->second.Resource->GetRefCount() > 1)
					{
						++entry;
						continue;
					}
					const GpuResourceType type = entry->second.Type;
					// Erasing drops the last reference, which destroys the object (NVRHI defers the Vulkan objects itself).
					entry = m_Tracked.erase(entry);
					++m_Counts[std::to_underlying(type)].Destroyed;
					releasedAny = true;
				}
			}
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
			m_Tracked.clear();
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
			case GpuResourceType::Buffer:           return "Buffer";
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
