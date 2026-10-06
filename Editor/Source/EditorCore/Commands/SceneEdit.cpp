#include "EditorPCH.h"
#include "EditorCore/Commands/SceneEdit.h"

#include "EditorCore/EditorContext.h"

// M4 contract stub (Roadmap rule 3): stream A (commands) implements tracked scene edits.

namespace Engine {

	SceneEdit::SceneEdit(EditorContext& context, std::string label, std::string mergeKey)
		: m_Context(&context), m_Label(std::move(label)), m_MergeKey(std::move(mergeKey))
	{
		ENGINE_CONTRACT_STUB();
	}

	SceneEdit::~SceneEdit()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<uint64_t> SceneEdit::Commit()
	{
		ENGINE_CONTRACT_STUB();
		m_IsActive = false;
		return MakeError(ErrorCode::Unsupported, "SceneEdit::Commit is an M4 contract stub");
	}

	void SceneEdit::Cancel()
	{
		ENGINE_CONTRACT_STUB();
		m_IsActive = false;
	}

	Scene& SceneEdit::GetScene() const
	{
		return m_Context->GetScene();
	}

}
