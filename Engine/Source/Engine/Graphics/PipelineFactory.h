#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/ShaderLibrary.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Pipelines checked against shader reflection (Architecture §8.4, §8.12). Every C++ BindingLayoutDesc is compared with the
// reflection JSON of the pipeline's shaders (set, binding, resource type, array size, constant-buffer and push-constant
// size, and the storage format of every storage image); a mismatch is a load error, never a silent binding bug. The check
// needs no GPU: "Shaders: LayoutsMatchReflection" runs ValidatePipelineLayout over the layout description of every engine
// pipeline in the CPU-only unit suite, and PipelineFactory runs it again before every creation.

namespace Engine {

	class GraphicsDevice;

	// The format a pass creates for one of its storage images: the binding's reflected [vk::image_format] must match it
	// (§8.4: "checks the reflected image format against the texture format the C++ pass creates").
	struct StorageImageFormat
	{
		uint32_t Set = 0;      // register space
		uint32_t Register = 0; // the u register (BindingLayoutItem::slot)
		nvrhi::Format Format = nvrhi::Format::UNKNOWN;
	};

	// The byte size of the buffer a pass binds to one of its constant buffers, sizeof the C++ struct it fills: the size of
	// the binding's reflected element struct must equal it (§8.12: "constant-buffer size"), so a pass that binds the wrong
	// struct, or a buffer of the wrong size, to a register fails the check. "Shaders: SharedStructsMatchReflection"
	// checks the members of each shared struct; this ties the struct to the register.
	struct ConstantBufferSize
	{
		uint32_t Set = 0;      // register space
		uint32_t Register = 0; // the b register (BindingLayoutItem::slot)
		uint32_t ByteSize = 0;
	};

	// Everything about a pipeline that the reflection check needs, available without a device. Each pass provides one
	// through a static function (TrianglePass::GetLayoutDescription, ImGuiRenderer::GetLayoutDescription), so the CPU test
	// can check every pipeline.
	struct PipelineLayoutDescription
	{
		std::string Name{};                 // for messages: "Triangle"
		std::string Program{};              // a program of Shaders.json
		std::vector<std::string> Entries{}; // the entry points of its stages: vertex then fragment, or the compute entry
		std::vector<ShaderDefine> Permutation{};
		// One layout per descriptor set the shaders use, each with registerSpace = its set and registerSpaceIsDescriptorSet =
		// true (§8.4). Push constants are a BindingLayoutItem::PushConstants of the set-0 layout.
		std::vector<nvrhi::BindingLayoutDesc> BindingLayouts{};
		std::vector<StorageImageFormat> StorageImages{};
		// One entry per constant buffer of the layouts (push constants carry their size in their layout item).
		std::vector<ConstantBufferSize> ConstantBuffers{};
	};

	// The reflection check (CPU only): every binding any of the entries uses must be declared by the layout of its set with
	// the same register class, resource shape, shift-adjusted binding number (the item's slot plus the layout's
	// bindingOffsets) and array size, and must be visible to every stage that uses it; the entries must agree on every
	// binding they share, constant-buffer sizes included; every constant buffer must have a ConstantBuffers entry whose
	// ByteSize equals the reflected size of its element struct, and no ConstantBuffers entry may be left over; the push
	// constants are one PushConstants item of the reflected size, at most 128 bytes, matched by kind rather than by slot;
	// every layout item must exist in some entry's reflection; every storage image must declare its [vk::image_format]
	// and have a StorageImages entry with that format, and no StorageImages entry may be left over; unbounded arrays,
	// duplicate sets and duplicate bindings are rejected; and every layout must set registerSpaceIsDescriptorSet. Errors:
	// Validation listing every mismatch as an ErrorIssue (JSON pointer "/<set>/<binding name>") with the pipeline's name
	// as context; those of ShaderLibrary::GetReflection.
	[[nodiscard]] Status ValidatePipelineLayout(const PipelineLayoutDescription& description, ShaderLibrary& shaders);

	struct GraphicsPipelineSpecification
	{
		PipelineLayoutDescription Layout{}; // Entries: { vertex entry, fragment entry }
		// Vulkan specialization constants for small pipeline toggles (§8.5: the debug views), applied to every stage's shader
		// through GraphicsDevice::CreateShaderSpecialization; a stage ignores the constant IDs it does not declare. Empty:
		// the shaders as compiled.
		std::vector<nvrhi::ShaderSpecialization> Specializations{};
		// M8: binding layouts the pipeline uses instead of creating its own, one entry per set of Layout.BindingLayouts in set
		// order: a layout made by PipelineFactory::CreateBindingLayouts for an equal desc of that set, or null for a set the
		// pipeline creates its own layout for. Pipelines created with the same handle for a set accept the same binding sets
		// for it (NVRHI requires a binding set made for the pipeline's own layout objects), so pipelines whose other sets
		// differ still share one set per resource (§8.4: "one cached BindingSet per (material, version)": the material's set
		// 1 serves the forward variants, the Mask prepass and M9's shadow casters alike, whatever their set 0). Empty: the
		// pipeline creates every layout.
		std::vector<nvrhi::BindingLayoutHandle> SharedBindingLayouts{};
		std::vector<nvrhi::VertexAttributeDesc> VertexAttributes{}; // empty: no input layout (vertices from the vertex index)
		nvrhi::PrimitiveType Primitive = nvrhi::PrimitiveType::TriangleList;
		// Rasterizer, blend and depth-stencil state. Engine pipelines set rasterState.frontCounterClockwise = true, because
		// glTF front faces are counter-clockwise and clip-space +Y is up: NVRHI's Vulkan viewport performs the Vulkan Y flip
		// (§8.3).
		nvrhi::RenderState RenderState{};
		// The target's formats and sample count.
		nvrhi::FramebufferInfo Framebuffer{};
	};

	struct ComputePipelineSpecification
	{
		PipelineLayoutDescription Layout{}; // Entries: { compute entry }
		// As GraphicsPipelineSpecification::Specializations, for the compute shader.
		std::vector<nvrhi::ShaderSpecialization> Specializations{};
		// As GraphicsPipelineSpecification::SharedBindingLayouts (M8).
		std::vector<nvrhi::BindingLayoutHandle> SharedBindingLayouts{};
	};

	// A created pipeline and the binding layouts it was created with (one per set, in set order), which binding sets for it
	// must use.
	struct GraphicsPipeline
	{
		nvrhi::GraphicsPipelineHandle Pipeline{};
		std::vector<nvrhi::BindingLayoutHandle> BindingLayouts{};
	};

	struct ComputePipeline
	{
		nvrhi::ComputePipelineHandle Pipeline{};
		std::vector<nvrhi::BindingLayoutHandle> BindingLayouts{};
	};

	// Not copyable or movable; main thread only (§4.11). The passes' pipelines are created at startup (§8.12); ImGui's are
	// created per target format on first use (ADR 0009 decision 25).
	class PipelineFactory
	{
	public:
		// `device` and `shaders` are documented back-references that must outlive the factory.
		PipelineFactory(GraphicsDevice& device, ShaderLibrary& shaders);
		~PipelineFactory();

		PipelineFactory(const PipelineFactory&) = delete;
		PipelineFactory& operator=(const PipelineFactory&) = delete;

		// ValidatePipelineLayout, then the shaders (ShaderLibrary::Get, specialized with GraphicsDevice::
		// CreateShaderSpecialization when Specializations is not empty), the binding layouts, the input layout and the
		// pipeline through the GraphicsDevice wrappers. Errors: those of ValidatePipelineLayout and ShaderLibrary::Get;
		// InvalidArgument for a wrong number of entries or an entry whose stage does not fit its position; Gpu when a
		// creation fails (callers creating at startup treat it as FatalError(OutOfMemory), §8.14 item 7).
		// With SharedBindingLayouts, the pipeline uses the non-null entries (GraphicsPipeline::BindingLayouts returns them)
		// and creates the layouts of the sets whose entry is null; InvalidArgument when their number differs from
		// Layout.BindingLayouts' or a non-null entry's desc (visibility, register space, registerSpaceIsDescriptorSet, binding
		// offsets and items) differs from its set's in set order.
		[[nodiscard]] Result<GraphicsPipeline> CreateGraphicsPipeline(const GraphicsPipelineSpecification& specification);
		[[nodiscard]] Result<ComputePipeline> CreateComputePipeline(const ComputePipelineSpecification& specification);

		// M8: the binding layouts of `layout`, one per set in set order, for SharedBindingLayouts: ValidatePipelineLayout first
		// (so a layout that does not match its shaders is never shared), then GraphicsDevice::CreateBindingLayout per set.
		// Errors: those of ValidatePipelineLayout; Gpu when a creation fails.
		[[nodiscard]] Result<std::vector<nvrhi::BindingLayoutHandle>> CreateBindingLayouts(const PipelineLayoutDescription& layout);

		[[nodiscard]] ShaderLibrary& GetShaderLibrary() { return *m_Shaders; }
	private:
		ShaderLibrary* m_Shaders = nullptr; // documented back-reference
		GraphicsDevice* m_Device = nullptr; // documented back-reference
	};

}
