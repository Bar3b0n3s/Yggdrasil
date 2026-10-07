#include "Engine/Core/Notes.h"

namespace Engine {

	Notes CollectNotes(const std::vector<std::string>& lines)
	{
		Notes notes;
		for (const std::string& line : lines)
			notes = std::move(notes).WithNote(line); // Seeded defect: the builder returns notes itself, moved onto itself.
		return notes;
	}

}
