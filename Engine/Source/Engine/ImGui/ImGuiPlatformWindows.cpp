#include "EnginePCH.h"
#include "Engine/ImGui/ImGuiPlatformWindows.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/ImGui/ImGuiRenderer.h"

// Dear ImGui's platform windows (Docs/Decisions/0035-m24-contract.md decisions 18 and 19): stream V implements them.

namespace Engine {

	Result<Scope<IImGuiPlatformWindowTarget>> CreateSwapchainPlatformWindowTarget(GraphicsDevice& /*device*/,
		const ImGuiPlatformWindowTargetRequest& /*request*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CreateSwapchainPlatformWindowTarget (Docs/Decisions/0035-m24-contract.md decision 18, stream V)");
	}

	struct ImGuiPlatformWindows::State
	{
	};

	ImGuiPlatformWindows::ImGuiPlatformWindows(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	ImGuiPlatformWindows::~ImGuiPlatformWindows() = default;

	Result<Scope<ImGuiPlatformWindows>> ImGuiPlatformWindows::Create(GraphicsDevice& /*device*/, ImGuiRenderer& /*renderer*/,
		const ImGuiPlatformWindowsSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ImGuiPlatformWindows::Create (Docs/Decisions/0035-m24-contract.md decision 18, stream V)");
	}

	Status ImGuiPlatformWindows::Render(uint32_t /*frameSlot*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ImGuiPlatformWindows::Render (Docs/Decisions/0035-m24-contract.md decision 18, stream V)");
	}

	size_t ImGuiPlatformWindows::GetWindowCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
