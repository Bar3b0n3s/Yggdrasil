#include "TestsPCH.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Graphics/ShaderLibrary.h"
#include "Engine/ImGui/ImGuiRenderer.h"
#include "Engine/Renderer/TrianglePass.h"
#include "Shared/ImGuiConstants.h"
#include "Shared/SmokeConstants.h"
#include "Shared/ViewConstants.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/MatrixConventionProgram.h"
#include "Support/SmokeProgram.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <set>

// The static shader checks of Architecture §15.1 (T0, "Tests --test-suite=Static"): every engine pipeline's binding
// layouts against slangc's reflection, every shared struct's member offsets against the C++ layout, and the compiled
// output of every Shaders.json variant, all without a GPU. They read this configuration's compiled shaders
// (ENGINE_SHADER_DIRECTORY), which building the Tests project produces through the Shaders project. The GPU check of the
// matrix convention (§8.4) completes them.

namespace Engine {

	namespace {

		// A device-less ShaderLibrary over the compiled shaders, on its own VFS.
		class CompiledShaders
		{
		public:
			CompiledShaders()
			{
				Result<Scope<NativeDirectoryMount>> mount =
					NativeDirectoryMount::Create(std::filesystem::path(ENGINE_SHADER_DIRECTORY), MountAccess::ReadOnly);
				REQUIRE_MESSAGE(mount.has_value(), mount.error().ToString());
				REQUIRE(m_Vfs.Mount(ShaderScheme, std::move(*mount)).has_value());
				const Result<VfsPath> root = VfsPath::Create(ShaderScheme, "");
				REQUIRE(root.has_value());
				m_Library = CreateScope<ShaderLibrary>(nullptr, m_Vfs, *root);
			}

			[[nodiscard]] ShaderLibrary& GetLibrary() { return *m_Library; }
		private:
			VirtualFileSystem m_Vfs;
			Scope<ShaderLibrary> m_Library;
		};

	}

	// Every engine pipeline's layout description; a new pass adds its own here.
	static std::vector<PipelineLayoutDescription> GetEnginePipelineLayouts()
	{
		return {
			TrianglePass::GetLayoutDescription(),
			ImGuiRenderer::GetLayoutDescription(),
			Test::MakeMatrixConventionLayoutDescription(),
			Test::MakeSmokeLayoutDescription("0"),
			Test::MakeSmokeLayoutDescription("1"),
		};
	}

	namespace {

		// One member of a shared struct as the C++ compiler lays it out.
		struct SharedField
		{
			std::string Name{};
			size_t Offset = 0;
			size_t Size = 0;
		};

		struct SharedStruct
		{
			std::string Name{};
			size_t Size = 0;
			std::vector<SharedField> Fields{};
			// A variant whose reflection contains the struct.
			std::string Program{};
			std::string Entry{};
			std::vector<ShaderDefine> Permutation{};
		};

	}

	// One SharedField, for ENGINE_TEST_SHARED_FIELD.
	static SharedField MakeSharedField(std::string name, size_t offset, size_t size)
	{
		return { .Name = std::move(name), .Offset = offset, .Size = size };
	}

#define ENGINE_TEST_SHARED_FIELD(type, member) MakeSharedField(#member, offsetof(type, member), sizeof(type::member))

	// Every struct of Resources/Shaders/Shared with the C++ offsets of its members; a new shared struct adds itself here.
	static std::vector<SharedStruct> GetSharedStructs()
	{
		return {
			{
				.Name = "ViewConstants",
				.Size = sizeof(ViewConstants),
				.Fields = {
					ENGINE_TEST_SHARED_FIELD(ViewConstants, View),
					ENGINE_TEST_SHARED_FIELD(ViewConstants, Projection),
					ENGINE_TEST_SHARED_FIELD(ViewConstants, ViewProjection),
					ENGINE_TEST_SHARED_FIELD(ViewConstants, InverseProjection),
					ENGINE_TEST_SHARED_FIELD(ViewConstants, InverseViewProjection),
					ENGINE_TEST_SHARED_FIELD(ViewConstants, CameraPosition),
					ENGINE_TEST_SHARED_FIELD(ViewConstants, ProjectionKind),
					ENGINE_TEST_SHARED_FIELD(ViewConstants, OrthoHalfExtents),
					ENGINE_TEST_SHARED_FIELD(ViewConstants, Near),
					ENGINE_TEST_SHARED_FIELD(ViewConstants, Far),
					ENGINE_TEST_SHARED_FIELD(ViewConstants, ViewportSize),
					ENGINE_TEST_SHARED_FIELD(ViewConstants, InverseViewportSize),
					ENGINE_TEST_SHARED_FIELD(ViewConstants, TanHalfFovY),
					ENGINE_TEST_SHARED_FIELD(ViewConstants, AspectRatio),
					ENGINE_TEST_SHARED_FIELD(ViewConstants, Exposure),
					ENGINE_TEST_SHARED_FIELD(ViewConstants, Padding0),
				},
				.Program = "Triangle",
				.Entry = "VSMain",
			},
			{
				.Name = "ImGuiConstants",
				.Size = sizeof(ImGuiConstants),
				.Fields = {
					ENGINE_TEST_SHARED_FIELD(ImGuiConstants, Scale),
					ENGINE_TEST_SHARED_FIELD(ImGuiConstants, Translate),
				},
				.Program = "ImGui",
				.Entry = "VSMain",
			},
			{
				.Name = "SmokeConstants",
				.Size = sizeof(SmokeConstants),
				.Fields = {
					ENGINE_TEST_SHARED_FIELD(SmokeConstants, ElementCount),
					ENGINE_TEST_SHARED_FIELD(SmokeConstants, Scale),
					ENGINE_TEST_SHARED_FIELD(SmokeConstants, Reserved0),
					ENGINE_TEST_SHARED_FIELD(SmokeConstants, Reserved1),
				},
				.Program = "Smoke",
				.Entry = "CSMain",
				.Permutation = { { .Key = "SMOKE_SATURATE", .Value = "0" } },
			},
		};
	}

#undef ENGINE_TEST_SHARED_FIELD

	TEST_SUITE("Static")
	{
		TEST_CASE("Shaders: LayoutsMatchReflection" * doctest::skip(true))
		{
			CompiledShaders shaders;
			std::set<std::string> covered;
			for (const PipelineLayoutDescription& description : GetEnginePipelineLayouts())
			{
				CAPTURE(description.Name);
				const Status valid = ValidatePipelineLayout(description, shaders.GetLibrary());
				CHECK_MESSAGE(valid.has_value(), (valid.has_value() ? std::string() : valid.error().ToString()));
				covered.insert(description.Program);
			}

			// Every program of Shaders.json has a pipeline whose layout is checked.
			const Result<std::string> manifest = FileSystem::ReadText(std::filesystem::path(ENGINE_REPO_ROOT) / "Resources/Shaders/Shaders.json");
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			const Result<Json> document = JsonReader::Parse(*manifest);
			REQUIRE_MESSAGE(document.has_value(), document.error().ToString());
			const Result<JsonReader> programs = JsonReader(*document).GetMember("Programs");
			REQUIRE(programs.has_value());
			const Result<size_t> count = programs->GetArraySize();
			REQUIRE(count.has_value());
			for (size_t index = 0; index < *count; ++index)
			{
				const Result<JsonReader> program = programs->GetElement(index);
				REQUIRE(program.has_value());
				const Result<std::string> name = program->ReadMember<std::string>("Name");
				REQUIRE(name.has_value());
				CHECK_MESSAGE(covered.contains(*name), "program '" << *name << "' has no layout description in GetEnginePipelineLayouts");
			}

			// A layout that disagrees with the reflection is caught: the Triangle layout without its constant buffer.
			PipelineLayoutDescription broken = TrianglePass::GetLayoutDescription();
			REQUIRE_FALSE(broken.BindingLayouts.empty());
			broken.BindingLayouts[0].bindings.clear();
			const Status mismatch = ValidatePipelineLayout(broken, shaders.GetLibrary());
			REQUIRE_FALSE(mismatch.has_value());
			CHECK(mismatch.error().GetCode() == ErrorCode::Validation);
			CHECK(mismatch.error().ToString().contains("View"));
		}

		TEST_CASE("Shaders: SharedStructsMatchReflection" * doctest::skip(true))
		{
			CompiledShaders shaders;
			for (const SharedStruct& shared : GetSharedStructs())
			{
				CAPTURE(shared.Name);
				const Result<const ShaderReflection*> reflection =
					shaders.GetLibrary().GetReflection(shared.Program, shared.Entry, shared.Permutation);
				REQUIRE_MESSAGE(reflection.has_value(), reflection.error().ToString());
				const ShaderStruct* reflected = (*reflection)->FindStruct(shared.Name);
				REQUIRE_MESSAGE(reflected != nullptr, "the reflection of " << shared.Program << "/" << shared.Entry << " has no struct " << shared.Name);
				CHECK(reflected->Size == shared.Size);
				REQUIRE(reflected->Fields.size() == shared.Fields.size());
				for (size_t index = 0; index < shared.Fields.size(); ++index)
				{
					CAPTURE(shared.Fields[index].Name);
					CHECK(reflected->Fields[index].Name == shared.Fields[index].Name);
					CHECK(reflected->Fields[index].Offset == shared.Fields[index].Offset);
					CHECK(reflected->Fields[index].Size == shared.Fields[index].Size);
				}
			}
		}

		TEST_CASE("Shaders: every Shaders.json variant was compiled and passed spirv-val" * doctest::skip(true))
		{
			// CompileShaders.py runs spirv-val on every output and writes the stamp only after every program of a complete
			// run compiled and validated (§8.12), so a stamp of this configuration proves the whole manifest is spirv-val
			// clean; each variant then has its .spv and .refl.json.
			const std::filesystem::path directory(ENGINE_SHADER_DIRECTORY);
			const Result<std::string> stamp = FileSystem::ReadText(directory / ".stamp");
			REQUIRE_MESSAGE(stamp.has_value(), stamp.error().ToString());
			const Result<Json> document = JsonReader::Parse(*stamp);
			REQUIRE_MESSAGE(document.has_value(), document.error().ToString());
			const Result<std::string> configuration = JsonReader(*document).ReadMember<std::string>("Configuration");
			REQUIRE(configuration.has_value());
#if defined(ENGINE_DEBUG)
			CHECK(*configuration == "Debug");
#else
			CHECK(*configuration == "Release");
#endif
			const std::array<std::string_view, 7> variants = {
				"ImGui/VSMain",
				"ImGui/PSMain",
				"MatrixConvention/CSMain",
				"Triangle/VSMain",
				"Triangle/PSMain",
				"Smoke/CSMain.SMOKE_SATURATE-0",
				"Smoke/CSMain.SMOKE_SATURATE-1",
			};
			for (const std::string_view variant : variants)
			{
				CAPTURE(std::string(variant));
				CHECK(FileSystem::Exists(directory / (std::string(variant) + ".spv")));
				CHECK(FileSystem::Exists(directory / (std::string(variant) + ".refl.json")));
			}
		}

		TEST_CASE("Shaders: matrix convention transforms known vectors" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// §8.4: glm's column-major matrices, uploaded through a shared struct (ViewConstants::ViewProjection) and compiled
			// with -matrix-layout-column-major, give mul(M, v) == M * v. A matrix with sixteen distinct entries tells M * v
			// from the transposed product, and the small integers keep every product exact.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<ComputePipeline> pipeline = gpu.GetPipelines().CreateComputePipeline({ .Layout = Test::MakeMatrixConventionLayoutDescription() });
			REQUIRE_MESSAGE(pipeline.has_value(), pipeline.error().ToString());

			ViewConstants constants{};
			constants.ViewProjection = glm::mat4(glm::vec4(1.0f, 2.0f, 3.0f, 4.0f), glm::vec4(5.0f, 6.0f, 7.0f, 8.0f),
				glm::vec4(9.0f, 10.0f, 11.0f, 12.0f), glm::vec4(13.0f, 14.0f, 15.0f, 16.0f));
			const std::array<glm::vec4, 6> vectors = {
				glm::vec4(1.0f, 0.0f, 0.0f, 0.0f),
				glm::vec4(0.0f, 1.0f, 0.0f, 0.0f),
				glm::vec4(0.0f, 0.0f, 1.0f, 0.0f),
				glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
				glm::vec4(1.0f, 2.0f, 3.0f, 1.0f),
				glm::vec4(-0.5f, 0.25f, 2.0f, 1.0f),
			};
			const size_t byteSize = sizeof(vectors);

			nvrhi::BufferDesc constantsDesc;
			constantsDesc.byteSize = sizeof(ViewConstants);
			constantsDesc.isConstantBuffer = true;
			constantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
			constantsDesc.keepInitialState = true;
			constantsDesc.debugName = "MatrixConvention.View";
			Result<nvrhi::BufferHandle> constantBuffer = device.CreateBuffer(constantsDesc);
			REQUIRE_MESSAGE(constantBuffer.has_value(), constantBuffer.error().ToString());

			nvrhi::BufferDesc vectorsDesc;
			vectorsDesc.byteSize = byteSize;
			vectorsDesc.structStride = sizeof(glm::vec4);
			vectorsDesc.canHaveUAVs = true;
			vectorsDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
			vectorsDesc.keepInitialState = true;
			vectorsDesc.debugName = "MatrixConvention.Vectors";
			Result<nvrhi::BufferHandle> vectorBuffer = device.CreateBuffer(vectorsDesc);
			REQUIRE_MESSAGE(vectorBuffer.has_value(), vectorBuffer.error().ToString());

			nvrhi::BufferDesc readbackDesc;
			readbackDesc.byteSize = byteSize;
			readbackDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
			readbackDesc.initialState = nvrhi::ResourceStates::CopyDest;
			readbackDesc.keepInitialState = true;
			readbackDesc.debugName = "MatrixConvention.Readback";
			Result<nvrhi::BufferHandle> readbackBuffer = device.CreateBuffer(readbackDesc);
			REQUIRE_MESSAGE(readbackBuffer.has_value(), readbackBuffer.error().ToString());

			nvrhi::BindingSetDesc bindings;
			bindings.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, *constantBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_UAV(0, *vectorBuffer),
			};
			REQUIRE(pipeline->BindingLayouts.size() == 1);
			Result<nvrhi::BindingSetHandle> bindingSet = device.CreateBindingSet(bindings, *pipeline->BindingLayouts[0]);
			REQUIRE_MESSAGE(bindingSet.has_value(), bindingSet.error().ToString());

			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			(*commandList)->open();
			(*commandList)->writeBuffer(*constantBuffer, &constants, sizeof(constants));
			(*commandList)->writeBuffer(*vectorBuffer, vectors.data(), byteSize);
			nvrhi::ComputeState state;
			state.pipeline = pipeline->Pipeline;
			state.bindings = { *bindingSet };
			(*commandList)->setComputeState(state);
			(*commandList)->dispatch(1);
			(*commandList)->copyBuffer(*readbackBuffer, 0, *vectorBuffer, 0, byteSize);
			(*commandList)->close();
			WaitForSubmission(device, device.ExecuteCommandList(**commandList), "the matrix-convention test");

			const void* mapped = device.GetNvrhiDevice()->mapBuffer(*readbackBuffer, nvrhi::CpuAccessMode::Read);
			REQUIRE(mapped != nullptr);
			std::array<glm::vec4, 6> transformed{};
			std::memcpy(transformed.data(), mapped, byteSize);
			device.GetNvrhiDevice()->unmapBuffer(*readbackBuffer);
			for (size_t index = 0; index < vectors.size(); ++index)
			{
				CAPTURE(index);
				const glm::vec4 expected = constants.ViewProjection * vectors[index];
				for (glm::length_t component = 0; component < 4; ++component)
				{
					CAPTURE(component);
					CHECK(transformed[index][component] == expected[component]);
				}
			}
		}
	}

}
