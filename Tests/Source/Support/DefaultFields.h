#pragma once

#include "Engine/Core/Json/Json.h"

#include <string>
#include <vector>

// Gate 8's non-default walk (Architecture §15.6; Docs/Decisions/0022-m14-contract.md decisions 6 and 12), shared by the
// AllSettings.eproj test (Engine/Project/ProjectSettingsTests.cpp) and the gate 8 seeded-gap test
// (EditorCore/Project/ProjectSettingsObservationTests.cpp), so both judge "non-default" the same way.

namespace Engine {

	class StructInfo;

	namespace Test {

		// Appends to `uncovered` the JSON pointer (below `pointer`) of every serialized field of `type` that does not hold a
		// non-default value in any of `actuals` (each a JSON object of `type`). Struct fields recurse; arrays and maps of
		// structs count a field as covered when any element has a non-default value for it (elements compare with the
		// element struct's MakeDefaultJson, pointers use "*" for the element); other arrays and maps count as covered when
		// they differ from `defaults`.
		void CollectDefaultFields(const StructInfo& type, const std::vector<const Json*>& actuals, const Json& defaults, const std::string& pointer,
			std::vector<std::string>& uncovered);

	}

}
