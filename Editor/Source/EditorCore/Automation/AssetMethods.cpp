#include "EditorPCH.h"
#include "EditorCore/Automation/AssetMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Reflection/TypeRegistry.h"

// M6 contract stub (Roadmap rule 3): stream E (commands, methods, hot reload) implements the asset domain: its handlers,
// the registration of its structs and enums (JSON keys per the conventions of MethodRegistry.h) and of its methods, with
// their Python tests, the method coverage and the regenerated Tools/MCP/catalog.json.

namespace Engine {

	namespace Automation {

		Result<AssetListResult> AssetList(EditorMethodContext& /*context*/, const AssetListParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::AssetList is an M6 contract stub");
		}

		Result<AssetInfoResult> AssetInfo(EditorMethodContext& /*context*/, const AssetInfoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::AssetInfo is an M6 contract stub");
		}

		Result<Scope<PendingOperation>> AssetImport(EditorMethodContext& /*context*/, const AssetImportParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::AssetImport is an M6 contract stub");
		}

		Result<Scope<PendingOperation>> AssetReimport(EditorMethodContext& /*context*/, const AssetReimportParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::AssetReimport is an M6 contract stub");
		}

		Result<AssetCreateResult> AssetCreate(EditorMethodContext& /*context*/, const AssetCreateParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::AssetCreate is an M6 contract stub");
		}

		Result<AssetGetPropertiesResult> AssetGetProperties(EditorMethodContext& /*context*/, const AssetGetPropertiesParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::AssetGetProperties is an M6 contract stub");
		}

		Result<AssetSetPropertiesResult> AssetSetProperties(EditorMethodContext& /*context*/, const AssetSetPropertiesParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::AssetSetProperties is an M6 contract stub");
		}

		Result<AssetGetImportSettingsResult> AssetGetImportSettings(EditorMethodContext& /*context*/, const AssetGetImportSettingsParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::AssetGetImportSettings is an M6 contract stub");
		}

		Result<AssetSetImportSettingsResult> AssetSetImportSettings(EditorMethodContext& /*context*/, const AssetSetImportSettingsParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::AssetSetImportSettings is an M6 contract stub");
		}

		Result<AssetMoveResult> AssetMove(EditorMethodContext& /*context*/, const AssetMoveParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::AssetMove is an M6 contract stub");
		}

		Result<AssetDeleteResult> AssetDelete(EditorMethodContext& /*context*/, const AssetDeleteParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::AssetDelete is an M6 contract stub");
		}

	}

	void RegisterAssetMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterAssetMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
