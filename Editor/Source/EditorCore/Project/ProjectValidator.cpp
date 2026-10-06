#include "EditorPCH.h"
#include "EditorCore/Project/ProjectValidator.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/LoadReport.h"

// M4 contract stub (Roadmap rule 3): stream C (validator) implements validation, fixes and diagnostic ids. The code list
// and the load-code mapping (ADR 0006 decision 35) are data the contract freezes, so they are real.

namespace Engine {

	namespace {

		constexpr std::array ValidatorCodes = {
			SceneNoPrimaryCameraCode,
			SceneMultiplePrimaryCamerasCode,
			SceneDuplicateUniqueComponentCode,
			SceneNonCanonicalOrderCode,
			SceneInvalidHierarchyCode,
			SceneInconsistentPrefabLinkCode,
			EntityDuplicateIdCode,
			EntityDanglingReferenceCode,
			ComponentFieldOutOfRangeCode,
			ComponentMissingRequirementCode,
			ComponentConflictCode,
			AssetImportFailedCode,
			BuildStartSceneMissingCode,
			BuildSceneMissingCode,
		};

	}

	Result<ValidationReport> ProjectValidator::Validate(const EditorContext& /*context*/, ValidationScope /*scope*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectValidator::Validate is an M4 contract stub");
	}

	Result<FixReport> ProjectValidator::Fix(EditorContext& /*context*/, ValidationScope /*scope*/, const FixSelection& /*selection*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectValidator::Fix is an M4 contract stub");
	}

	std::string ProjectValidator::MakeDiagnosticId(std::string_view /*code*/, std::string_view /*file*/, std::string_view /*entity*/,
		std::string_view /*component*/, std::string_view /*field*/, std::string_view /*subject*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::span<const std::string_view> ProjectValidator::GetCodes()
	{
		return ValidatorCodes;
	}

	std::string_view ProjectValidator::MapLoadCode(std::string_view loadCode)
	{
		// ADR 0006 decision 35. SCENE_INVALID_COMPONENT has no single validator code: the validator runs its own registry
		// checks for the COMPONENT_* codes instead of splitting it by cause.
		if (loadCode == SceneDuplicateIdCode)
			return EntityDuplicateIdCode;
		if (loadCode == SceneDanglingParentCode || loadCode == SceneParentCycleCode)
			return SceneInvalidHierarchyCode;
		if (loadCode == SceneDuplicateUniqueComponentCode)
			return SceneDuplicateUniqueComponentCode;
		if (loadCode == SceneNonCanonicalOrderCode)
			return SceneNonCanonicalOrderCode;
		if (loadCode == SceneInconsistentPrefabLinkCode)
			return SceneInconsistentPrefabLinkCode;
		return {};
	}

	void RegisterProjectValidatorTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
