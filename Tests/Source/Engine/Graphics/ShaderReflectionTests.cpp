#include "TestsPCH.h"

#include "Engine/Graphics/ShaderReflection.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Random.h"

// slangc's reflection JSON as CompileShaders.py writes it (<variant>.refl.json). The documents come from this
// configuration's compiled shaders (ENGINE_SHADER_DIRECTORY), so the parser is tested against the slangc version the
// toolchain pins. The engine's programs use no storage images, register spaces above 0, arrays or specialization
// constants yet, so those parts are tested on hand-written documents in slangc's format.

namespace Engine {

	static std::string ReadCompiledReflection(std::string_view variant)
	{
		const Result<std::string> text =
			FileSystem::ReadText(std::filesystem::path(ENGINE_SHADER_DIRECTORY) / std::format("{}.refl.json", variant));
		REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
		return *text;
	}

	// A compute entry point with a storage image, an array in register space 1, an unbounded array in space 2, a
	// specialization constant and push constants whose struct nests another struct directly and as array elements.
	static constexpr std::string_view SyntheticReflection = R"({
	"parameters": [
		{ "name": "Shadows", "binding": { "kind": "shaderResource", "space": 1, "index": 3 },
			"type": { "kind": "array", "elementCount": 4,
				"elementType": { "kind": "resource", "baseShape": "texture2D", "array": true } } },
		{ "name": "Output", "binding": { "kind": "unorderedAccess", "index": 386 }, "format": "r11f_g11f_b10f",
			"type": { "kind": "resource", "baseShape": "texture2D", "access": "readWrite" } },
		{ "name": "Cubes", "binding": { "kind": "shaderResource", "space": 2, "index": 0 },
			"type": { "kind": "array", "elementCount": 0, "elementType": { "kind": "resource", "baseShape": "textureCube" } } },
		{ "name": "Lights", "binding": { "kind": "shaderResource", "index": 5 },
			"type": { "kind": "resource", "baseShape": "structuredBuffer" } },
		{ "name": "Compare", "binding": { "kind": "samplerState", "index": 131 }, "type": { "kind": "samplerState" } },
		{ "name": "DebugView", "binding": { "kind": "specializationConstant", "index": 3 },
			"type": { "kind": "scalar", "scalarType": "int32" } },
		{ "name": "Draw", "binding": { "kind": "pushConstantBuffer", "index": 0 },
			"type": { "kind": "constantBuffer",
				"elementVarLayout": {
					"type": { "kind": "struct", "name": "DrawConstants", "fields": [
						{ "name": "World", "type": { "kind": "matrix", "rowCount": 4, "columnCount": 4 },
							"binding": { "kind": "uniform", "offset": 0, "size": 64 } },
						{ "name": "Inner", "type": { "kind": "struct", "name": "Nested", "fields": [
								{ "name": "A", "type": { "kind": "scalar", "scalarType": "float32" },
									"binding": { "kind": "uniform", "offset": 0, "size": 4 } } ] },
							"binding": { "kind": "uniform", "offset": 64, "size": 16 } },
						{ "name": "List", "type": { "kind": "array", "elementCount": 2, "uniformStride": 16,
								"elementType": { "kind": "struct", "name": "Nested", "fields": [
									{ "name": "A", "type": { "kind": "scalar", "scalarType": "float32" },
										"binding": { "kind": "uniform", "offset": 0, "size": 4 } } ] } },
							"binding": { "kind": "uniform", "offset": 80, "size": 32 } } ] },
					"binding": { "kind": "uniform", "offset": 0, "size": 112 } } } }
	],
	"entryPoints": [
		{ "name": "CSMain", "stage": "compute", "threadGroupSize": [8, 8, 1],
			"bindings": [
				{ "name": "Shadows", "binding": { "kind": "shaderResource", "space": 1, "index": 3, "used": 1 } },
				{ "name": "Output", "binding": { "kind": "unorderedAccess", "index": 386, "used": 0 } },
				{ "name": "DebugView", "binding": { "kind": "specializationConstant", "index": 3 } },
				{ "name": "Draw", "binding": { "kind": "pushConstantBuffer", "index": 0 } } ] }
	]
})";

	// SyntheticReflection with `from` replaced by `to` (which must occur).
	static std::string ReplaceInSynthetic(std::string_view from, std::string_view to)
	{
		std::string document(SyntheticReflection);
		const size_t position = document.find(from);
		REQUIRE(position != std::string::npos);
		document.replace(position, from.size(), to);
		return document;
	}

	// Parses `document`, which must fail with Validation located at `pointer`.
	static void CheckRejected(const std::string& document, std::string_view pointer)
	{
		const Result<ShaderReflection> reflection = ParseShaderReflection(document);
		REQUIRE_FALSE(reflection.has_value());
		CHECK(reflection.error().GetCode() == ErrorCode::Validation);
		REQUIRE(reflection.error().GetLocation().JsonPointer.has_value());
		CHECK(*reflection.error().GetLocation().JsonPointer == std::string(pointer));
	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("ShaderReflection: parses bindings, structs and the entry point of a vertex shader")
		{
			const Result<ShaderReflection> reflection = ParseShaderReflection(ReadCompiledReflection("Triangle/VSMain"));
			REQUIRE_MESSAGE(reflection.has_value(), reflection.error().ToString());
			CHECK(reflection->EntryPoint == "VSMain");
			CHECK(reflection->Stage == nvrhi::ShaderType::Vertex);
			const ShaderBinding* view = reflection->FindBinding("View");
			REQUIRE(view != nullptr);
			CHECK(view->Kind == ShaderBindingKind::ConstantBuffer);
			CHECK(view->Set == 0);
			CHECK(view->Binding == 256); // b0 shifted by NVRHI's CBV offset (§8.4)
			CHECK(view->ArraySize == 1);
			CHECK(view->ByteSize == 400); // ViewConstants with the light count M8 appended
			CHECK(view->StructName == "ViewConstants");
			CHECK(view->Used);
			const ShaderStruct* constants = reflection->FindStruct("ViewConstants");
			REQUIRE(constants != nullptr);
			CHECK(constants->Size == 400);
			REQUIRE_FALSE(constants->Fields.empty());
			CHECK(constants->Fields.front().Name == "View");
			CHECK(constants->Fields.front().Offset == 0);
			CHECK(constants->Fields.front().Size == 64);
		}

		TEST_CASE("ShaderReflection: parses push constants, textures, samplers and compute thread groups")
		{
			const Result<ShaderReflection> pixel = ParseShaderReflection(ReadCompiledReflection("ImGui/PSMain"));
			REQUIRE_MESSAGE(pixel.has_value(), pixel.error().ToString());
			CHECK(pixel->Stage == nvrhi::ShaderType::Pixel);
			const ShaderBinding* push = pixel->FindBinding("Projection");
			REQUIRE(push != nullptr);
			CHECK(push->Kind == ShaderBindingKind::PushConstantBuffer);
			CHECK(push->ByteSize == 16);
			const ShaderBinding* texture = pixel->FindBinding("Texture");
			REQUIRE(texture != nullptr);
			CHECK(texture->Kind == ShaderBindingKind::ShaderResource);
			CHECK(texture->Shape == ShaderResourceShape::Texture2D);
			CHECK(texture->Binding == 0);
			const ShaderBinding* sampler = pixel->FindBinding("Sampler");
			REQUIRE(sampler != nullptr);
			CHECK(sampler->Kind == ShaderBindingKind::Sampler);
			CHECK(sampler->Binding == 128);

			const Result<ShaderReflection> compute = ParseShaderReflection(ReadCompiledReflection("Smoke/CSMain.SMOKE_SATURATE-1"));
			REQUIRE_MESSAGE(compute.has_value(), compute.error().ToString());
			CHECK(compute->Stage == nvrhi::ShaderType::Compute);
			CHECK(compute->ThreadGroupSize == std::array<uint32_t, 3>{ 64, 1, 1 });
			const ShaderBinding* values = compute->FindBinding("Values");
			REQUIRE(values != nullptr);
			CHECK(values->Kind == ShaderBindingKind::UnorderedAccess);
			CHECK(values->Shape == ShaderResourceShape::StructuredBuffer);
			CHECK(values->Binding == 384);
			CHECK_FALSE(values->StorageFormat.has_value());
		}

		TEST_CASE("ShaderReflection: use flags come from the entry point")
		{
			// The Triangle fragment shader declares View (the program's global) without using it; the vertex shader uses it.
			const Result<ShaderReflection> pixel = ParseShaderReflection(ReadCompiledReflection("Triangle/PSMain"));
			REQUIRE_MESSAGE(pixel.has_value(), pixel.error().ToString());
			const ShaderBinding* view = pixel->FindBinding("View");
			REQUIRE(view != nullptr);
			CHECK_FALSE(view->Used);
			CHECK(pixel->ThreadGroupSize == std::array<uint32_t, 3>{ 0, 0, 0 });

			// The ImGui vertex shader uses its push constants, for which slangc writes no use flag: they count as used.
			const Result<ShaderReflection> vertex = ParseShaderReflection(ReadCompiledReflection("ImGui/VSMain"));
			REQUIRE_MESSAGE(vertex.has_value(), vertex.error().ToString());
			const ShaderBinding* projection = vertex->FindBinding("Projection");
			REQUIRE(projection != nullptr);
			CHECK(projection->Used);
			CHECK(projection->StructName == "ImGuiConstants");
		}

		TEST_CASE("ShaderReflection: reads register spaces, arrays and storage formats and skips specialization constants")
		{
			const Result<ShaderReflection> reflection = ParseShaderReflection(SyntheticReflection);
			REQUIRE_MESSAGE(reflection.has_value(), reflection.error().ToString());
			CHECK(reflection->EntryPoint == "CSMain");
			CHECK(reflection->ThreadGroupSize == std::array<uint32_t, 3>{ 8, 8, 1 });
			REQUIRE(reflection->Bindings.size() == 6);
			CHECK(reflection->FindBinding("DebugView") == nullptr);

			const ShaderBinding* shadows = reflection->FindBinding("Shadows");
			REQUIRE(shadows != nullptr);
			CHECK(shadows->Set == 1);
			CHECK(shadows->Binding == 3);
			CHECK(shadows->ArraySize == 4);
			CHECK(shadows->Shape == ShaderResourceShape::Texture2DArray);
			CHECK(shadows->Used);

			const ShaderBinding* output = reflection->FindBinding("Output");
			REQUIRE(output != nullptr);
			CHECK(output->Kind == ShaderBindingKind::UnorderedAccess);
			CHECK(output->Shape == ShaderResourceShape::Texture2D);
			CHECK(output->StorageFormat == nvrhi::Format::R11G11B10_FLOAT);
			CHECK_FALSE(output->Used);

			// Unbounded arrays have size 0; a parameter the entry point does not list counts as used.
			const ShaderBinding* cubes = reflection->FindBinding("Cubes");
			REQUIRE(cubes != nullptr);
			CHECK(cubes->Set == 2);
			CHECK(cubes->ArraySize == 0);
			CHECK(cubes->Shape == ShaderResourceShape::TextureCube);
			CHECK(cubes->Used);

			const ShaderBinding* lights = reflection->FindBinding("Lights");
			REQUIRE(lights != nullptr);
			CHECK(lights->Shape == ShaderResourceShape::StructuredBuffer);
			const ShaderBinding* compare = reflection->FindBinding("Compare");
			REQUIRE(compare != nullptr);
			CHECK(compare->Kind == ShaderBindingKind::Sampler);
			CHECK(compare->Binding == 131);

			const ShaderBinding* draw = reflection->FindBinding("Draw");
			REQUIRE(draw != nullptr);
			CHECK(draw->Kind == ShaderBindingKind::PushConstantBuffer);
			CHECK(draw->Set == 0);
			CHECK(draw->Binding == 0);
			CHECK(draw->ByteSize == 112);
			CHECK(draw->StructName == "DrawConstants");

			// Every reachable struct once, sorted by name; a nested struct has its own size (member size or array stride).
			REQUIRE(reflection->Structs.size() == 2);
			CHECK(reflection->Structs[0].Name == "DrawConstants");
			CHECK(reflection->Structs[1].Name == "Nested");
			const ShaderStruct* drawConstants = reflection->FindStruct("DrawConstants");
			REQUIRE(drawConstants != nullptr);
			CHECK(drawConstants->Size == 112);
			REQUIRE(drawConstants->Fields.size() == 3);
			CHECK(drawConstants->Fields[1].Name == "Inner");
			CHECK(drawConstants->Fields[1].Offset == 64);
			CHECK(drawConstants->Fields[2].Name == "List");
			CHECK(drawConstants->Fields[2].Offset == 80);
			CHECK(drawConstants->Fields[2].Size == 32);
			const ShaderStruct* nested = reflection->FindStruct("Nested");
			REQUIRE(nested != nullptr);
			CHECK(nested->Size == 16);
			REQUIRE(nested->Fields.size() == 1);
			CHECK(nested->Fields[0].Size == 4);
		}

		TEST_CASE("ShaderReflection: shapes outside the binding model are located Validation errors")
		{
			SUBCASE("unknown stage")
			{
				CheckRejected(ReplaceInSynthetic(R"("stage": "compute")", R"("stage": "geometry")"), "/entryPoints/0/stage");
			}
			SUBCASE("thread-group size without three dimensions")
			{
				CheckRejected(ReplaceInSynthetic("[8, 8, 1]", "[8, 8]"), "/entryPoints/0/threadGroupSize");
			}
			SUBCASE("more than one entry point")
			{
				const std::string document = ReplaceInSynthetic(R"("entryPoints": [)",
					R"("entryPoints": [ { "name": "Other", "stage": "compute", "threadGroupSize": [1, 1, 1] },)");
				CheckRejected(document, "/entryPoints");
			}
			SUBCASE("unknown binding kind")
			{
				CheckRejected(ReplaceInSynthetic(R"({ "kind": "samplerState", "index": 131 })", R"({ "kind": "uniform", "index": 0 })"),
					"/parameters/4/binding/kind");
			}
			SUBCASE("a parameter with several binding kinds")
			{
				CheckRejected(ReplaceInSynthetic(R"("binding": { "kind": "samplerState", "index": 131 })",
								  R"("bindings": [ { "kind": "samplerState", "index": 0 }, { "kind": "uniform", "offset": 0, "size": 16 } ])"),
					"/parameters/4");
			}
			SUBCASE("unknown image format")
			{
				CheckRejected(ReplaceInSynthetic(R"("format": "r11f_g11f_b10f")", R"("format": "rgb9e5")"), "/parameters/1/format");
			}
			SUBCASE("multisampled texture")
			{
				CheckRejected(ReplaceInSynthetic(R"("baseShape": "texture2D", "access")", R"("baseShape": "texture2D", "multisample": true, "access")"),
					"/parameters/1/type/multisample");
			}
			SUBCASE("unsupported resource shape")
			{
				CheckRejected(ReplaceInSynthetic(R"("baseShape": "structuredBuffer")", R"("baseShape": "accelerationStructure")"),
					"/parameters/3/type/baseShape");
			}
			SUBCASE("array of arrays")
			{
				CheckRejected(ReplaceInSynthetic(R"("elementType": { "kind": "resource", "baseShape": "textureCube" })",
								  R"("elementType": { "kind": "array", "elementCount": 2, "elementType": { "kind": "resource", "baseShape": "textureCube" } })"),
					"/parameters/2/type/elementType");
			}
			SUBCASE("a resource inside a constant-buffer struct")
			{
				CheckRejected(ReplaceInSynthetic(R"("binding": { "kind": "uniform", "offset": 0, "size": 64 })",
								  R"("binding": { "kind": "shaderResource", "index": 0 })"),
					"/parameters/6/type/elementVarLayout/type/fields/0/binding");
			}
			SUBCASE("a use flag that is not a number")
			{
				CheckRejected(ReplaceInSynthetic(R"("used": 0)", R"("used": "no")"), "/entryPoints/0/bindings/1/binding/used");
			}
		}

		TEST_CASE("ShaderReflection: a structured buffer's struct element is collected with its fields and extent")
		{
			// slangc reports a structured buffer's element type as "resultType", its fields with the std430 offsets of a
			// constant buffer's but without a stride: the struct's size is the end of its last field. A vector element carries
			// no struct.
			constexpr std::string_view Document = R"({
	"parameters": [
		{ "name": "Lights", "binding": { "kind": "shaderResource", "index": 0 },
			"type": { "kind": "resource", "baseShape": "structuredBuffer",
				"resultType": { "kind": "struct", "name": "Light", "fields": [
					{ "name": "Position", "type": { "kind": "vector", "elementCount": 3, "elementType": { "kind": "scalar", "scalarType": "float32" } },
						"binding": { "kind": "uniform", "offset": 0, "size": 12, "elementStride": 4 } },
					{ "name": "Range", "type": { "kind": "scalar", "scalarType": "float32" },
						"binding": { "kind": "uniform", "offset": 12, "size": 4, "elementStride": 0 } },
					{ "name": "Color", "type": { "kind": "vector", "elementCount": 3, "elementType": { "kind": "scalar", "scalarType": "float32" } },
						"binding": { "kind": "uniform", "offset": 16, "size": 12, "elementStride": 4 } } ] } } },
		{ "name": "Values", "binding": { "kind": "unorderedAccess", "index": 384 },
			"type": { "kind": "resource", "baseShape": "structuredBuffer", "access": "readWrite",
				"resultType": { "kind": "vector", "elementCount": 4, "elementType": { "kind": "scalar", "scalarType": "float32" } } } }
	],
	"entryPoints": [ { "name": "CSMain", "stage": "compute", "threadGroupSize": [64, 1, 1], "bindings": [] } ]
})";
			const Result<ShaderReflection> reflection = ParseShaderReflection(Document);
			REQUIRE_MESSAGE(reflection.has_value(), reflection.error().ToString());
			REQUIRE(reflection->Bindings.size() == 2);
			CHECK(reflection->FindBinding("Lights")->Shape == ShaderResourceShape::StructuredBuffer);
			CHECK(reflection->FindBinding("Values")->Shape == ShaderResourceShape::StructuredBuffer);
			REQUIRE(reflection->Structs.size() == 1);
			const ShaderStruct* light = reflection->FindStruct("Light");
			REQUIRE(light != nullptr);
			CHECK(light->Size == 28);
			REQUIRE(light->Fields.size() == 3);
			CHECK(light->Fields[1].Name == "Range");
			CHECK(light->Fields[1].Offset == 12);
			CHECK(light->Fields[2].Offset == 16);
			CHECK(light->Fields[2].Size == 12);

			// The compiled forward pass's light list: ShaderLight, 64 bytes (Shared/ShaderLight.h).
			const Result<ShaderReflection> forward = ParseShaderReflection(ReadCompiledReflection("Scene/PSForward.ALPHA_MASK-0"));
			REQUIRE_MESSAGE(forward.has_value(), forward.error().ToString());
			const ShaderStruct* shaderLight = forward->FindStruct("ShaderLight");
			REQUIRE(shaderLight != nullptr);
			CHECK(shaderLight->Size == 64);
		}

		TEST_CASE("ShaderReflection: malformed and mutated documents are errors, never crashes")
		{
			const Result<ShaderReflection> notJson = ParseShaderReflection("{ \"parameters\": [");
			REQUIRE_FALSE(notJson.has_value());
			CHECK(notJson.error().GetCode() == ErrorCode::Parse);
			const Result<ShaderReflection> noEntry = ParseShaderReflection(R"({ "parameters": [], "entryPoints": [] })");
			REQUIRE_FALSE(noEntry.has_value());
			CHECK(noEntry.error().GetCode() == ErrorCode::Validation);

			// Seeded byte mutations of a real document: every outcome is a value.
			const std::string original = ReadCompiledReflection("Triangle/VSMain");
			Random random(0x5EED5EEDu);
			for (int iteration = 0; iteration < 2000; ++iteration)
			{
				std::string mutated = original;
				const uint32_t flips = 1 + random.NextU32() % 4;
				for (uint32_t flip = 0; flip < flips; ++flip)
					mutated[random.NextU32() % mutated.size()] = static_cast<char>(random.NextU32() & 0xFF);
				const Result<ShaderReflection> parsed = ParseShaderReflection(mutated);
				if (!parsed.has_value())
					CHECK((parsed.error().GetCode() == ErrorCode::Parse || parsed.error().GetCode() == ErrorCode::Validation));
			}
		}

		TEST_CASE("ShaderReflection: lookups find bindings and structs by name")
		{
			ShaderReflection reflection;
			reflection.Bindings.push_back({ .Name = "View", .Kind = ShaderBindingKind::ConstantBuffer, .Binding = 256 });
			reflection.Bindings.push_back({ .Name = "Sampler", .Kind = ShaderBindingKind::Sampler, .Binding = 128 });
			reflection.Structs.push_back({ .Name = "ViewConstants", .Size = 384 });
			REQUIRE(reflection.FindBinding("Sampler") != nullptr);
			CHECK(reflection.FindBinding("Sampler")->Binding == 128);
			CHECK(reflection.FindBinding("Missing") == nullptr);
			REQUIRE(reflection.FindStruct("ViewConstants") != nullptr);
			CHECK(reflection.FindStruct("ViewConstants")->Size == 384);
			CHECK(reflection.FindStruct("view") == nullptr);
			CHECK(ShaderBindingKindToString(ShaderBindingKind::PushConstantBuffer) == "PushConstantBuffer");
		}
	}

}
