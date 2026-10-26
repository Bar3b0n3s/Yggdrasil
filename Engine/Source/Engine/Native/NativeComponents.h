#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"
#include "Engine/Scene/LoadReport.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Native components in scenes (Docs/Decisions/0030-m19-contract.md decision 27). A native component lives in its entity's
// preserved components (Scene/Components/UnknownComponentsComponent.h), as the JSON its scene file holds under its registry
// name "Native.<Name>": the serializer keeps every "Native." component as written, with or without a module, and never
// reports it as unknown (IsNativeComponentName). With a module loaded, the functions below make each one canonical against
// the module's types: an object holding every field in registration order, written with NativeComponentVersion, the
// entity's native components sorted by registry name (byte order) ahead of its unknown components, so a scene saved with
// the module loaded is canonical (§6) and its state hash covers native data like any component's. The editor canonicalizes
// when a scene opens, when the module loads or reloads (its migration, by field name), and on every native.setComponents;
// a play session when it starts (PlaySession::Prepare). Main thread only, like the scene.

namespace Engine {

	class ConstEntity;
	class Entity;
	class NativeModuleTypes;
	class Scene;

	// What canonicalizing a scene's native components found.
	struct NativeSceneReport
	{
		// A component's JSON, its version or its position among the entity's preserved components changed; the editor then
		// marks the open scene modified (dirty). Not an undo step: like a load's migrations, it is the form the scene has
		// from now on.
		bool Changed = false;
		uint32_t ComponentCount = 0; // "Native." components in the scene, registered or not
		// The NATIVE_* codes of NativeModule.h, each with its entity and its pointer below the entity ("/Components/
		// Native.Spinner/Speed"), in canonical entity order, then component order.
		std::vector<LoadDiagnostic> Diagnostics{};
	};

	// Canonicalizes every "Native." component of `scene` against `types` (decision 27). A component `types` registers is
	// rewritten in its canonical JSON (CanonicalizeNativeValue: renamed fields carried, members it does not know dropped,
	// values that do not convert reset, each with its note as a diagnostic); one whose value its type rejects is kept as
	// written with NATIVE_COMPONENT_INVALID (an error); one `types` does not register, every one when `types` is null, is kept
	// as written with NATIVE_COMPONENT_UNREGISTERED. With `strictUnknowns` (--strict, LoadOptions::StrictUnknowns), the
	// warnings are errors too. Returns the report, whose errors leave the scene unchanged by this call; errors as values
	// (Validation, with every error diagnostic as an issue) are only for a scene whose value cannot be read at all.
	[[nodiscard]] Result<NativeSceneReport> CanonicalizeNativeComponents(Scene& scene, const NativeModuleTypes* types, bool strictUnknowns);

	// CanonicalizeNativeComponents' report for `scene`, which is left as it is: its diagnostics are the ones that call would
	// report, and Changed says whether that call would change the scene (false when it reports an error). The editor checks
	// every open scene with it before a module load migrates any of them, so that one scene's error leaves them all as they
	// were (Docs/Decisions/0031-m20-contract.md, "Merge with M19").
	[[nodiscard]] NativeSceneReport CheckNativeComponents(const Scene& scene, const NativeModuleTypes* types, bool strictUnknowns);

	// An entity's "Native." components (native.getComponents, decision 32), each with its registry name and JSON, in the
	// entity's order: Registered holds those `types` registers, Unregistered the others (all of them when `types` is null), as
	// written.
	struct NativeEntityComponents
	{
		std::vector<std::pair<std::string, VariantValue>> Registered{};
		std::vector<std::pair<std::string, VariantValue>> Unregistered{};
	};
	[[nodiscard]] NativeEntityComponents ReadNativeComponents(ConstEntity entity, const NativeModuleTypes* types);

	// Applies `patch` ({"Native.<Name>": <object> | null, ...}, native.setComponents) to `entity`'s native components, each
	// component as an RFC 7386 merge patch of its canonical JSON (a component the entity lacks starts from its type's
	// defaults; null removes it), then canonicalizes the result. All or nothing: errors leave the entity unchanged.
	// Returns whether anything changed. Errors: NotFound (with suggestions) for a name `types` does not register, located at
	// `pointer`/<name>; Validation for a value its type rejects, located below it; InvalidArgument for a patch that is not an
	// object.
	[[nodiscard]] Result<bool> PatchNativeComponents(Entity entity, const NativeModuleTypes& types, const Json& patch, std::string_view pointer);

}
