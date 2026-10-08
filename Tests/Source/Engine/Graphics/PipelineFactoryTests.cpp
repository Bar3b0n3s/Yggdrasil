#include "TestsPCH.h"

#include "Engine/Graphics/PipelineFactory.h"

#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/TrianglePass.h"
#include "Shared/SmokeConstants.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/SmokeProgram.h"

#include <cstring>

// ValidatePipelineLayout runs on hand-written reflection documents in slangc's format, served by a ShaderLibrary over a
// memory mount, so every kind of mismatch is tested without a GPU; the engine's own pipelines are checked against the
// compiled shaders by "Shaders: LayoutsMatchReflection".

namespace Engine {

	// Program "Probe", entry point CSMain: set 0 holds b0 Constants, t0 Input, u0 Output (rgba16f), s0 Sampler, the unused
	// t5 Unused and 16 bytes of push constants; set 1 holds the array t2 Array[3].
	static constexpr std::string_view ProbeReflection = R"({
	"parameters": [
		{ "name": "Constants", "binding": { "kind": "constantBuffer", "index": 256 },
			"type": { "kind": "constantBuffer", "elementVarLayout": {
				"type": { "kind": "struct", "name": "ProbeConstants", "fields": [
					{ "name": "Scale", "type": { "kind": "scalar", "scalarType": "float32" },
						"binding": { "kind": "uniform", "offset": 0, "size": 4 } } ] },
				"binding": { "kind": "uniform", "offset": 0, "size": 16 } } } },
		{ "name": "Input", "binding": { "kind": "shaderResource", "index": 0 },
			"type": { "kind": "resource", "baseShape": "texture2D" } },
		{ "name": "Output", "binding": { "kind": "unorderedAccess", "index": 384 }, "format": "rgba16f",
			"type": { "kind": "resource", "baseShape": "texture2D", "access": "readWrite" } },
		{ "name": "Sampler", "binding": { "kind": "samplerState", "index": 128 }, "type": { "kind": "samplerState" } },
		{ "name": "Unused", "binding": { "kind": "shaderResource", "index": 5 },
			"type": { "kind": "resource", "baseShape": "texture2D" } },
		{ "name": "Array", "binding": { "kind": "shaderResource", "space": 1, "index": 2 },
			"type": { "kind": "array", "elementCount": 3, "elementType": { "kind": "resource", "baseShape": "texture2D" } } },
		{ "name": "Draw", "binding": { "kind": "pushConstantBuffer", "index": 0 },
			"type": { "kind": "constantBuffer", "elementVarLayout": {
				"type": { "kind": "struct", "name": "DrawConstants", "fields": [] },
				"binding": { "kind": "uniform", "offset": 0, "size": 16 } } } }
	],
	"entryPoints": [
		{ "name": "CSMain", "stage": "compute", "threadGroupSize": [8, 8, 1],
			"bindings": [
				{ "name": "Constants", "binding": { "kind": "constantBuffer", "index": 256, "used": 1 } },
				{ "name": "Input", "binding": { "kind": "shaderResource", "index": 0, "used": 1 } },
				{ "name": "Output", "binding": { "kind": "unorderedAccess", "index": 384, "used": 1 } },
				{ "name": "Sampler", "binding": { "kind": "samplerState", "index": 128, "used": 1 } },
				{ "name": "Unused", "binding": { "kind": "shaderResource", "index": 5, "used": 0 } },
				{ "name": "Array", "binding": { "kind": "shaderResource", "space": 1, "index": 2, "used": 1 } },
				{ "name": "Draw", "binding": { "kind": "pushConstantBuffer", "index": 0 } } ] }
	]
})";

	// Program "Odd", entry point CSMain: a storage image without [vk::image_format] and an unbounded array.
	static constexpr std::string_view OddReflection = R"({
	"parameters": [
		{ "name": "Output", "binding": { "kind": "unorderedAccess", "index": 384 },
			"type": { "kind": "resource", "baseShape": "texture2D", "access": "readWrite" } },
		{ "name": "Textures", "binding": { "kind": "shaderResource", "index": 0 },
			"type": { "kind": "array", "elementCount": 0, "elementType": { "kind": "resource", "baseShape": "texture2D" } } }
	],
	"entryPoints": [ { "name": "CSMain", "stage": "compute", "threadGroupSize": [1, 1, 1] } ]
})";

	// Program "Split": a vertex shader using b0 View and a fragment shader using t0 Texture, which declares View with
	// another size.
	static constexpr std::string_view SplitVertexReflection = R"({
	"parameters": [
		{ "name": "View", "binding": { "kind": "constantBuffer", "index": 256 },
			"type": { "kind": "constantBuffer", "elementVarLayout": {
				"type": { "kind": "struct", "name": "SplitConstants", "fields": [] },
				"binding": { "kind": "uniform", "offset": 0, "size": 64 } } } },
		{ "name": "Texture", "binding": { "kind": "shaderResource", "index": 0 },
			"type": { "kind": "resource", "baseShape": "texture2D" } }
	],
	"entryPoints": [ { "name": "VSMain", "stage": "vertex", "bindings": [
		{ "name": "View", "binding": { "kind": "constantBuffer", "index": 256, "used": 1 } },
		{ "name": "Texture", "binding": { "kind": "shaderResource", "index": 0, "used": 0 } } ] } ]
})";

	static constexpr std::string_view SplitPixelReflection = R"({
	"parameters": [
		{ "name": "View", "binding": { "kind": "constantBuffer", "index": 256 },
			"type": { "kind": "constantBuffer", "elementVarLayout": {
				"type": { "kind": "struct", "name": "SplitConstants", "fields": [] },
				"binding": { "kind": "uniform", "offset": 0, "size": 80 } } } },
		{ "name": "Texture", "binding": { "kind": "shaderResource", "index": 0 },
			"type": { "kind": "resource", "baseShape": "texture2D" } }
	],
	"entryPoints": [ { "name": "PSMain", "stage": "fragment", "bindings": [
		{ "name": "View", "binding": { "kind": "constantBuffer", "index": 256, "used": 0 } },
		{ "name": "Texture", "binding": { "kind": "shaderResource", "index": 0, "used": 1 } } ] } ]
})";

	namespace {

		// A device-less ShaderLibrary over the reflection documents above.
		class SyntheticShaders
		{
		public:
			SyntheticShaders()
			{
				Scope<MemoryMount> mount = CreateScope<MemoryMount>();
				Write(*mount, "Probe/CSMain.refl.json", ProbeReflection);
				Write(*mount, "Odd/CSMain.refl.json", OddReflection);
				Write(*mount, "Split/VSMain.refl.json", SplitVertexReflection);
				Write(*mount, "Split/PSMain.refl.json", SplitPixelReflection);
				REQUIRE(m_Vfs.Mount(ShaderScheme, std::move(mount)).has_value());
				const Result<VfsPath> root = VfsPath::Create(ShaderScheme, "");
				REQUIRE(root.has_value());
				m_Library = CreateScope<ShaderLibrary>(nullptr, m_Vfs, *root);
			}

			[[nodiscard]] ShaderLibrary& GetLibrary() { return *m_Library; }
		private:
			static void Write(MemoryMount& mount, std::string_view path, std::string_view text)
			{
				const Result<VfsPath> file = VfsPath::Create(ShaderScheme, path);
				REQUIRE(file.has_value());
				REQUIRE(mount.CreateDirectories(file->GetParent()).has_value());
				REQUIRE(mount.WriteFileAtomic(*file, std::span(reinterpret_cast<const std::byte*>(text.data()), text.size())).has_value());
			}
		private:
			VirtualFileSystem m_Vfs;
			Scope<ShaderLibrary> m_Library;
		};

	}

	// The layout that matches ProbeReflection.
	static PipelineLayoutDescription MakeProbeLayout()
	{
		nvrhi::BindingLayoutDesc set0;
		set0.visibility = nvrhi::ShaderType::Compute;
		set0.registerSpace = 0;
		set0.registerSpaceIsDescriptorSet = true;
		set0.bindings = {
			nvrhi::BindingLayoutItem::ConstantBuffer(0),
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Texture_UAV(0),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::PushConstants(0, 16),
		};
		nvrhi::BindingLayoutDesc set1;
		set1.visibility = nvrhi::ShaderType::Compute;
		set1.registerSpace = 1;
		set1.registerSpaceIsDescriptorSet = true;
		set1.bindings = { nvrhi::BindingLayoutItem::Texture_SRV(2).setSize(3) };
		return {
			.Name = "Probe",
			.Program = "Probe",
			.Entries = { "CSMain" },
			.BindingLayouts = { set1, set0 }, // any order
			.StorageImages = { { .Set = 0, .Register = 0, .Format = nvrhi::Format::RGBA16_FLOAT } },
			.ConstantBuffers = { { .Set = 0, .Register = 0, .ByteSize = 16 } },
		};
	}

	// The item of `layout` with `type` and `slot` (which must exist).
	static nvrhi::BindingLayoutItem& FindItem(nvrhi::BindingLayoutDesc& layout, nvrhi::ResourceType type, uint32_t slot)
	{
		const auto found = std::ranges::find_if(layout.bindings, [type, slot](const nvrhi::BindingLayoutItem& item)
		{
			return item.type == type && item.slot == slot;
		});
		REQUIRE(found != layout.bindings.end());
		return *found;
	}

	// Removes the item of `layout` with `type` and `slot`.
	static void RemoveItem(nvrhi::BindingLayoutDesc& layout, nvrhi::ResourceType type, uint32_t slot)
	{
		const size_t removed = std::erase_if(layout.bindings, [type, slot](const nvrhi::BindingLayoutItem& item)
		{
			return item.type == type && item.slot == slot;
		});
		REQUIRE(removed == 1);
	}

	// ValidatePipelineLayout must fail with Validation and an issue at `pointer` whose message contains `text`.
	static void CheckMismatch(const PipelineLayoutDescription& description, ShaderLibrary& shaders, std::string_view pointer,
		std::string_view text)
	{
		const Status valid = ValidatePipelineLayout(description, shaders);
		REQUIRE_FALSE(valid.has_value());
		INFO(valid.error().ToString());
		CHECK(valid.error().GetCode() == ErrorCode::Validation);
		CHECK(valid.error().ToString().contains(std::format("pipeline '{}'", description.Name)));
		const std::vector<ErrorIssue>& issues = valid.error().GetIssues();
		const bool found = std::ranges::any_of(issues, [pointer, text](const ErrorIssue& issue)
		{
			return issue.JsonPointer == pointer && issue.Message.contains(text);
		});
		CHECK_MESSAGE(found, "no issue at " << std::string(pointer) << " mentioning '" << std::string(text) << "'");
	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("PipelineFactory: a layout that matches the reflection passes the check")
		{
			SyntheticShaders shaders;
			const Status valid = ValidatePipelineLayout(MakeProbeLayout(), shaders.GetLibrary());
			CHECK_MESSAGE(valid.has_value(), (valid.has_value() ? std::string() : valid.error().ToString()));

			// A binding no entry point uses may be declared, with matching type.
			PipelineLayoutDescription withUnused = MakeProbeLayout();
			withUnused.BindingLayouts[1].bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(5));
			const Status unusedDeclared = ValidatePipelineLayout(withUnused, shaders.GetLibrary());
			CHECK_MESSAGE(unusedDeclared.has_value(), (unusedDeclared.has_value() ? std::string() : unusedDeclared.error().ToString()));

			// A volatile constant buffer provides the same descriptor as a constant buffer.
			PipelineLayoutDescription volatileConstants = MakeProbeLayout();
			FindItem(volatileConstants.BindingLayouts[1], nvrhi::ResourceType::ConstantBuffer, 0).setType(nvrhi::ResourceType::VolatileConstantBuffer);
			CHECK(ValidatePipelineLayout(volatileConstants, shaders.GetLibrary()).has_value());
		}

		TEST_CASE("PipelineFactory: every binding mismatch is reported with the set and the binding name")
		{
			SyntheticShaders shaders;
			ShaderLibrary& library = shaders.GetLibrary();
			PipelineLayoutDescription layout = MakeProbeLayout();
			nvrhi::BindingLayoutDesc& set0 = layout.BindingLayouts[1];
			nvrhi::BindingLayoutDesc& set1 = layout.BindingLayouts[0];

			SUBCASE("a used binding the layout lacks")
			{
				RemoveItem(set0, nvrhi::ResourceType::Texture_SRV, 0);
				CheckMismatch(layout, library, "/0/Input", "does not declare it");
			}
			SUBCASE("a used binding in a set without a layout")
			{
				layout.BindingLayouts.erase(layout.BindingLayouts.begin());
				CheckMismatch(layout, library, "/1/Array", "no layout declares set 1");
			}
			SUBCASE("a layout item no shader declares")
			{
				set0.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(7));
				CheckMismatch(layout, library, "/0/t7", "no entry point of 'Probe' declares");
			}
			SUBCASE("a buffer item for a texture")
			{
				FindItem(set0, nvrhi::ResourceType::Texture_SRV, 0).setType(nvrhi::ResourceType::StructuredBuffer_SRV);
				CheckMismatch(layout, library, "/0/Input", "is a Texture2D");
			}
			SUBCASE("another register class at the same Vulkan binding")
			{
				// t128 lands on Vulkan binding 128, where the shaders have the sampler s0.
				RemoveItem(set0, nvrhi::ResourceType::Sampler, 0);
				set0.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(128));
				CheckMismatch(layout, library, "/0/Sampler", "Texture_SRV t128");
			}
			SUBCASE("another array size")
			{
				FindItem(set1, nvrhi::ResourceType::Texture_SRV, 2).setSize(2);
				CheckMismatch(layout, library, "/1/Array", "has 3 element(s)");
			}
			SUBCASE("a stage outside the layout's visibility")
			{
				set1.visibility = nvrhi::ShaderType::Pixel;
				CheckMismatch(layout, library, "/1/Array", "used by the Compute stage(s) but the layout of set 1 is visible to Pixel");
			}
			SUBCASE("a storage format that differs from the shader's")
			{
				layout.StorageImages[0].Format = nvrhi::Format::R8_UNORM;
				CheckMismatch(layout, library, "/0/Output", "the pass creates R8_UNORM");
			}
			SUBCASE("a storage image without a StorageImages entry")
			{
				layout.StorageImages.clear();
				CheckMismatch(layout, library, "/0/Output", "no StorageImages entry");
			}
			SUBCASE("a StorageImages entry without a storage image")
			{
				layout.StorageImages.push_back({ .Set = 1, .Register = 4, .Format = nvrhi::Format::R8_UNORM });
				CheckMismatch(layout, library, "/1/u4", "not a storage image");
			}
			SUBCASE("push constants of another size")
			{
				FindItem(set0, nvrhi::ResourceType::PushConstants, 0).setSize(32);
				CheckMismatch(layout, library, "/0/Draw", "declares 32 bytes");
			}
			SUBCASE("push constants the layout lacks")
			{
				RemoveItem(set0, nvrhi::ResourceType::PushConstants, 0);
				CheckMismatch(layout, library, "/0/Draw", "no layout has a PushConstants item");
			}
			SUBCASE("push constants outside the layout's visibility")
			{
				set0.visibility = nvrhi::ShaderType::Vertex;
				CheckMismatch(layout, library, "/0/Draw", "declared by the Compute stage(s) but the layout of set 0 is visible to Vertex");
			}
			SUBCASE("two push-constant items")
			{
				set1.bindings.push_back(nvrhi::BindingLayoutItem::PushConstants(0, 16));
				CheckMismatch(layout, library, "/1/PushConstants", "more than one");
			}
			SUBCASE("a layout without registerSpaceIsDescriptorSet")
			{
				set1.registerSpaceIsDescriptorSet = false;
				CheckMismatch(layout, library, "/1", "registerSpaceIsDescriptorSet");
			}
			SUBCASE("two layouts of one set")
			{
				set1.registerSpace = 0;
				CheckMismatch(layout, library, "/0", "two layouts declare descriptor set 0");
			}
			SUBCASE("binding offsets other than the shader compiler's shifts")
			{
				set0.bindingOffsets.setConstantBufferOffset(512);
				CheckMismatch(layout, library, "/0/Constants", "does not declare it");
				CheckMismatch(layout, library, "/0/b0", "Vulkan binding 512");
			}
			SUBCASE("an item type outside the binding model")
			{
				set0.bindings.push_back(nvrhi::BindingLayoutItem::RayTracingAccelStruct(9));
				CheckMismatch(layout, library, "/0/slot9", "RayTracingAccelStruct");
			}
		}

		TEST_CASE("PipelineFactory: every constant buffer's C++ size must equal its reflected struct size")
		{
			SyntheticShaders shaders;
			ShaderLibrary& library = shaders.GetLibrary();
			PipelineLayoutDescription layout = MakeProbeLayout();

			SUBCASE("a pass that binds a buffer of another size")
			{
				layout.ConstantBuffers[0].ByteSize = 32;
				CheckMismatch(layout, library, "/0/Constants", "binds 32 bytes to constant buffer");
			}
			SUBCASE("a constant buffer without a size")
			{
				layout.ConstantBuffers.clear();
				CheckMismatch(layout, library, "/0/Constants", "no ConstantBuffers entry");
			}
			SUBCASE("a size for a register that is no constant buffer")
			{
				layout.ConstantBuffers.push_back({ .Set = 0, .Register = 3, .ByteSize = 64 });
				CheckMismatch(layout, library, "/0/b3", "not a constant buffer of the layout and shaders");
			}
		}

		TEST_CASE("PipelineFactory: storage images need a format and arrays a fixed size")
		{
			SyntheticShaders shaders;
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = { nvrhi::BindingLayoutItem::Texture_UAV(0), nvrhi::BindingLayoutItem::Texture_SRV(0).setSize(4) };
			const PipelineLayoutDescription description = {
				.Name = "Odd",
				.Program = "Odd",
				.Entries = { "CSMain" },
				.BindingLayouts = { layout },
				.StorageImages = { { .Set = 0, .Register = 0, .Format = nvrhi::Format::RGBA8_UNORM } },
			};
			CheckMismatch(description, shaders.GetLibrary(), "/0/Output", "declares no [vk::image_format]");
			CheckMismatch(description, shaders.GetLibrary(), "/0/Textures", "unbounded array");
		}

		TEST_CASE("PipelineFactory: entry points must agree and each stage must see what it uses")
		{
			SyntheticShaders shaders;
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Vertex;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(0) };
			const PipelineLayoutDescription description = {
				.Name = "Split",
				.Program = "Split",
				.Entries = { "VSMain", "PSMain" },
				.BindingLayouts = { layout },
			};
			CheckMismatch(description, shaders.GetLibrary(), "/0/View", "different declarations");
			CheckMismatch(description, shaders.GetLibrary(), "/0/Texture", "used by the Pixel stage(s)");
		}

		TEST_CASE("PipelineFactory: the check propagates reflection errors and needs entry points")
		{
			SyntheticShaders shaders;
			PipelineLayoutDescription missing = MakeProbeLayout();
			missing.Entries = { "PSMain" };
			const Status notCompiled = ValidatePipelineLayout(missing, shaders.GetLibrary());
			REQUIRE_FALSE(notCompiled.has_value());
			CHECK(notCompiled.error().GetCode() == ErrorCode::NotFound);
			CHECK(notCompiled.error().ToString().contains("Probe/PSMain"));
			CHECK(notCompiled.error().ToString().contains("pipeline 'Probe'"));

			PipelineLayoutDescription empty = MakeProbeLayout();
			empty.Entries.clear();
			const Status noEntries = ValidatePipelineLayout(empty, shaders.GetLibrary());
			REQUIRE_FALSE(noEntries.has_value());
			CHECK(noEntries.error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("PipelineFactory: creates the Triangle pipeline after the reflection check" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsPipelineSpecification specification;
			specification.Layout = TrianglePass::GetLayoutDescription();
			specification.RenderState.rasterState.setCullBack().setFrontCounterClockwise(true);
			specification.RenderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
			specification.Framebuffer.addColorFormat(nvrhi::Format::RGBA8_UNORM);
			const Result<GraphicsPipeline> pipeline = gpu.GetPipelines().CreateGraphicsPipeline(specification);
			REQUIRE_MESSAGE(pipeline.has_value(), pipeline.error().ToString());
			CHECK(pipeline->Pipeline != nullptr);
			CHECK(pipeline->BindingLayouts.size() == specification.Layout.BindingLayouts.size());

			// Specialization constants reach every stage; a stage ignores the IDs it does not declare.
			GraphicsPipelineSpecification specialized = specification;
			specialized.Specializations = { nvrhi::ShaderSpecialization::UInt32(7, 1) };
			const Result<GraphicsPipeline> specializedPipeline = gpu.GetPipelines().CreateGraphicsPipeline(specialized);
			REQUIRE_MESSAGE(specializedPipeline.has_value(), specializedPipeline.error().ToString());
			CHECK(specializedPipeline->Pipeline != nullptr);

			// A layout the reflection disagrees with is refused before anything is created.
			GraphicsPipelineSpecification broken = specification;
			broken.Layout.BindingLayouts[0].bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(7));
			const Result<GraphicsPipeline> refused = gpu.GetPipelines().CreateGraphicsPipeline(broken);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Validation);

			// Entries must fit the stages.
			GraphicsPipelineSpecification wrongEntries = specification;
			wrongEntries.Layout.Entries = { "VSMain" };
			const Result<GraphicsPipeline> rejected = gpu.GetPipelines().CreateGraphicsPipeline(wrongEntries);
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::InvalidArgument);
			GraphicsPipelineSpecification swappedEntries = specification;
			swappedEntries.Layout.Entries = { "PSMain", "VSMain" };
			swappedEntries.Layout.BindingLayouts[0].visibility = nvrhi::ShaderType::AllGraphics;
			const Result<GraphicsPipeline> swapped = gpu.GetPipelines().CreateGraphicsPipeline(swappedEntries);
			REQUIRE_FALSE(swapped.has_value());
			CHECK(swapped.error().GetCode() == ErrorCode::InvalidArgument);
			const Result<ComputePipeline> notCompute = gpu.GetPipelines().CreateComputePipeline({ .Layout = TrianglePass::GetLayoutDescription() });
			REQUIRE_FALSE(notCompute.has_value());
			CHECK(notCompute.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("PipelineFactory: pipelines created with shared binding layouts accept the same binding set" * doctest::test_suite(Test::GpuSuite))
		{
			// M8 (Docs/Decisions/0013-m8-decisions.md decision 7): the variants of a pass share their layout objects, so one
			// binding set (a material's set 1, §8.4) serves all of them; NVRHI requires the set's layout to be the pipeline's.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				const PipelineLayoutDescription layout = TrianglePass::GetLayoutDescription();
				const Result<std::vector<nvrhi::BindingLayoutHandle>> shared = gpu.GetPipelines().CreateBindingLayouts(layout);
				REQUIRE_MESSAGE(shared.has_value(), shared.error().ToString());
				REQUIRE(shared->size() == layout.BindingLayouts.size());

				GraphicsPipelineSpecification back;
				back.Layout = layout;
				back.SharedBindingLayouts = *shared;
				back.RenderState.rasterState.setCullBack().setFrontCounterClockwise(true);
				back.RenderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
				back.Framebuffer.addColorFormat(nvrhi::Format::RGBA8_UNORM);
				GraphicsPipelineSpecification none = back;
				none.RenderState.rasterState.setCullNone();
				const Result<GraphicsPipeline> first = gpu.GetPipelines().CreateGraphicsPipeline(back);
				const Result<GraphicsPipeline> second = gpu.GetPipelines().CreateGraphicsPipeline(none);
				REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
				REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
				REQUIRE(first->BindingLayouts.size() == shared->size());
				for (size_t index = 0; index < shared->size(); ++index)
				{
					CHECK(first->BindingLayouts[index] == (*shared)[index]);
					CHECK(second->BindingLayouts[index] == (*shared)[index]);
				}

				// A null entry shares nothing for its set: the pipeline creates that layout itself (pipelines whose other sets
				// differ share only the sets they agree on).
				GraphicsPipelineSpecification own = back;
				own.SharedBindingLayouts.assign(shared->size(), nullptr);
				const Result<GraphicsPipeline> ownLayouts = gpu.GetPipelines().CreateGraphicsPipeline(own);
				REQUIRE_MESSAGE(ownLayouts.has_value(), ownLayouts.error().ToString());
				REQUIRE(ownLayouts->BindingLayouts.size() == shared->size());
				for (size_t index = 0; index < shared->size(); ++index)
				{
					CHECK(ownLayouts->BindingLayouts[index] != nullptr);
					CHECK(ownLayouts->BindingLayouts[index] != (*shared)[index]);
				}

				// A shared layout that differs from the description, or the wrong number of them, is refused.
				GraphicsPipelineSpecification extra = back;
				extra.SharedBindingLayouts.push_back((*shared)[0]);
				const Result<GraphicsPipeline> tooMany = gpu.GetPipelines().CreateGraphicsPipeline(extra);
				REQUIRE_FALSE(tooMany.has_value());
				CHECK(tooMany.error().GetCode() == ErrorCode::InvalidArgument);
				nvrhi::BindingLayoutDesc otherDesc = layout.BindingLayouts[0];
				otherDesc.visibility = nvrhi::ShaderType::Pixel;
				Result<nvrhi::BindingLayoutHandle> other = device.CreateBindingLayout(otherDesc);
				REQUIRE(other.has_value());
				GraphicsPipelineSpecification mismatched = back;
				mismatched.SharedBindingLayouts = { *other };
				const Result<GraphicsPipeline> refused = gpu.GetPipelines().CreateGraphicsPipeline(mismatched);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);

				// A layout the reflection disagrees with is never created for sharing.
				PipelineLayoutDescription broken = layout;
				broken.BindingLayouts[0].bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(7));
				const Result<std::vector<nvrhi::BindingLayoutHandle>> brokenLayouts = gpu.GetPipelines().CreateBindingLayouts(broken);
				REQUIRE_FALSE(brokenLayouts.has_value());
				CHECK(brokenLayouts.error().GetCode() == ErrorCode::Validation);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("Compute: Smoke scales a buffer exactly" * doctest::test_suite(Test::GpuSuite))
		{
			// §15.3 "compute arithmetic": a power-of-two scale is exact in floating point, and saturation clamps to [0, 1].
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			for (const char* saturate : { "0", "1" })
			{
				CAPTURE(std::string(saturate));
				const Result<ComputePipeline> pipeline = gpu.GetPipelines().CreateComputePipeline({ .Layout = Test::MakeSmokeLayoutDescription(saturate) });
				REQUIRE_MESSAGE(pipeline.has_value(), pipeline.error().ToString());

				const std::array<float, 8> input = { -1.5f, -0.25f, 0.0f, 0.125f, 0.25f, 0.5f, 1.0f, 3.0f };
				nvrhi::BufferDesc valuesDesc;
				valuesDesc.byteSize = sizeof(input);
				valuesDesc.structStride = sizeof(float);
				valuesDesc.canHaveUAVs = true;
				valuesDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
				valuesDesc.keepInitialState = true;
				valuesDesc.debugName = "SmokeValues";
				Result<nvrhi::BufferHandle> values = device.CreateBuffer(valuesDesc);
				REQUIRE_MESSAGE(values.has_value(), values.error().ToString());
				nvrhi::BufferDesc constantsDesc;
				constantsDesc.byteSize = sizeof(SmokeConstants);
				constantsDesc.isConstantBuffer = true;
				constantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
				constantsDesc.keepInitialState = true;
				constantsDesc.debugName = "SmokeConstants";
				Result<nvrhi::BufferHandle> constants = device.CreateBuffer(constantsDesc);
				REQUIRE_MESSAGE(constants.has_value(), constants.error().ToString());
				nvrhi::BufferDesc readbackDesc;
				readbackDesc.byteSize = sizeof(input);
				readbackDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
				readbackDesc.initialState = nvrhi::ResourceStates::CopyDest;
				readbackDesc.keepInitialState = true;
				readbackDesc.debugName = "SmokeReadback";
				Result<nvrhi::BufferHandle> readback = device.CreateBuffer(readbackDesc);
				REQUIRE_MESSAGE(readback.has_value(), readback.error().ToString());

				nvrhi::BindingSetDesc setDesc;
				setDesc.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, *constants), nvrhi::BindingSetItem::StructuredBuffer_UAV(0, *values) };
				Result<nvrhi::BindingSetHandle> set = device.CreateBindingSet(setDesc, *pipeline->BindingLayouts[0]);
				REQUIRE_MESSAGE(set.has_value(), set.error().ToString());

				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
				const SmokeConstants smoke{ .ElementCount = static_cast<uint32_t>(input.size()), .Scale = 2.0f };
				(*commandList)->open();
				(*commandList)->writeBuffer(*constants, &smoke, sizeof(smoke));
				(*commandList)->writeBuffer(*values, input.data(), sizeof(input));
				nvrhi::ComputeState state;
				state.pipeline = pipeline->Pipeline;
				state.bindings = { *set };
				(*commandList)->setComputeState(state);
				(*commandList)->dispatch(1);
				(*commandList)->copyBuffer(*readback, 0, *values, 0, sizeof(input));
				(*commandList)->close();
				WaitForSubmission(device, device.ExecuteCommandList(**commandList), "Smoke compute test");

				std::array<float, 8> output{};
				const void* mapped = device.GetNvrhiDevice()->mapBuffer(*readback, nvrhi::CpuAccessMode::Read);
				REQUIRE(mapped != nullptr);
				std::memcpy(output.data(), mapped, sizeof(output));
				device.GetNvrhiDevice()->unmapBuffer(*readback);
				for (size_t index = 0; index < input.size(); ++index)
				{
					const float scaled = input[index] * 2.0f;
					const float expected = std::string_view(saturate) == "1" ? std::clamp(scaled, 0.0f, 1.0f) : scaled;
					CHECK(output[index] == expected);
				}
			}
		}
	}

}
