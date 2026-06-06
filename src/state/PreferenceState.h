#pragma once
#include <string>
#include "OFS_StateHandle.h"

enum class OFS_Theme : int32_t
{
	Dark,
	Light
};

struct PreferenceState 
{
	static constexpr auto StateName = "Preferences";

	std::string languageCsv;
	std::string fontOverride;

	int32_t defaultFontSize = 18;
	int32_t currentTheme = static_cast<int32_t>(OFS_Theme::Dark);

	int32_t fastStepAmount = 6;

	int32_t	vsync = 0;
	int32_t framerateLimit = 150;

	bool forceHwDecoding = false;
	bool showMetaOnNew = true;

	int32_t undoLimit = 100;

	// Persisted window geometry (Windowed mode). -1 sentinels mean
	// "no value saved yet" -- on first launch we fall back to the
	// hard-coded launch defaults.
	int32_t windowWidth = -1;
	int32_t windowHeight = -1;
	int32_t windowX = -1;
	int32_t windowY = -1;
	bool windowMaximized = false;
	bool windowFullscreen = false;

	static inline PreferenceState& State(uint32_t stateHandle) noexcept {
		return OFS_AppState<PreferenceState>(stateHandle).Get();
	}
};

REFL_TYPE(PreferenceState)
	REFL_FIELD(languageCsv)
	REFL_FIELD(fontOverride)
	REFL_FIELD(defaultFontSize)
	REFL_FIELD(currentTheme)
	REFL_FIELD(fastStepAmount)
	REFL_FIELD(vsync)
	REFL_FIELD(framerateLimit)
	REFL_FIELD(forceHwDecoding)
	REFL_FIELD(showMetaOnNew)
	REFL_FIELD(undoLimit)
	REFL_FIELD(windowWidth)
	REFL_FIELD(windowHeight)
	REFL_FIELD(windowX)
	REFL_FIELD(windowY)
	REFL_FIELD(windowMaximized)
	REFL_FIELD(windowFullscreen)
REFL_END