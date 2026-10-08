#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"

#include <string>

// The error an importer returns for a failure of its own steps (decoding, synthesis, validation of the result), shared by
// the importers that report such failures as ImportFailed (AudioImporter, SoundEffectImporter).

namespace Engine {

	namespace Utils {

		// `error` as ImportFailed, keeping its message, location, hint, issues and contexts, located in `sourcePath` and with
		// `context` (what was being done) added last. Pure.
		[[nodiscard]] Error MakeImportFailed(const Error& error, const std::string& sourcePath, std::string context);

	}

}
