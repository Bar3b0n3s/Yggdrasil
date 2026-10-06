#include "EnginePCH.h"
#include "Engine/Graphics/GpuResourceTracker.h"

#include "Engine/Core/Assert.h"

// M5 contract stub (Roadmap rule 3): stream A (loader, device, selection, creation wrappers, host image upload) implements
// the counts and the sweep of Architecture §8.14 item 5 ("GpuResourceTracker: live counts return to zero").

namespace Engine {

	GpuResourceTracker::GpuResourceTracker() = default;

	GpuResourceTracker::~GpuResourceTracker() = default;

	void GpuResourceTracker::Track(GpuResourceType /*type*/, nvrhi::IResource* /*resource*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void GpuResourceTracker::RecordCreated(GpuResourceType /*type*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void GpuResourceTracker::RecordDestroyed(GpuResourceType /*type*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void GpuResourceTracker::Sweep()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool GpuResourceTracker::HasOtherReferences(nvrhi::IResource& /*resource*/, uint32_t /*callerReferences*/) const
	{
		ENGINE_CONTRACT_STUB();
		return true;
	}

	void GpuResourceTracker::ReleaseAll()
	{
		ENGINE_CONTRACT_STUB();
	}

	GpuResourceCounts GpuResourceTracker::GetCounts(GpuResourceType /*type*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	uint64_t GpuResourceTracker::GetLiveCount(GpuResourceType /*type*/) const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	uint64_t GpuResourceTracker::GetTotalLiveCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	std::string GpuResourceTracker::DescribeLiveCounts() const
	{
		ENGINE_CONTRACT_STUB();
		return "none";
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
