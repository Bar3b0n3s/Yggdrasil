#include "EnginePCH.h"
#include "Engine/Core/Profiler.h"

// M1 contract stub (Roadmap rule 3): stream D implements the per-thread rings, collection and the Chrome trace export.
// Until then nothing is recorded.

namespace Engine {

	void Profiler::Initialize(const ProfilerSpecification& /*specification*/)
	{
	}

	void Profiler::Shutdown()
	{
	}

	bool Profiler::IsInitialized()
	{
		return false;
	}

	void Profiler::SetThreadName(std::string_view /*name*/)
	{
	}

	void Profiler::RecordZone(const char* /*name*/, uint64_t /*beginNs*/, uint64_t /*endNs*/, uint32_t /*depth*/)
	{
	}

	void Profiler::SubmitGpuZones(std::span<const ProfileZone> /*zones*/)
	{
	}

	std::vector<ProfileZone> Profiler::CollectZones(uint64_t /*sinceNs*/)
	{
		return {};
	}

	std::string Profiler::ExportChromeTrace(uint64_t /*sinceNs*/)
	{
		return {};
	}

	uint64_t Profiler::GetTimeNs()
	{
		return 0;
	}

	ProfileScope::ProfileScope(const char* name)
		: m_Name(name), m_BeginNs(Profiler::GetTimeNs())
	{
	}

	ProfileScope::~ProfileScope()
	{
		Profiler::RecordZone(m_Name, m_BeginNs, Profiler::GetTimeNs(), m_Depth);
	}

}
