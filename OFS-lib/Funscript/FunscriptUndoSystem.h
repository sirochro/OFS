#pragma once

#include "Funscript.h"
#include <vector>

class ScriptState {
private:
	Funscript::FunscriptData data;
public:
	inline Funscript::FunscriptData& Data() { return data; }
	int32_t type;
	const char* Description() const noexcept;

	ScriptState() noexcept 
		: type(-1) {}
	ScriptState(int32_t type, const Funscript::FunscriptData& data) noexcept
		: type(type), data(data) {}
};

class FunscriptUndoSystem
{
	friend class UndoSystem;

	Funscript* script = nullptr;
	void SnapshotRedo(int32_t type) noexcept;

	std::vector<ScriptState> UndoStack;
	std::vector<ScriptState> RedoStack;

	void Snapshot(int32_t type, bool clearRedo = true) noexcept;
	bool Undo() noexcept;
	bool Redo() noexcept;
	void ClearRedo() noexcept;
public:
	// Maximum entries retained on the undo (and redo) stack. Both
	// FunscriptUndoSystem and the project-level UndoSystem read from this
	// global so the user-visible "history limit" is a single number.
	// Updated from PreferenceState::undoLimit at startup and whenever the
	// preference changes.
	static int32_t StackLimit;

	FunscriptUndoSystem(Funscript* script) : script(script) {
		FUN_ASSERT(script != nullptr, "no script");
		UndoStack.reserve(StackLimit);
		RedoStack.reserve(StackLimit);
	}

	inline bool MatchUndoTop(int32_t type) const noexcept { return !UndoEmpty() && UndoStack.back().type == type; }
	inline bool UndoEmpty() const noexcept { return UndoStack.empty(); }
	inline bool RedoEmpty() const noexcept { return RedoStack.empty(); }
};
