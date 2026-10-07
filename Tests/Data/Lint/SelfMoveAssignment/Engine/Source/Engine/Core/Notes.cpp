#include "Engine/Core/Notes.h"

namespace Engine {

	Notes CollectNotes(const std::vector<std::string>& lines)
	{
		Notes notes;
		for (const std::string& line : lines)
			notes = std::move(notes).WithNote(line); // Seeded defect: the builder returns notes itself, moved onto itself.
		return notes;
	}

	// Controls: lambda init-captures make new closure members from the outer variables; they are not self-moves.
	size_t CountLater(Notes notes, std::vector<std::string> extra)
	{
		const auto count = [notes = std::move(notes), extra = std::move(extra)]()
		{
			return notes.GetCount() + extra.size();
		};
		return count();
	}

}
