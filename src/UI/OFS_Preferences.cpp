#include "OFS_Preferences.h"
#include "OFS_Util.h"
#include "OpenFunscripter.h"
#include "OFS_Localization.h"
#include "OFS_ImGui.h"

#include "imgui.h"
#include "imgui_stdlib.h"

#include "OFS_Reflection.h"
#include "OFS_StateHandle.h"
#include "state/states/BaseOverlayState.h"
#include "FunscriptUndoSystem.h"

OFS_Preferences::OFS_Preferences() noexcept
{
	prefStateHandle = OFS_AppState<PreferenceState>::Register(PreferenceState::StateName);
	auto& state = PreferenceState::State(prefStateHandle);
    OFS_DynFontAtlas::FontOverride = state.fontOverride;
}

static void copyTranslationHelper() noexcept
{
	auto srcDir = Util::Basepath() / "data" / OFS_Translator::TranslationDir;
	auto targetDir = Util::Prefpath(OFS_Translator::TranslationDir);
	std::error_code ec;
	std::filesystem::directory_iterator langDirIt(srcDir, ec);
	for(auto& pIt : langDirIt) {
		if(pIt.path().extension() == ".csv") {
			auto targetFile = targetDir / pIt.path().filename();
			if(Util::FileExists(targetFile.u8string())) {
				// merge the two
				auto input1 = pIt.path().u8string();
				auto input2 = targetFile.u8string();
				if(OFS_Translator::MergeIntoOne(input1.c_str(), input2.c_str(), input2.c_str())) {
					std::filesystem::remove(pIt.path(), ec);
				}
			}
			else {
				std::filesystem::copy_file(pIt.path(), targetFile, ec);
				if(!ec) {
					std::filesystem::remove(pIt.path(), ec);
				}
			}
		}
	}
}

bool OFS_Preferences::ShowPreferenceWindow() noexcept
{
	bool save = false;
	if (ShowWindow)
		ImGui::OpenPopup(TR_ID("PREFERENCES", Tr::PREFERENCES));
	if (ImGui::BeginPopupModal(TR_ID("PREFERENCES", Tr::PREFERENCES), &ShowWindow, ImGuiWindowFlags_AlwaysAutoResize))
	{
		OFS_PROFILE(__FUNCTION__);
		auto& state = PreferenceState::State(prefStateHandle);

		if (ImGui::BeginChild("prefTabChild", ImVec2(600.f, 360.f))) {
			if (ImGui::BeginTabBar("##PreferenceTabs"))
			{
				if (ImGui::BeginTabItem(TR(APPLICATION)))
				{
					if (ImGui::RadioButton(TR(DARK_MODE), (int*)&state.currentTheme,
						static_cast<int32_t>(OFS_Theme::Dark))) {
						SetTheme((OFS_Theme)state.currentTheme);
						save = true;
					}
					ImGui::SameLine();
					if (ImGui::RadioButton(TR(LIGHT_MODE), (int*)&state.currentTheme,
						static_cast<int32_t>(OFS_Theme::Light))) {
						SetTheme((OFS_Theme)state.currentTheme);
						save = true;
					}

					ImGui::Separator();

					// Display mode: Fullscreen vs Windowed (radio). Used to be a
					// flat toggle in the Options menu; surfacing it here makes the
					// windowed alternative obvious and matches the rest of the
					// preference UI. Apply unconditionally on click rather than
					// only when the mode index flips -- the OFS auto-maximize at
					// launch leaves the Status flag unset (= "Windowed" per the
					// radio) while the window is actually display-sized, so the
					// only way to force a real windowed layout is to re-trigger
					// SetFullscreen even when the user clicked the already-
					// selected radio.
					{
						auto app = OpenFunscripter::ptr;
						const bool isFullscreen = (app->Status & OFS_Status::OFS_Fullscreen) != 0;
						int mode = isFullscreen ? 0 : 1;
						ImGui::Text("Display mode:");
						ImGui::SameLine();
						bool clickFullscreen = ImGui::RadioButton("Fullscreen##displayMode", &mode, 0);
						ImGui::SameLine();
						bool clickWindowed   = ImGui::RadioButton("Windowed##displayMode",   &mode, 1);
						if (clickFullscreen || clickWindowed) {
							const bool wantFullscreen = (mode == 0);
							app->SetFullscreen(wantFullscreen);
							app->Status = wantFullscreen
								? (uint8_t)(app->Status | OFS_Status::OFS_Fullscreen)
								: (uint8_t)(app->Status & ~OFS_Status::OFS_Fullscreen);
						}
					}

					ImGui::Separator();

					ImGui::TextWrapped(TR(PREFERENCES_TXT));

					// Undo history limit: max snapshots retained per script.
					// Pushed through to FunscriptUndoSystem::StackLimit so the
					// limit takes effect immediately, not just on restart.
					if (ImGui::InputInt("Undo history limit##undoLimit", &state.undoLimit, 1, 10)) {
						state.undoLimit = Util::Clamp(state.undoLimit, 10, 10000);
						FunscriptUndoSystem::StackLimit = state.undoLimit;
						save = true;
					}
					OFS::Tooltip("Maximum number of undo states retained (default 100). Higher = more memory used per script.");

					if (ImGui::InputInt(TR(FRAME_LIMIT), &state.framerateLimit, 1, 10)) {
						state.framerateLimit = Util::Clamp(state.framerateLimit, 60, 300);
						save = true;
					}
					OFS::Tooltip(TR(FRAME_LIMIT_TOOLTIP));
					ImGui::SameLine();
					if (ImGui::Checkbox(TR(VSYNC), (bool*)&state.vsync)) {
						state.vsync = Util::Clamp(state.vsync, 0, 1); // just in case...
						SDL_GL_SetSwapInterval(state.vsync);
						save = true;
					}
					OFS::Tooltip(TR(VSYNC_TOOLTIP));
					ImGui::Separator();
					ImGui::InputText(TR(FONT), state.fontOverride.empty() ? (char*)TR(DEFAULT_FONT) : (char*)state.fontOverride.c_str(),
						state.fontOverride.size(), ImGuiInputTextFlags_ReadOnly);
					ImGui::SameLine();
					if (ImGui::Button(TR(CHANGE))) {
						Util::OpenFileDialog(TR(CHOOSE_FONT), "",
							[&](auto& result) {
								if (result.files.size() > 0) {
									state.fontOverride = result.files.back();
									OpenFunscripter::ptr->LoadOverrideFont(state.fontOverride);
									save = true;
								}
							}, false, { "*.ttf", "*.otf" }, "Fonts (*.ttf, *.otf)");
					}
					ImGui::SameLine();
					if (ImGui::Button(TR(CLEAR))) {
						state.fontOverride = "";
						EV::Enqueue<OFS_DeferEvent>([]()
						{
							// fonts can't be updated during a frame
							// this updates the font during event processing
							// which is not during the frame
							auto app = OpenFunscripter::ptr;
							app->LoadOverrideFont("");
						});
					}

					if (ImGui::InputInt(TR(FONT_SIZE), (int*)&state.defaultFontSize, 1, 1)) {
						state.defaultFontSize = Util::Clamp(state.defaultFontSize, 8, 64);
						EV::Enqueue<OFS_DeferEvent>([stateHandle = prefStateHandle]() {
							// fonts can't be updated during a frame
							// this updates the font during event processing
							// which is not during the frame
							auto& state = PreferenceState::State(stateHandle);
							auto app = OpenFunscripter::ptr;
							app->LoadOverrideFont(state.fontOverride);
						});
						save = true;
					}
					if(ImGui::BeginCombo(TR_ID("LANGUAGE", Tr::LANGUAGE), state.languageCsv.empty() ? "English" : state.languageCsv.c_str()))
					{
						for(auto& file : translationFiles) {
							if(ImGui::Selectable(file.c_str(), file == state.languageCsv)) {
								if(OFS_Translator::ptr->LoadTranslation(file.c_str())) {
									state.languageCsv = file;
									OFS_DynFontAtlas::AddTranslationText();
								}
							}
						}
						ImGui::EndCombo();
					}
					if(ImGui::IsItemClicked(ImGuiMouseButton_Left))	{
						copyTranslationHelper();
						translationFiles.clear();
						std::error_code ec;
						std::filesystem::directory_iterator dirIt(Util::Prefpath(OFS_Translator::TranslationDir), ec);
						for (auto& pIt : dirIt) {
							if(pIt.path().extension() == ".csv") {
								translationFiles.emplace_back(pIt.path().filename().u8string());
							}
						}
					}
					ImGui::SameLine();
					if(ImGui::Button(TR(RESET))) {
						state.languageCsv = std::string();
						OFS_Translator::ptr->LoadDefaults();
					}
					ImGui::SameLine();
					if(ImGui::Button(FMT("%s###DIRECTORY_TRANSLATION", ICON_FOLDER_OPEN)))
					{
						Util::OpenFileExplorer(Util::Prefpath(OFS_Translator::TranslationDir));
					}
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem(TR(VIDEOPLAYER))) {
					if (ImGui::Checkbox(TR(FORCE_HW_DECODING), &state.forceHwDecoding)) {
						save = true;
					}
					OFS::Tooltip(TR(FORCE_HW_DECODING_TOOLTIP));
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem(TR(SCRIPTING)))
				{
					auto& overlayState = BaseOverlay::State();
					if(ImGui::Checkbox(TR_ID("HighlightEnable", Tr::ENABLE_MAX_SPEED_HIGHLIGHT), &overlayState.ShowMaxSpeedHighlight)) {
						save = true;
					}
					ImGui::BeginDisabled(!overlayState.ShowMaxSpeedHighlight);
					if(ImGui::InputFloat(TR(HIGHLIGHT_TRESHOLD), &overlayState.MaxSpeedPerSecond)) {
						save = true;
					}
					ImGui::ColorEdit3(TR_ID("HighlightColor", Tr::COLOR), &overlayState.MaxSpeedColor.Value.x, ImGuiColorEditFlags_None);
					if(ImGui::IsItemDeactivatedAfterEdit()) {
						save = true;
					}
					ImGui::EndDisabled();
					
					ImGui::Separator();
					if (ImGui::InputInt(TR(FAST_FRAME_STEP), &state.fastStepAmount, 1, 1)) {
						save = true;
						state.fastStepAmount = Util::Clamp<int32_t>(state.fastStepAmount, 2, 30);
					}
					OFS::Tooltip(TR(FAST_FRAME_STEP_TOOLTIP));
					ImGui::Separator();
					if (ImGui::Checkbox(TR(SHOW_METADATA_DIALOG_ON_NEW_PROJECT), &state.showMetaOnNew)) {
						save = true;
					}
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
			ImGui::EndChild();
		}
		ImGui::EndPopup();
	}
	return save;
}

void OFS_Preferences::SetTheme(OFS_Theme theme) noexcept
{
	auto& style = ImGui::GetStyle();
	auto& io = ImGui::GetIO();

	switch (theme) {
		case OFS_Theme::Dark: {
			ImGui::StyleColorsDark(&style);
			break;
		}
		case OFS_Theme::Light: {
			ImGui::StyleColorsLight(&style);
			break;
		}
	}

	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
		//style.WindowRounding = 0.0f;
		style.WindowRounding = 6.f;
		style.Colors[ImGuiCol_WindowBg].w = 1.f;
		style.Colors[ImGuiCol_PopupBg].w = 1.f;
	}
}
