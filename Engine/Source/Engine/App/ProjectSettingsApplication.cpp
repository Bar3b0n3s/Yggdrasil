#include "EnginePCH.h"
#include "Engine/App/ProjectSettingsApplication.h"

namespace Engine {

	void ApplyGameSettings(ApplicationSpecification& /*specification*/, std::string_view /*name*/, const WindowSettings& /*window*/,
		const SimulationSettings& /*simulation*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	ScriptingSettings ApplyTestSuiteOverrides(const ScriptingSettings& /*project*/, const TestSuiteOverrides& /*overrides*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
