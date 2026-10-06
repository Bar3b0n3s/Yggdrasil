#include "TestsPCH.h"
#include "Support/FixtureSchemaSource.h"

// M3 contract stub (Roadmap rule 3): stream A (Reflection) implements the fixture schema source with the registry suite.

namespace Engine {

	namespace Test {

		void FixtureSchemaSource::DeclareField(std::string_view /*name*/, FieldType /*kind*/, const FieldMeta& /*meta*/, UUID /*owner*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		FixtureSchemaSource FixtureSchemaSource::CreateStandard()
		{
			ENGINE_CONTRACT_STUB();
			return FixtureSchemaSource();
		}

		Result<const FieldInfo*> FixtureSchemaSource::FindField(UUID /*owner*/, std::string_view /*name*/) const
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "FixtureSchemaSource::FindField is an M3 contract stub");
		}

		std::vector<std::string> FixtureSchemaSource::GetFieldNames(UUID /*owner*/) const
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

	}

}
