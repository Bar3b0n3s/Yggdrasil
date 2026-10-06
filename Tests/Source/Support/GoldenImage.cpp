#include "TestsPCH.h"
#include "Support/GoldenImage.h"

#include "Support/TestData.h"
#include "Support/TestOptions.h"

// M5 contract stub (Roadmap rule 3): stream E (readback, ImageCompare, golden harness, screenshots) implements the golden
// paths, the comparison with its outputs, the update mode and the smoke mode of Architecture §15.4.

namespace Engine {

	namespace Test {

		std::filesystem::path GetGoldenDirectory(std::string_view /*deviceClass*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		std::filesystem::path GetGoldenOutputDirectory()
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		GoldenCheckResult CheckGoldenImage(std::string_view name, const Image& /*actual*/, std::string_view /*deviceClass*/,
			const ImageCompareThresholds& /*thresholds*/)
		{
			ENGINE_CONTRACT_STUB();
			return { .Outcome = GoldenOutcome::Error, .Message = std::format("golden check of '{}' is not implemented yet", name) };
		}

		std::string_view GoldenOutcomeToString(GoldenOutcome outcome)
		{
			switch (outcome)
			{
				case GoldenOutcome::Matched:     return "Matched";
				case GoldenOutcome::Mismatched:  return "Mismatched";
				case GoldenOutcome::Updated:     return "Updated";
				case GoldenOutcome::SmokePassed: return "SmokePassed";
				case GoldenOutcome::SmokeFailed: return "SmokeFailed";
				case GoldenOutcome::Error:       return "Error";
			}
			return "Unknown";
		}

	}

}
