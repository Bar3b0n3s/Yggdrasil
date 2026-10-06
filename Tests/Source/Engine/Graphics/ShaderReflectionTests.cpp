#include "TestsPCH.h"

#include "Engine/Graphics/ShaderReflection.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Random.h"

// slangc's reflection JSON as CompileShaders.py writes it (<variant>.refl.json). The documents come from this
// configuration's compiled shaders (ENGINE_SHADER_DIRECTORY), so the parser is tested against the slangc version the
// toolchain pins.

namespace Engine {

	static std::string ReadCompiledReflection(std::string_view variant)
	{
		const Result<std::string> text =
			FileSystem::ReadText(std::filesystem::path(ENGINE_SHADER_DIRECTORY) / (std::string(variant) + ".refl.json"));
		REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
		return *text;
	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("ShaderReflection: parses bindings, structs and the entry point of a vertex shader" * doctest::skip(true))
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
			CHECK(view->ByteSize == 384);
			CHECK(view->StructName == "ViewConstants");
			CHECK(view->Used);
			const ShaderStruct* constants = reflection->FindStruct("ViewConstants");
			REQUIRE(constants != nullptr);
			CHECK(constants->Size == 384);
			REQUIRE_FALSE(constants->Fields.empty());
			CHECK(constants->Fields.front().Name == "View");
			CHECK(constants->Fields.front().Offset == 0);
			CHECK(constants->Fields.front().Size == 64);
		}

		TEST_CASE("ShaderReflection: parses push constants, textures, samplers and compute thread groups" * doctest::skip(true))
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

		TEST_CASE("ShaderReflection: malformed and mutated documents are errors, never crashes" * doctest::skip(true))
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
