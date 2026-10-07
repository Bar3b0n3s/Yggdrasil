#include "EnginePCH.h"
#include "Engine/Asset/PakReader.h"

// M6 contract stub (Roadmap rule 3): stream D (caches, pak, PakMount) implements the reader. The accessors read the state
// Open fills.

namespace Engine {

	struct PakReader::State
	{
		std::string Name;
		PakHeader Header;
		PakToc Toc;
	};

	PakReader::PakReader(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	PakReader::~PakReader() = default;

	Result<Ref<const PakReader>> PakReader::Open(const std::filesystem::path& /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PakReader::Open is an M6 contract stub");
	}

	Result<Ref<const PakReader>> PakReader::OpenMemory(Buffer /*bytes*/, std::string /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PakReader::OpenMemory is an M6 contract stub");
	}

	const std::string& PakReader::GetName() const
	{
		return m_State->Name;
	}

	const PakHeader& PakReader::GetHeader() const
	{
		return m_State->Header;
	}

	std::span<const PakEntry> PakReader::GetEntries() const
	{
		return m_State->Toc.Entries;
	}

	const VariantValue& PakReader::GetMetadata() const
	{
		return m_State->Toc.Metadata;
	}

	const PakEntry* PakReader::FindByHandle(AssetHandle /*handle*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const PakEntry* PakReader::FindByPath(std::string_view /*path*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	Result<Buffer> PakReader::ReadEntry(const PakEntry& /*entry*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PakReader::ReadEntry is an M6 contract stub");
	}

}
