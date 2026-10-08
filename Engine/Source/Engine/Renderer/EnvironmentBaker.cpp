#include "EnginePCH.h"
#include "Engine/Renderer/EnvironmentBaker.h"

#include "Engine/Core/Error.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include <array>
#include <format>
#include <string>
#include <string_view>

// The contract stub (Docs/Decisions/0013-m8-decisions.md decision 3): the pipelines of the stub EnvironmentBake program (no
// bindings), so the program is covered by "Shaders: LayoutsMatchReflection" and the pipeline count matches the
// descriptions; every bake is Unsupported. Stream B replaces the file.

namespace Engine {

	namespace Utils {

		static std::vector<PipelineLayoutDescription> MakeEnvironmentBakeStubDescriptions()
		{
			std::vector<PipelineLayoutDescription> descriptions;
			for (const std::string_view entry : std::array<std::string_view, 5>{ "CSEquirectToCube", "CSDownsampleCube", "CSPrefilterSpecular",
					 "CSProjectIrradiance", "CSReduceIrradiance" })
			{
				descriptions.push_back({ .Name = std::format("EnvironmentBake.{}", entry), .Program = "EnvironmentBake", .Entries = { std::string(entry) } });
			}
			return descriptions;
		}

	}

	struct EnvironmentBaker::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		std::vector<ComputePipeline> Pipelines{};
	};

	EnvironmentBaker::EnvironmentBaker(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	EnvironmentBaker::~EnvironmentBaker() = default;

	Result<Scope<EnvironmentBaker>> EnvironmentBaker::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		ENGINE_CONTRACT_STUB();
		Scope<EnvironmentBaker> baker = CreateScope<EnvironmentBaker>(ConstructionKey());
		State& state = *baker->m_State;
		state.Device = &device;
		for (PipelineLayoutDescription& description : Utils::MakeEnvironmentBakeStubDescriptions())
		{
			ENGINE_TRY_ASSIGN(ComputePipeline pipeline, pipelines.CreateComputePipeline({ .Layout = std::move(description) }));
			state.Pipelines.push_back(std::move(pipeline));
		}
		return baker;
	}

	std::vector<PipelineLayoutDescription> EnvironmentBaker::GetLayoutDescriptions()
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeEnvironmentBakeStubDescriptions();
	}

	uint32_t EnvironmentBaker::GetPipelineCount() const
	{
		return static_cast<uint32_t>(m_State->Pipelines.size());
	}

	Result<EnvironmentData> EnvironmentBaker::Bake(const EnvironmentBakeInput& input)
	{
		ENGINE_CONTRACT_STUB();
		return BakeWithOptions(input, EnvironmentBakeOptions{});
	}

	Result<EnvironmentData> EnvironmentBaker::BakeWithOptions(const EnvironmentBakeInput& /*input*/, const EnvironmentBakeOptions& /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "the environment baker is not implemented yet (M8 stream B)")
				.WithHint("start the editor with a GPU once to bake this environment"));
	}

}
