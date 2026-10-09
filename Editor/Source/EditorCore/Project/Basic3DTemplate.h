#pragma once

#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"

namespace Engine {

	class TypeRegistry;
	class UUIDGenerator;

	// Canonical scene document for Basic3D, shared by ProjectManager and its tests. Main thread, caller-owned generator.
	// Camera + AudioListener, sun, environment (built-in Studio), post-process, and ground with a static box collider.
	// Built-in references use catalogue handles, no copied assets. No script required; validates with zero errors AND
	// zero missing-listener warnings. Errors: serialization/registry Validation; no disk writes in this builder.
	[[nodiscard]] Result<Json> BuildBasic3DScene(const TypeRegistry& registry, UUIDGenerator& ids);
	// ProjectManager's Basic3D branch, with the same atomic-creation/error contract as CreateProject. Contract stub
	// explicitly refuses the unimplemented template; implementation reuses the Empty skeleton without recursion.
	[[nodiscard]] Result<CreatedProject> CreateBasic3DProject(const ProjectCreateSpecification& specification, const TypeRegistry& registry);

}
