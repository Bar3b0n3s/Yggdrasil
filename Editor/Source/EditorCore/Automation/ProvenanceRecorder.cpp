#include "EditorPCH.h"
#include "EditorCore/Automation/ProvenanceRecorder.h"

#include "Engine/Core/VirtualFileSystem.h"

// M4 contract stub (Roadmap rule 3): stream C (methods, validator, provenance) implements provenance.

namespace Engine {

	bool ProvenanceRecorder::IsRecordedPath(std::string_view /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	Result<ProvenanceRecorder> ProvenanceRecorder::Load(const VirtualFileSystem& /*vfs*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProvenanceRecorder::Load is an M4 contract stub");
	}

	std::string ProvenanceRecorder::ToText(std::span<const ProvenanceEntry> /*entries*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<std::vector<ProvenanceEntry>> ProvenanceRecorder::FromText(std::string_view /*text*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProvenanceRecorder::FromText is an M4 contract stub");
	}

	void ProvenanceRecorder::Record(std::string_view /*path*/, uint64_t /*hash*/, const WriteAttribution& /*attribution*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Status ProvenanceRecorder::Save(VirtualFileSystem& /*vfs*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProvenanceRecorder::Save is an M4 contract stub");
	}

	const ProvenanceEntry* ProvenanceRecorder::Find(std::string_view /*path*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

}
