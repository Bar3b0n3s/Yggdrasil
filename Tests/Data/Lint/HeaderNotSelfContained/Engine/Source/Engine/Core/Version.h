// Seeded defect: no #pragma once. The header still compiles when included twice, because it only declares a
// function, so the pragma-once rule is what catches it.
namespace Engine {

	int GetVersionNumber();

}
