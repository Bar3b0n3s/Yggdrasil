#include "TestsPCH.h"

#include "Engine/Core/Result.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Support/GoldenImage.h"
#include "Support/GoldenScene.h"
#include "Support/HeadlessGpuFixture.h"

#include <cstddef>
#include <string_view>

// The MaterialParameters golden (M31; Docs/Decisions/0046-m31-material-parameters.md decision 9; World Tree's brief 5, "a
// tinted, glowing, scrolled and half-dissolved copy of one mesh beside the untouched one"): the golden scaffold
// Scaffold/GoldenMaterialParameters.jsonl (IdSeed 2802) writes Assets/Scenes/GoldenMaterialParameters/MaterialParameters.scene,
// a row of six copies of one checkered cube under a shadow-casting sun over a floor: untouched, tinted red, glowing cyan,
// its texture offset by half a checker square, half dissolved with an orange edge (its shadow dissolved with it) and half
// faded by the dither (its whole shadow kept). An edit scene renders at simulation time 0, so the scrolled copy shows its
// UVOffset alone.

namespace Engine {

	namespace {

		constexpr std::string_view MaterialParametersGoldenScene = "MaterialParameters";

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		// The five copies with parameters reach the snapshot resolved and the untouched one neutral, and the capture repeats
		// exactly (the dissolve noise and the fade's dither are fixed patterns).
		TEST_CASE("Golden: MaterialParameters")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			size_t withParameters = 0;
			const Test::GoldenSceneOptions options{ .InspectSnapshot = [&withParameters](const RenderSnapshot& snapshot)
			{
				withParameters = 0;
				for (const MeshDrawItem& item : snapshot.Meshes)
					withParameters += item.Parameters.IsNeutral() ? 0U : 1U;
			} };
			const Result<Image> image = Test::RenderGoldenScene(gpu, MaterialParametersGoldenScene, options);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			REQUIRE(image->IsValid());
			CHECK(image->Width == 640);
			CHECK(image->Height == 360);
			CHECK(withParameters == 5);
			const Result<Image> again = Test::RenderGoldenScene(gpu, MaterialParametersGoldenScene);
			REQUIRE_MESSAGE(again.has_value(), again.error().ToString());
			CHECK(again->Pixels == image->Pixels);
			ENGINE_CHECK_GOLDEN(MaterialParametersGoldenScene, *image, gpu.GetDevice().GetInfo().DeviceClass);
		}
	}

}
