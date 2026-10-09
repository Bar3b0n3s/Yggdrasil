#include "EditorPCH.h"
#include "EditorCore/Automation/ScreenshotMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodContext.h"
#include "Engine/Automation/Protocol/PendingOperation.h"

namespace Engine {

	namespace Utils {

		class FreshEditorScreenshot final : public PendingOperation
		{
		public:
			FreshEditorScreenshot(uint64_t admittedFrame, const EditorScreenshotParams& params)
				: m_AdmittedFrame(admittedFrame), m_Params(params)
			{
			}

			std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				if (m_Cancelled)
					return MakeError(ErrorCode::Cancelled, "editor screenshot was cancelled");
				EditorMethodContext& editorContext = static_cast<EditorMethodContext&>(context);
				const ScreenshotCaptures& captures = editorContext.GetServer().GetSpecification().Screenshots;
				if (!captures.EditorUi || !captures.CompletedUiFrame || !captures.RequestUiFrame)
					return MakeError(ErrorCode::Unsupported, "editor screenshot no longer has a UI frame provider");
				const uint64_t completed = captures.CompletedUiFrame();
				if (completed < m_AdmittedFrame)
					return MakeError(ErrorCode::Cancelled, "editor UI frame sequence changed while a screenshot was pending");
				if (completed == m_AdmittedFrame)
					return std::nullopt;
				const Result<EditorScreenshotResult> result = Automation::EditorScreenshot(editorContext, m_Params);
				if (!result)
					return std::unexpected(result.error());
				return context.SerializeResult(*result);
			}

			void Cancel(MethodContext& /*context*/) override
			{
				m_Cancelled = true;
			}

			std::string GetPhase() const override { return "Waiting for a fresh editor UI frame"; }
		private:
			uint64_t m_AdmittedFrame = 0;
			EditorScreenshotParams m_Params{};
			bool m_Cancelled = false;
		};

	}

	Result<Scope<PendingOperation>> Automation::BeginEditorScreenshot(EditorMethodContext& context, const EditorScreenshotParams& params)
	{
		if (params.MaxDimension == 0 || params.MaxDimension > 8192)
			return std::unexpected(Error(ErrorCode::InvalidArgument, "maxDimension must be from 1 to 8192").WithLocation({ .File = {}, .JsonPointer = "/maxDimension", .Entity = {} }));
		const ScreenshotCaptures& captures = context.GetServer().GetSpecification().Screenshots;
		if (!captures.EditorUi || !captures.CompletedUiFrame || !captures.RequestUiFrame)
			return MakeError(ErrorCode::Unsupported, "editor.screenshot needs a renderer and a fresh UI frame provider");
		const uint64_t admittedFrame = captures.CompletedUiFrame();
		Scope<PendingOperation> operation = CreateScope<Utils::FreshEditorScreenshot>(admittedFrame, params);
		captures.RequestUiFrame();
		return operation;
	}

}
