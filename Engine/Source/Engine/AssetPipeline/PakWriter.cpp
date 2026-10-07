#include "EnginePCH.h"
#include "Engine/AssetPipeline/PakWriter.h"

// M6 contract stub (Roadmap rule 3): stream D (caches, pak, PakMount) implements the pak writer.

namespace Engine {

	Status PakWriter::Add(PakWriterEntry /*entry*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PakWriter::Add is an M6 contract stub");
	}

	void PakWriter::SetMetadata(VariantValue /*metadata*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Buffer PakWriter::Build() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status PakWriter::WriteToFile(const std::filesystem::path& /*path*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PakWriter::WriteToFile is an M6 contract stub");
	}

}
