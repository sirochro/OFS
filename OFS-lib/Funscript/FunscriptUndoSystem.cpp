#include "FunscriptUndoSystem.h"

int32_t FunscriptUndoSystem::StackLimit = 100;

void FunscriptUndoSystem::ClearRedo() noexcept
{
	RedoStack.clear();
}

void FunscriptUndoSystem::SnapshotRedo(int32_t type) noexcept
{
	RedoStack.emplace_back(std::move(ScriptState(type, script->Data())));
	// Cap the redo stack too, otherwise an aggressive undo run after many
	// edits could leak memory the user can never reach again.
	if (StackLimit > 0 && (int32_t)RedoStack.size() > StackLimit) {
		RedoStack.erase(RedoStack.begin(), RedoStack.begin() + (RedoStack.size() - StackLimit));
	}
}

void FunscriptUndoSystem::Snapshot(int32_t type, bool clearRedo) noexcept
{
	OFS_PROFILE(__FUNCTION__);
	UndoStack.emplace_back(std::move(ScriptState(type, script->Data())));

	// Enforce the user-configured history limit. Drop the oldest entries
	// first so the most recent N states are always preserved.
	if (StackLimit > 0 && (int32_t)UndoStack.size() > StackLimit) {
		UndoStack.erase(UndoStack.begin(), UndoStack.begin() + (UndoStack.size() - StackLimit));
	}

	// redo gets cleared after every snapshot
	if (clearRedo)
		ClearRedo();
}

bool FunscriptUndoSystem::Undo() noexcept
{
	if (UndoStack.empty()) return false;
	OFS_PROFILE(__FUNCTION__);
	SnapshotRedo(UndoStack.back().type); // copy data to redo
	script->Rollback(std::move(UndoStack.back().Data())); // move data
	UndoStack.pop_back(); // pop of the stack
	return true;
}

bool FunscriptUndoSystem::Redo() noexcept
{
	if (RedoStack.empty()) return false;
	OFS_PROFILE(__FUNCTION__);
	Snapshot(RedoStack.back().type, false); // copy data to undo
	script->Rollback(std::move(RedoStack.back().Data())); // move data
	RedoStack.pop_back(); // pop of the stack
	return true;
}