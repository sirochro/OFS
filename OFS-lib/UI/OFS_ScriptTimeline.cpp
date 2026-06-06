#include "OFS_ScriptTimeline.h"
#include "OFS_Profiling.h"
#include "OFS_VideoplayerEvents.h"

#include "stb_sprintf.h"

#include "imgui.h"
#include "imgui_stdlib.h"

#include "OFS_ImGui.h"
#include "OFS_Shader.h"
#include "OFS_GL.h"
#include "OFS_EventSystem.h"

#include "state/states/BaseOverlayState.h"
#include "state/states/WaveformState.h"

#include "SDL_events.h"
#include "SDL_timer.h"

inline static FunscriptAction getActionForPoint(const OverlayDrawingCtx& ctx, ImVec2 point) noexcept
{
	auto localCoord = point - ctx.canvasPos;
	float relativeX = localCoord.x / ctx.canvasSize.x;
	float relativeY = localCoord.y / ctx.canvasSize.y;
	float atTime = ctx.offsetTime + (relativeX * ctx.visibleTime);
	float pos = Util::Clamp<float>(100.f - (relativeY * 100.f), 0.f, 100.f);
	return FunscriptAction(atTime, pos);
}

void ScriptTimeline::updateSelection(const OverlayDrawingCtx& ctx, bool clear) noexcept
{
	OFS_PROFILE(__FUNCTION__);
	float relSel1 = (absSel1 - ctx.offsetTime) / visibleTime;
	float min = std::min(relSel1, relSel2);
	float max = std::max(relSel1, relSel2);

	float startTime = ctx.offsetTime + (visibleTime * min);
	float endTime = ctx.offsetTime + (visibleTime * max);

	float selectionInterval = endTime - startTime;
	// Tiny selections are ignored this is a bit arbitrary.
	// It's supposed to prevent accidentally clearing the selection.
	if(selectionInterval <= 0.008f) // 8ms
		return;

	// Toggle (Shift+drag) inherently means "do not clear" so each covered
	// action XORs against the existing selection — SelectRect/SelectTime
	// already use ToggleSelection internally.
	const bool effectiveClear = clear && !IsToggleSelecting;

	if (IsRectSelecting) {
		float yMinRel = std::min(relSel1Y, relSel2Y);
		float yMaxRel = std::max(relSel1Y, relSel2Y);
		int32_t maxPos = (int32_t)std::round((1.f - yMinRel) * 100.f);
		int32_t minPos = (int32_t)std::round((1.f - yMaxRel) * 100.f);
		if (minPos < 0) minPos = 0;
		if (maxPos > 100) maxPos = 100;
		EV::Enqueue<FunscriptShouldSelectRectEvent>(startTime, endTime, minPos, maxPos, effectiveClear, ctx.ActiveScript());
		return;
	}

	EV::Enqueue<FunscriptShouldSelectTimeEvent>(startTime, endTime, effectiveClear, ctx.ActiveScript());
}

void ScriptTimeline::FfmpegAudioProcessingFinished(const WaveformProcessingFinishedEvent* ev) noexcept
{
	ShowAudioWaveform = true;
	// Update cache
	auto& waveCache = WaveformState::StaticStateSlow();
	waveCache.Filename = videoPath;
	waveCache.SetSamples(Wave.data.Samples());
	LOG_INFO("Audio processing complete.");
}

void ScriptTimeline::Init()
{
	overlayStateHandle = BaseOverlayState::RegisterStatic();

	EV::Queue().appendListener(SDL_MOUSEWHEEL,
		OFS_SDL_Event::HandleEvent(EVENT_SYSTEM_BIND(this, &ScriptTimeline::mouseScroll)));
	EV::Queue().appendListener(WaveformProcessingFinishedEvent::EventType,
		WaveformProcessingFinishedEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &ScriptTimeline::FfmpegAudioProcessingFinished)));
	EV::Queue().appendListener(VideoLoadedEvent::EventType,
		VideoLoadedEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &ScriptTimeline::videoLoaded)));

	Wave.Init();
}

void ScriptTimeline::mouseScroll(const OFS_SDL_Event* ev) noexcept
{
	OFS_PROFILE(__FUNCTION__);
	auto& wheel = ev->sdl.wheel;
	constexpr float scrollPercent = 0.10f;
	if (PositionsItemHovered) {
		previousVisibleTime = visibleTime;
		nextVisisbleTime *= 1 + (scrollPercent * -wheel.y);
		nextVisisbleTime = Util::Clamp(nextVisisbleTime, MinVisibleTime, MaxVisibleTime);
		visibleTimeUpdate = SDL_GetTicks();
	}
}

inline static float easeOutExpo(float x) noexcept {
	return x >= 1.f ? 1.f : 1.f - powf(2, -10 * x);
}

void ScriptTimeline::Update() noexcept
{
	auto timePassed = Util::Clamp((SDL_GetTicks() - visibleTimeUpdate) / 150.f, 0.f, 1.f);
	timePassed = easeOutExpo(timePassed);
	visibleTime = Util::Lerp(previousVisibleTime, nextVisisbleTime, timePassed);
}

void ScriptTimeline::videoLoaded(const VideoLoadedEvent* ev) noexcept
{
	if(ev->playerType != VideoplayerType::Main) return;
	videoPath = ev->videoPath;
	auto& waveCache = WaveformState::StaticStateSlow();
	auto samples = waveCache.GetSamples();
	if(waveCache.Filename == videoPath && !samples.empty())
	{
		Wave.data.SetSamples(std::move(samples));
		ShowAudioWaveform = true;
	}
	else 
	{
		ClearAudioWaveform();
	}
}

void ScriptTimeline::handleSelectionScrolling(const OverlayDrawingCtx& ctx) noexcept
{
	constexpr float seekBorderMargin = 0.03f;
	constexpr float scrollSpeed = 80.f;
	if(relSel2 < seekBorderMargin || relSel2 > (1.f - seekBorderMargin)) {
		float seekToTime = ctx.offsetTime + (visibleTime / 2.f);
		seekToTime = Util::Max(0.f, seekToTime);
		float relSeek = (relSel2 < seekBorderMargin) 
			? -(seekBorderMargin - relSel2) 
			: relSel2 - (1.f - seekBorderMargin);

		relSeek *= ImGui::GetIO().DeltaTime * scrollSpeed;

		float seek = visibleTime * relSeek; 
		seekToTime += seek;
		EV::Enqueue<ShouldSetTimeEvent>(seekToTime);
	}
}

void ScriptTimeline::handleTimelineHover(const OverlayDrawingCtx& ctx) noexcept
{
	if(IsSelecting)
	{
		// Update selection
		auto mp = ImGui::GetMousePos();
		relSel2 = (mp.x - ctx.canvasPos.x) / ctx.canvasSize.x;
		relSel2 = Util::Clamp(relSel2, 0.f, 1.f);
		if (IsRectSelecting) {
			relSel2Y = (mp.y - ctx.canvasPos.y) / ctx.canvasSize.y;
			relSel2Y = Util::Clamp(relSel2Y, 0.f, 1.f);
		}
	}
	else if(ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
	{
		// middle mouse panning
		auto delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
		float timeDelta = (-delta.x / ctx.canvasSize.x) * ctx.visibleTime;
		float seekToTime = (ctx.offsetTime + (ctx.visibleTime/2.f)) + timeDelta;
		EV::Enqueue<ShouldSetTimeEvent>(seekToTime);
		ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
	}
}

bool ScriptTimeline::handleTimelineClicks(const OverlayDrawingCtx& ctx) noexcept
{
	const bool shiftHeld = ImGui::IsKeyDown(ImGuiMod_Shift);
	auto mousePos = ImGui::GetMousePos();

	auto leftMouseClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
	if(ctx.activeScriptIdx == ctx.drawingScriptIdx && BaseOverlay::PointSize >= 4.f)
	{
		auto startIt = ctx.DrawingScript()->Actions().begin() + ctx.actionFromIdx;
		auto endIt = ctx.DrawingScript()->Actions().begin() + ctx.actionToIdx;
		for (; startIt != endIt; ++startIt)
		{
			auto point = BaseOverlay::GetPointForAction(ctx, *startIt);
			const ImVec2 size(BaseOverlay::PointSize, BaseOverlay::PointSize);
			ImRect rect(point - size, point + size);
			bool mouseOnPoint = rect.Contains(mousePos);

			if(mouseOnPoint)
			{
				ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			}

			if (!shiftHeld && mouseOnPoint && leftMouseClicked) {
				// Click on a point with no modifier:
				//   - If the point is already in the selection: arm a group drag.
				//     Mouse movement past threshold promotes it to active drag in
				//     ShowScriptPositions; release without movement still fires
				//     the click event so the existing seek-on-click behavior
				//     remains intact.
				//   - Otherwise: legacy behavior, fire click event immediately.
				if (ctx.DrawingScript()->IsSelected(*startIt)) {
					IsGroupMovingPotential = true;
					IsGroupMovingActive = false;
					GroupMovingScriptIdx = ctx.drawingScriptIdx;
					groupAnchorAtS = startIt->atS;
					groupAnchorPos = startIt->pos;
					groupLastAppliedTimeOffset = 0.f;
					groupLastAppliedPosOffset = 0;
				} else {
					EV::Enqueue<FunscriptActionClickedEvent>(*startIt, ctx.DrawingScript());
				}
				return true;
			}
			else if(shiftHeld && IsMovingIdx < 0 && mouseOnPoint && leftMouseClicked)
			{
				// Shift + click on a point: legacy single-point drag.
				ctx.DrawingScript()->ClearSelection();
				ctx.DrawingScript()->SetSelected(*startIt, true);
				IsMovingIdx = ctx.drawingScriptIdx;
				EV::Enqueue<FunscriptActionShouldMoveEvent>(*startIt, ctx.DrawingScript(), true);
				return true;
			}
		}
	}

	if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
	{
		float relX = (mousePos.x - ctx.canvasPos.x) / ctx.canvasSize.x;
		float seekToTime = ctx.offsetTime + (visibleTime * relX);
		EV::Enqueue<ShouldSetTimeEvent>(seekToTime);
		return true;
	}
	else if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle))
	{
		ctx.DrawingScript()->ClearSelection();
		return true;
	}
	else if (ctx.hoveredScriptIdx != ctx.activeScriptIdx && leftMouseClicked) {
		EV::Enqueue<ShouldChangeActiveScriptEvent>(ctx.hoveredScriptIdx);
		return true;
	}
	else if(leftMouseClicked)
	{
		// Begin selection. Mode locked at drag start:
		//   default       -> rect select (replace)
		//   Shift + drag  -> rect select (toggle/XOR with current selection)
		//   Alt   + drag  -> legacy time-band selection
		// Shift + click without movement is deferred to release and fires
		// FunscriptActionShouldCreateEvent (the legacy "new point" gesture).
		IsSelecting = true;
		IsRectSelecting = !ImGui::IsKeyDown(ImGuiMod_Alt);
		IsToggleSelecting = shiftHeld;
		float relSel1 = (mousePos.x - ctx.canvasPos.x) / ctx.canvasSize.x;
		relSel2 = relSel1;
		absSel1 = ctx.offsetTime + (visibleTime * relSel1);
		float startY = (mousePos.y - ctx.canvasPos.y) / ctx.canvasSize.y;
		startY = Util::Clamp(startY, 0.f, 1.f);
		relSel1Y = startY;
		relSel2Y = startY;
		return true;
	}
	return false;
}

void ScriptTimeline::ShowScriptPositions(
	const OFS_Videoplayer* player,
	BaseOverlay* overlay,
	const std::vector<std::shared_ptr<Funscript>>& scripts,
	int activeScriptIdx) noexcept
{
	OFS_PROFILE(__FUNCTION__);

	auto& style = ImGui::GetStyle();
	OverlayDrawingCtx drawingCtx = {0};
	drawingCtx.offsetTime = player->CurrentTime() - (visibleTime / 2.0);
	drawingCtx.activeScriptIdx = activeScriptIdx;
	drawingCtx.visibleTime = visibleTime;
	drawingCtx.totalDuration = player->Duration();
	drawingCtx.scripts = &scripts;
	drawingCtx.hoveredScriptIdx = -1;
	
	if (drawingCtx.totalDuration == 0.f) return;

	if(IsSelecting) handleSelectionScrolling(drawingCtx);
	
	ImGui::Begin(TR_ID(WindowId, Tr::POSITIONS));
	drawingCtx.drawList = ImGui::GetWindowDrawList();
	PositionsItemHovered = ImGui::IsWindowHovered();

	drawingCtx.drawnScriptCount = 0;
	for (auto&& script : scripts) {
		if (script->Enabled) { drawingCtx.drawnScriptCount += 1; }
	}

	const auto totalAvailSize = ImGui::GetContentRegionAvail();
	const float verticalSpacingBetweenScripts = style.ItemSpacing.y*2.f;
	auto availSize = totalAvailSize - ImVec2(0.f , verticalSpacingBetweenScripts*((float)drawingCtx.drawnScriptCount-1));
	if (availSize.y < 0.f) { availSize.y = 0.f; }
	const auto startCursor = ImGui::GetCursorScreenPos();
	auto currentCursor = startCursor;

	for(int i=0; i < scripts.size(); i += 1) 
	{
		auto script = scripts[i].get();
		if (!script->Enabled) continue;
		
		drawingCtx.drawingScriptIdx = i;
		drawingCtx.canvasPos = currentCursor;
		drawingCtx.canvasSize = ImVec2(availSize.x, availSize.y / (float)drawingCtx.drawnScriptCount);
		const auto itemID = ImGui::GetID(script->Title().empty() ? "empty script" : script->Title().c_str());
		ImRect itemBB(drawingCtx.canvasPos, drawingCtx.canvasPos + drawingCtx.canvasSize);
		ImGui::ItemSize(itemBB);
		if (!ImGui::ItemAdd(itemBB, itemID)) {
			continue;
		}

		drawingCtx.drawList->PushClipRect(itemBB.Min - ImVec2(3.f, 3.f), itemBB.Max + ImVec2(3.f, 3.f));

		bool ItemIsHovered = ImGui::IsItemHovered();
		if (ItemIsHovered) {
			drawingCtx.hoveredScriptIdx = i;
		}

		const bool IsActivated = i == activeScriptIdx && drawingCtx.drawnScriptCount > 1;

		if (IsActivated) {
			drawingCtx.drawList->AddRectFilledMultiColor(
				drawingCtx.canvasPos, 
				ImVec2(drawingCtx.canvasPos.x + drawingCtx.canvasSize.x, drawingCtx.canvasPos.y + drawingCtx.canvasSize.y),
				IM_COL32(1.2f*50, 0, 1.2f*50, 255), IM_COL32(1.2f*50, 0, 1.2f*50, 255),
				IM_COL32(1.2f*20, 0, 1.2f*20, 255), IM_COL32(1.2f*20, 0, 1.2f*20, 255)
			);
		}
		else {
			drawingCtx.drawList->AddRectFilledMultiColor(drawingCtx.canvasPos, 
				ImVec2(drawingCtx.canvasPos.x + drawingCtx.canvasSize.x, drawingCtx.canvasPos.y + drawingCtx.canvasSize.y),
				IM_COL32(0, 0, 50, 255), IM_COL32(0, 0, 50, 255),
				IM_COL32(0, 0, 20, 255), IM_COL32(0, 0, 20, 255)
			);
		}

		if (ItemIsHovered) {
			drawingCtx.drawList->AddRectFilled(
				drawingCtx.canvasPos, 
				ImVec2(drawingCtx.canvasPos.x + drawingCtx.canvasSize.x, drawingCtx.canvasPos.y + drawingCtx.canvasSize.y),
				IM_COL32(255, 255, 255, 10)
			);
		}

		auto startIt = script->Actions().lower_bound(FunscriptAction(drawingCtx.offsetTime, 0));
		if (startIt != script->Actions().begin()) {
		    startIt -= 1;
		}

		auto endIt = script->Actions().lower_bound(FunscriptAction(drawingCtx.offsetTime + visibleTime, 0));
		if (endIt != script->Actions().end()) {
		    endIt += 1;
		}

		drawingCtx.actionFromIdx = std::distance(script->Actions().begin(), startIt);
		drawingCtx.actionToIdx = std::distance(script->Actions().begin(), endIt);

		if(script->HasSelection())
		{
			auto startIt = script->Selection().lower_bound(FunscriptAction(drawingCtx.offsetTime, 0));
			if (startIt != script->Selection().begin())
				startIt -= 1;

			auto endIt = script->Selection().lower_bound(FunscriptAction(drawingCtx.offsetTime + drawingCtx.visibleTime, 0));
			if (endIt != script->Selection().end())
				endIt += 1;

			drawingCtx.selectionFromIdx = std::distance(script->Selection().begin(), startIt);
			drawingCtx.selectionToIdx = std::distance(script->Selection().begin(), endIt);
		}
		else 
		{
			drawingCtx.selectionFromIdx = 0;
			drawingCtx.selectionToIdx = 0;
		}

		// border
		constexpr float borderThicknes = 1.f;
		uint32_t borderColor = IsActivated ? IM_COL32(0, 180, 0, 255) : IM_COL32(255, 255, 255, 255);
		if (script->HasSelection()) { 
			borderColor = ImGui::GetColorU32(ImGuiCol_SliderGrabActive); 
		}
		drawingCtx.drawList->AddRect(
			drawingCtx.canvasPos - ImVec2(2, 2),
			ImVec2(drawingCtx.canvasPos.x + drawingCtx.canvasSize.x, drawingCtx.canvasPos.y + drawingCtx.canvasSize.y) + ImVec2(2, 2),
			borderColor,
			0.f, ImDrawFlags_None,
			borderThicknes
		);

		// draws mode specific things in the timeline
		// by default it draws the frame and time dividers
		// DrawAudioWaveform called in scripting mode to control the draw order. spaghetti
		{
			OFS_PROFILE("overlay->DrawScriptPositionContent(drawingCtx)");
			overlay->DrawScriptPositionContent(drawingCtx);
		}

		// current position indicator -> |
		drawingCtx.drawList->AddTriangleFilled(
			drawingCtx.canvasPos + ImVec2((drawingCtx.canvasSize.x/2.f) - ImGui::GetFontSize(), 0.f),
			drawingCtx.canvasPos + ImVec2((drawingCtx.canvasSize.x/2.f) + ImGui::GetFontSize(), 0.f),
			drawingCtx.canvasPos + ImVec2((drawingCtx.canvasSize.x/2.f), ImGui::GetFontSize()/1.5f),
			IM_COL32(255, 255, 255, 255)
		);
		drawingCtx.drawList->AddLine(
			drawingCtx.canvasPos + ImVec2((drawingCtx.canvasSize.x/2.f)-0.5f, 0),
			drawingCtx.canvasPos + ImVec2((drawingCtx.canvasSize.x/2.f)-0.5f, drawingCtx.canvasSize.y-1.f),
			IM_COL32(255, 255, 255, 255),
		4.0f);

		// selection box
		constexpr auto selectColor = IM_COL32(3, 252, 207, 255);
		constexpr auto selectColorBackground = IM_COL32(3, 252, 207, 100);
		// Shift+drag (toggle/XOR) uses a warmer color so the user can tell
		// at a glance that the gesture will XOR with the existing selection
		// rather than replace it.
		constexpr auto toggleColor = IM_COL32(252, 207, 3, 255);
		constexpr auto toggleColorBackground = IM_COL32(252, 207, 3, 100);
		if (IsSelecting && (i == activeScriptIdx)) {
			const auto edgeColor = IsToggleSelecting ? toggleColor : selectColor;
			const auto fillColor = IsToggleSelecting ? toggleColorBackground : selectColorBackground;
			float relSel1 = (absSel1 - drawingCtx.offsetTime) / visibleTime;
			float yTop = IsRectSelecting ? (drawingCtx.canvasSize.y * std::min(relSel1Y, relSel2Y)) : 0.f;
			float yBot = IsRectSelecting ? (drawingCtx.canvasSize.y * std::max(relSel1Y, relSel2Y)) : drawingCtx.canvasSize.y;
			drawingCtx.drawList->AddRectFilled(
				drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * relSel1, yTop),
				drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * relSel2, yBot),
				fillColor);
			drawingCtx.drawList->AddLine(
				drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * relSel1, yTop),
				drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * relSel1, yBot),
				edgeColor, 3.0f);
			drawingCtx.drawList->AddLine(
				drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * relSel2, yTop),
				drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * relSel2, yBot),
				edgeColor, 3.0f);
			if (IsRectSelecting) {
				drawingCtx.drawList->AddLine(
					drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * relSel1, yTop),
					drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * relSel2, yTop),
					edgeColor, 3.0f);
				drawingCtx.drawList->AddLine(
					drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * relSel1, yBot),
					drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * relSel2, yBot),
					edgeColor, 3.0f);
			}
		}

		// selectionStart currently used for controller select
		if (startSelectionTime >= 0.f) {
			float startSelectRel = (startSelectionTime - drawingCtx.offsetTime) / visibleTime;
			drawingCtx.drawList->AddLine(
				drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * startSelectRel, 0),
				drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * startSelectRel, drawingCtx.canvasSize.y),
				selectColor, 3.0f
			);
		}

		// Handle action clicks
		if(ItemIsHovered && handleTimelineClicks(drawingCtx)) { /* click was handled */ }
		else if(drawingCtx.drawingScriptIdx == IsMovingIdx)
		{
			if(ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f))
			{
				// Update dragged action
				auto mousePos = ImGui::GetMousePos();
				auto newAction = getActionForPoint(drawingCtx, mousePos);
				EV::Enqueue<FunscriptActionShouldMoveEvent>(newAction, scripts[i], false);
			}
			else
			{
				// Stop dragging
				IsMovingIdx = -1;
			}
		}
		else if(drawingCtx.drawingScriptIdx == GroupMovingScriptIdx && (IsGroupMovingPotential || IsGroupMovingActive))
		{
			// Group drag: armed on press over a selected point; promotes to
			// active once the cursor moves past the imgui drag threshold.
			// Each frame we feed the incremental delta to the script so the
			// whole selection follows the cursor in lockstep.
			constexpr float kDragThresholdPx = 4.f;
			if(ImGui::IsMouseDragging(ImGuiMouseButton_Left, kDragThresholdPx))
			{
				if(!IsGroupMovingActive)
				{
					IsGroupMovingActive = true;
					EV::Enqueue<FunscriptSelectionShouldMoveEvent>(0.f, 0, true, scripts[i]);
				}
				auto mousePos = ImGui::GetMousePos();
				auto target = getActionForPoint(drawingCtx, mousePos);
				float deltaT = target.atS - groupAnchorAtS;
				int32_t deltaP = (int32_t)target.pos - groupAnchorPos;
				float incrT = deltaT - groupLastAppliedTimeOffset;
				int32_t incrP = deltaP - groupLastAppliedPosOffset;
				if(incrT != 0.f || incrP != 0)
				{
					EV::Enqueue<FunscriptSelectionShouldMoveEvent>(incrT, incrP, false, scripts[i]);
					groupLastAppliedTimeOffset = deltaT;
					groupLastAppliedPosOffset = deltaP;
				}
			}
			else if(ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			{
				// Released without ever crossing the drag threshold -- this
				// was a click on a selected point. Preserve OFS's existing
				// seek-on-click by sending the player to the anchor time.
				if(!IsGroupMovingActive)
				{
					EV::Enqueue<ShouldSetTimeEvent>(groupAnchorAtS);
				}
				IsGroupMovingPotential = false;
				IsGroupMovingActive = false;
				GroupMovingScriptIdx = -1;
				groupLastAppliedTimeOffset = 0.f;
				groupLastAppliedPosOffset = 0;
			}
		}
		else if(IsSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			IsSelecting = false;

			// Distinguish click vs drag using imgui's drag-delta threshold.
			// A tiny delta means the mouse was released without ever crossing
			// the threshold -- treat as a click.
			const auto dragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f);
			const float distSq = dragDelta.x * dragDelta.x + dragDelta.y * dragDelta.y;
			constexpr float kClickThresholdSq = 16.f; // 4 px
			const bool wasClick = distSq < kClickThresholdSq;

			if(wasClick)
			{
				if(IsToggleSelecting)
				{
					// Shift + click on empty area: legacy "create new point"
					// gesture preserved from before Shift was reassigned to
					// the toggle-rect modifier.
					auto mp = ImGui::GetMousePos();
					auto newAction = getActionForPoint(drawingCtx, mp);
					EV::Enqueue<FunscriptActionShouldCreateEvent>(newAction, drawingCtx.DrawingScript());
				}
				else
				{
					// Bare click on empty area clears the active script's
					// selection -- the standard desktop idiom.
					auto& sc = drawingCtx.ActiveScript();
					if(sc) sc->ClearSelection();
				}
			}
			else
			{
				const bool ctrlHeld = (SDL_GetModState() & KMOD_CTRL) != 0;
				const bool clearSelection = !ctrlHeld;
				updateSelection(drawingCtx, clearSelection);
			}

			IsRectSelecting = false;
			IsToggleSelecting = false;
		}
		else if(IsMovingIdx < 0 && ItemIsHovered)
		{
			handleTimelineHover(drawingCtx);
		}

		ImVec2 newCursor(drawingCtx.canvasPos.x, drawingCtx.canvasPos.y + drawingCtx.canvasSize.y + verticalSpacingBetweenScripts);
		if (newCursor.y < (startCursor.y + totalAvailSize.y)) { currentCursor = newCursor; }

		// right click context menu
		if (ImGui::BeginPopupContextItem(script->Title().c_str()))
		{
			if (ImGui::BeginMenu(TR_ID("SCRIPTS", Tr::SCRIPTS))) {
				for (auto& script : scripts) {
					if(script->Title().empty()) {
						ImGui::TextDisabled(TR(NONE));
						continue;
					}
					ImGui::PushItemFlag(ImGuiItemFlags_Disabled, drawingCtx.drawnScriptCount == 1 && script->Enabled);
					if (ImGui::Checkbox(script->Title().c_str(), &script->Enabled) && !script->Enabled) {
						if (i == activeScriptIdx) {
							// find a enabled script which can be set active
							for (int i = 0; i < scripts.size(); i += 1) {
								if (scripts[i]->Enabled) {									
									EV::Enqueue<ShouldChangeActiveScriptEvent>(i);
									break;
								}
							}
						}
					}
					ImGui::PopItemFlag();
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(TR_ID("RENDERING", Tr::RENDERING))) {
				auto& overlayState = BaseOverlayState::State(overlayStateHandle);
				ImGui::MenuItem(TR(SHOW_ACTION_LINES), 0, &BaseOverlay::ShowLines);
				ImGui::MenuItem(TR(SHOW_ACTION_POINTS), 0, &BaseOverlay::ShowPoints);
				ImGui::MenuItem(TR(SPLINE_MODE), 0, &overlayState.SplineMode);
				ImGui::MenuItem(TR(SHOW_VIDEO_POSITION), 0, &overlayState.SyncLineEnable);
				OFS::Tooltip(TR(SHOW_VIDEO_POSITION_TOOLTIP));
				ImGui::EndMenu();
			}

			auto updateAudioWaveformThread = [](void* userData) -> int {
				auto& ctx = *((ScriptTimeline*)userData);
				std::error_code ec;
				auto ffmpegPath = Util::FfmpegPath();
				auto outputPath = Util::Prefpath("tmp");
				if (!Util::CreateDirectories(outputPath)) {
					return 0;
				}
				
				outputPath = (Util::PathFromString(outputPath) / "audio.flac").u8string();
				bool succ = ctx.Wave.data.GenerateAndLoadFlac(ffmpegPath.u8string(), ctx.videoPath, outputPath);
				EV::Enqueue<WaveformProcessingFinishedEvent>();
				return 0;
			};
			if (ImGui::BeginMenu(TR_ID("WAVEFORM", Tr::WAVEFORM))) {
				if(ImGui::BeginMenu(TR_ID("SETTINGS", Tr::SETTINGS))) {
					ImGui::SetNextItemWidth(ImGui::GetFontSize()*5.f);
					ImGui::DragFloat(TR(SCALE), &ScaleAudio, 0.01f, 0.01f, 10.f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
					ImGui::ColorEdit3(TR(COLOR), &Wave.WaveformColor.Value.x, ImGuiColorEditFlags_NoInputs);
					ImGui::EndMenu();
				}
				if (ImGui::MenuItem(TR(ENABLE_WAVEFORM), NULL, &ShowAudioWaveform, !Wave.data.BusyGenerating())) {}

				if(Wave.data.BusyGenerating()) {
					ImGui::MenuItem(TR(PROCESSING_AUDIO), NULL, false, false);
					ImGui::SameLine();
					OFS::Spinner("##AudioSpin", ImGui::GetFontSize() / 3.f, 4.f, ImGui::GetColorU32(ImGuiCol_TabActive));
				}
				else if(ImGui::MenuItem(TR(UPDATE_WAVEFORM), NULL, false, !Wave.data.BusyGenerating() && !videoPath.empty())) {
					if (!Wave.data.BusyGenerating()) {
						ShowAudioWaveform = false; // gets switched true after processing

						auto& waveCache = WaveformState::StaticStateSlow();
						auto samples = waveCache.GetSamples();
						if(waveCache.Filename == videoPath && !samples.empty())
						{
							Wave.data.SetSamples(std::move(samples));
							ShowAudioWaveform = true;
						}
						else 
						{
							auto handle = SDL_CreateThread(updateAudioWaveformThread, "OFS_GenWaveform", this);
							SDL_DetachThread(handle);
						}
					}
				}
				ImGui::EndMenu();
			}
			ImGui::EndPopup();
		}

		drawingCtx.drawList->PopClipRect();
	}
	ImGui::End();
}

constexpr uint32_t HighRangeCol = IM_COL32(0xE3, 0x42, 0x34, 0xff);
constexpr uint32_t MidRangeCol = IM_COL32(0xE8, 0xD7, 0x5A, 0xff);
constexpr uint32_t LowRangeCol = IM_COL32(0xF7, 0x65, 0x38, 0xff); // IM_COL32(0xff, 0xba, 0x08, 0xff);

void ScriptTimeline::DrawAudioWaveform(const OverlayDrawingCtx& ctx) noexcept
{
	OFS_PROFILE(__FUNCTION__);

	if (ShowAudioWaveform && Wave.data.SampleCount() > 0 && ctx.totalDuration > 1.f) {
		auto renderWaveform = [](ScriptTimeline* timeline, const OverlayDrawingCtx& ctx) noexcept
		{
			OFS_PROFILE("DrawAudioWaveform::renderWaveform");
			
			timeline->Wave.Update(ctx);
			
			ctx.drawList->AddCallback([](const ImDrawList* parent_list, const ImDrawCmd* cmd) noexcept {
				ScriptTimeline* ctx = (ScriptTimeline*)cmd->UserCallbackData;
				
				glActiveTexture(GL_TEXTURE1);
				glBindTexture(GL_TEXTURE_2D, ctx->Wave.WaveformTex);
				glActiveTexture(GL_TEXTURE0);
				ctx->Wave.WaveShader->Use();
				auto drawData = OFS_ImGui::CurrentlyRenderedViewport->DrawData;
				float L = drawData->DisplayPos.x;
				float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
				float T = drawData->DisplayPos.y;
				float B = drawData->DisplayPos.y + drawData->DisplaySize.y;
				const float orthoProjection[4][4] =
				{
					{ 2.0f / (R - L), 0.0f, 0.0f, 0.0f },
					{ 0.0f, 2.0f / (T - B), 0.0f, 0.0f },
					{ 0.0f, 0.0f, -1.0f, 0.0f },
					{ (R + L) / (L - R),  (T + B) / (B - T),  0.0f,   1.0f },
				};
				ctx->Wave.WaveShader->ProjMtx(&orthoProjection[0][0]);
				ctx->Wave.WaveShader->AudioData(1);
				ctx->Wave.WaveShader->SampleOffset(ctx->Wave.samplingOffset);
				ctx->Wave.WaveShader->ScaleFactor(ctx->ScaleAudio);
				ctx->Wave.WaveShader->Color(&ctx->Wave.WaveformColor.Value.x);
			}, timeline);

			ctx.drawList->AddImage(0, ctx.canvasPos, ctx.canvasPos + ctx.canvasSize);
			ctx.drawList->AddCallback(ImDrawCallback_ResetRenderState, 0);
		};

		renderWaveform(this, ctx);
	}
}