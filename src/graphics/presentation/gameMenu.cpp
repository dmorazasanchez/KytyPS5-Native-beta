#include "graphics/presentation/gameMenu.h"

#include "graphics/native_gpu/nativeShadow.h"
#include "graphics/native_gpu/nativeUpscaler.h"
#include "graphics/presentation/exitVideo.h"
#include "loader/gamePatch.h"
#include "graphics/presentation/performanceOverlay.h"
#include "imgui.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>

namespace Libs::Graphics {

namespace {

enum class Page : uint8_t { Closed, Main, Cheats, Upscaling, ConfirmExit };

enum MainItem : int { ItemResume, ItemMonitoring, ItemUpscaling, ItemCheats, ItemExit, MainItemCount };
enum UpscalingItem : int { UpItemUpscaler, UpItemFrameGen, UpItemBack, UpItemCount }; // resolution/preset: per-game settings, before launch
enum ConfirmItem : int { ConfirmExitGame, ConfirmCancel, ConfirmItemCount };

struct MenuState {
	Page page     = Page::Closed;
	int  selected = 0;
};

std::mutex            g_mutex;
MenuState             g_state;
std::atomic<bool>     g_open {false};
std::atomic<uint64_t> g_revision {0};
std::string g_cheat_message;

// Window thread only: Share and Options held, for the combination.
bool g_back_down  = false;
bool g_start_down = false;

MenuState GetState() {
	std::scoped_lock lock(g_mutex);
	return g_state;
}

void SetState(MenuState state) {
	{
		std::scoped_lock lock(g_mutex);
		g_state = state;
	}
	g_open.store(state.page != Page::Closed, std::memory_order_release);
	g_revision.fetch_add(1, std::memory_order_acq_rel);
}

int ItemCount(Page page) {
    if (page == Page::Upscaling) {
        return NativeUpscaler::TitleSupported() ? static_cast<int>(UpItemCount) : 1; // Back only
    }
    if (page == Page::Cheats) {
        return static_cast<int>(Loader::GamePatch::ListCheats().size()) + 1; // Back
    }
    return page == Page::ConfirmExit ? static_cast<int>(ConfirmItemCount) :
                                      static_cast<int>(MainItemCount);
}

std::string CheatMessage() {
    std::scoped_lock lock(g_mutex);
    return g_cheat_message;
}
void SetCheatMessage(std::string message) {
    {
        std::scoped_lock lock(g_mutex);
        g_cheat_message = std::move(message);
    }
    g_revision.fetch_add(1, std::memory_order_acq_rel);
}

const char* MonitoringLabel(int mode) {
	switch (mode) {
		case 1: return "FPS";
		case 2: return "Detailed";
		default: return "Off";
	}
}

void StepMonitoring(int step) {
	SetPerformanceOverlayMode((PerformanceOverlayMode() + step + 3) % 3);
	g_revision.fetch_add(1, std::memory_order_acq_rel);
}

// Upscaling page: the upscaler switches live (next frame). Resolution and preset come from the game's settings in
// the launcher, because the title sizes its frame for them.
void StepUpscaling(int item, int step) {
	using namespace NativeUpscaler;
	if (item == UpItemFrameGen) { // Off <-> FSR 3 (2x), live: the next flip
		// (Dynamic uses the same memory as 2x and is not offered: development only, KYTY_FRAMEGEN=fsr3_dynamic.)
		const int n = FrameGen() == FrameGenMode::Fsr3Dynamic ? static_cast<int>(FrameGenMode::Count) : static_cast<int>(FrameGenMode::Fsr3Dynamic);
		SetFrameGen(static_cast<FrameGenMode>((static_cast<int>(FrameGen()) + (step < 0 ? n - 1 : 1)) % n));
		g_revision.fetch_add(1, std::memory_order_acq_rel);
		return;
	}
	if (item != UpItemUpscaler) {
		return;
	}
	auto s     = CurrentSettings();
	const int n = static_cast<int>(UpscalerKind::Count); // Game FSR3 -> HLX FSR 4 -> HLX DLSS (Model E) -> ...
	s.upscaler  = static_cast<UpscalerKind>((static_cast<int>(s.upscaler) + (step < 0 ? n - 1 : 1)) % n);
	ApplySettings(s);
	g_revision.fetch_add(1, std::memory_order_acq_rel);
}

void LeaveGame() {
	SDL_Event quit {};
	quit.type = SDL_EVENT_QUIT;
	SDL_PushEvent(&quit);
}

void Activate(const MenuState& state) {
	if (state.page == Page::Main) {
		switch (state.selected) {
			case ItemResume: SetState({}); break;
			case ItemMonitoring: StepMonitoring(1); break;
			case ItemUpscaling: SetState({Page::Upscaling, 0}); break;
            case ItemCheats:
                SetCheatMessage("");
                SetState({Page::Cheats, 0});
                break;
			case ItemExit: SetState({Page::ConfirmExit, ConfirmExitGame}); break;
			default: break;
		}
		return;
	}
    if (state.page == Page::Upscaling) {
        if (!NativeUpscaler::TitleSupported() || state.selected == UpItemBack) {
            SetState({Page::Main, ItemUpscaling});
        } else {
            StepUpscaling(state.selected, 1);
        }
        return;
    }
    if (state.page == Page::Cheats) {
        const auto cheats = Loader::GamePatch::ListCheats();
        if (state.selected < static_cast<int>(cheats.size())) {
            const auto result = Loader::GamePatch::ToggleCheat(static_cast<size_t>(state.selected));
            SetCheatMessage(result.message);
        } else {
            SetState({Page::Main, ItemCheats});
        }
        return;
    }
	if (state.selected == ConfirmExitGame) {
		SetState({});
        MarkUserExit();
        LeaveGame();
	} else {
		SetState({Page::Main, ItemExit});
	}
}

void Back(const MenuState& state) {
    if (state.page == Page::Upscaling) {
        SetState({Page::Main, ItemUpscaling});
        return;
    }
    if (state.page == Page::Cheats) {
        SetState({Page::Main, ItemCheats});
    } else {
	    SetState(state.page == Page::ConfirmExit ? MenuState {Page::Main, ItemExit} : MenuState {});
    }
}

// Navigation shared by the gamepad and the keyboard. True when the menu took the input.
bool Navigate(int up_down, int left_right, bool accept, bool back) {
	MenuState state = GetState();
	if (state.page == Page::Closed) {
		return false;
	}
	if (up_down != 0) {
		const int count = ItemCount(state.page);
		state.selected  = (state.selected + up_down + count) % count;
		SetState(state);
	} else if (left_right != 0 && state.page == Page::Main && state.selected == ItemMonitoring) {
		StepMonitoring(left_right);
	} else if (left_right != 0 && state.page == Page::Upscaling && NativeUpscaler::TitleSupported()) {
		StepUpscaling(state.selected, left_right);
	} else if (accept) {
		Activate(state);
	} else if (back) {
		Back(state);
	}
	return true;
}

} // namespace

bool ProcessGameMenuInput(const SDL_Event& event) {
	if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
		const bool down   = event.gbutton.down;
		const auto button = static_cast<SDL_GamepadButton>(event.gbutton.button);
		if (button == SDL_GAMEPAD_BUTTON_BACK) {
			g_back_down = down;
		} else if (button == SDL_GAMEPAD_BUTTON_START) {
			g_start_down = down;
		}
		if (!down) {
			return false; // releases go on to the game: nothing stays held there
		}
		const bool combination = (button == SDL_GAMEPAD_BUTTON_BACK && g_start_down) ||
		                         (button == SDL_GAMEPAD_BUTTON_START && g_back_down);
		if (button == SDL_GAMEPAD_BUTTON_GUIDE || combination) {
			SetState(GameMenuOpen() ? MenuState {} : MenuState {Page::Main, ItemResume});
			return true;
		}
		switch (button) {
			case SDL_GAMEPAD_BUTTON_DPAD_UP: return Navigate(-1, 0, false, false);
			case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return Navigate(1, 0, false, false);
			case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return Navigate(0, -1, false, false);
			case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return Navigate(0, 1, false, false);
			case SDL_GAMEPAD_BUTTON_SOUTH: return Navigate(0, 0, true, false);
			case SDL_GAMEPAD_BUTTON_EAST: return Navigate(0, 0, false, true);
			default: return GameMenuOpen();
		}
	}
	if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
		return GameMenuOpen();
	}
	// A key the menu took on its way down is the menu's on its way up too: the game must not see a lone release
	// (Yotei opened its own pause menu from the Enter/arrow releases of a menu session).
	static std::array<bool, SDL_SCANCODE_COUNT> taken {};
	if (event.type == SDL_EVENT_KEY_UP && event.key.scancode < SDL_SCANCODE_COUNT && taken[event.key.scancode]) {
		taken[event.key.scancode] = false;
		return true;
	}
	if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode < SDL_SCANCODE_COUNT &&
	    (event.key.key == SDLK_F10 || GameMenuOpen())) {
		taken[event.key.scancode] = true;
	}
	if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F10) {
		if (!event.key.repeat) {
			SetState(GameMenuOpen() ? MenuState {} : MenuState {Page::Main, ItemResume});
		}
		return true;
	}
	if (event.type == SDL_EVENT_KEY_DOWN && GameMenuOpen()) {
		switch (event.key.key) {
			case SDLK_UP: return Navigate(-1, 0, false, false);
			case SDLK_DOWN: return Navigate(1, 0, false, false);
			case SDLK_LEFT: return Navigate(0, -1, false, false);
			case SDLK_RIGHT: return Navigate(0, 1, false, false);
			case SDLK_RETURN:
			case SDLK_KP_ENTER: return Navigate(0, 0, true, false);
			case SDLK_ESCAPE:
			case SDLK_BACKSPACE: return Navigate(0, 0, false, true);
			default: return true;
		}
	}
	return false;
}

bool GameMenuOpen() noexcept {
	return g_open.load(std::memory_order_acquire);
}

uint64_t GameMenuRevision() noexcept {
	return g_revision.load(std::memory_order_acquire);
}

void DrawGameMenu(vk::Extent2D extent) {
	const MenuState state = GetState();
	if (state.page == Page::Closed) {
		return;
	}
	const ImVec2 display(static_cast<float>(extent.width), static_cast<float>(extent.height));
	const float  scale = std::max(std::min(display.x / 1280.0f, display.y / 720.0f), 0.5f);
	ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, display, IM_COL32(0, 0, 0, 170));
	ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    constexpr const char* hint = "Cross / Enter: select    Circle / Esc: back    PS / F10: close";
    ImGui::PushFont(nullptr, 17.0f * scale);
    const float hint_width = ImGui::CalcTextSize(hint).x + 2.0f * 24.0f * scale;
    ImGui::PopFont();
    ImGui::SetNextWindowSize(
        {std::max(std::min(std::max(460.0f * scale, hint_width), display.x - 32.0f), 1.0f), 0.0f},
        ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24.0f * scale, 22.0f * scale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12.0f * scale, 12.0f * scale});
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * scale);
	ImGui::PushFont(nullptr, 22.0f * scale);
	constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
	                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
	                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
	ImGui::Begin("##KytyGameMenu", nullptr, flags);
	ImGui::TextUnformatted("KytyPS5-Native");
	ImGui::SameLine();
	ImGui::TextDisabled("%s", NativeGpu::PrimaryRendering() ? "native PS5 GPU path" : "Vulkan path");
	ImGui::Separator();
	const float row_height = 44.0f * scale;
	auto        row        = [&](int index, const char* label, const char* value) {
        const bool selected = state.selected == index;
        const ImVec2 start  = ImGui::GetCursorScreenPos();
        const float  width  = ImGui::GetContentRegionAvail().x;
        if (selected) {
            ImGui::GetWindowDrawList()->AddRectFilled(start, {start.x + width, start.y + row_height},
                                                      IM_COL32(255, 255, 255, 40), 6.0f * scale);
        }
        const float text_y = start.y + (row_height - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::GetWindowDrawList()->AddText({start.x + 12.0f * scale, text_y}, IM_COL32(240, 240, 245, 255), label);
        if (value != nullptr) {
            const float value_width = ImGui::CalcTextSize(value).x;
            ImGui::GetWindowDrawList()->AddText({start.x + width - value_width - 12.0f * scale, text_y},
                                                IM_COL32(170, 200, 255, 255), value);
        }
        ImGui::Dummy({width, row_height});
	};
	if (state.page == Page::Main) {
		row(ItemResume, "Resume", nullptr);
		row(ItemMonitoring, "Monitoring", MonitoringLabel(PerformanceOverlayMode()));
		row(ItemUpscaling, "Upscaling", NativeUpscaler::TitleSupported()
		                                    ? NativeUpscaler::UpscalerName(NativeUpscaler::CurrentSettings().upscaler)
		                                    : "n/a");
        row(ItemCheats, "Cheats / Patches", nullptr);
		row(ItemExit, "Exit game", nullptr);
    } else if (state.page == Page::Upscaling) {
        using namespace NativeUpscaler;
        ImGui::TextUnformatted("Upscaling");
        if (!TitleSupported()) {
            ImGui::PushFont(nullptr, 16.0f * scale);
            ImGui::TextWrapped("%s", Status().c_str());
            ImGui::PopFont();
            row(0, "Back", nullptr);
        } else {
            const auto s      = CurrentSettings();
            const int  level  = s.preset == Preset::Custom ? s.level : PresetLevel(s.preset);
            const auto size   = LevelSize(level);
            const auto output = OutputSize();
            const auto target = TargetSize();
            char       ratio[160];
            const std::string up = std::string("< ") + UpscalerName(s.upscaler) + " >";
            row(UpItemUpscaler, "Upscaler", up.c_str());
            const std::string fg = std::string("< ") + FrameGenName(FrameGen()) + " >";
            row(UpItemFrameGen, "Frame generation", fg.c_str());
            row(UpItemBack, "Back", nullptr);
            ImGui::Separator();
            ImGui::PushFont(nullptr, 16.0f * scale);
            if (level >= 0 && size.first != 0 && target.first != 0 && target.first != output.first) {
                std::snprintf(ratio, sizeof(ratio), "%s: %u x %u -> %u x %u (%.2fx), scaled to %u x %u", PresetName(s.preset), size.first,
                              size.second, target.first, target.second, static_cast<double>(target.first) / size.first, output.first,
                              output.second);
            } else if (level >= 0 && size.first != 0) {
                std::snprintf(ratio, sizeof(ratio), "%s: %u x %u -> %u x %u (%.2fx)", PresetName(s.preset), size.first, size.second,
                              output.first, output.second, static_cast<double>(output.first) / size.first);
            } else {
                std::snprintf(ratio, sizeof(ratio), "%s: resolution chosen by the game -> %u x %u", PresetName(s.preset), output.first,
                              output.second);
            }
            ImGui::TextUnformatted(ratio);
            ImGui::TextWrapped("%s", Status().c_str());
            if (const std::string fg_status = FrameGenStatus(); !fg_status.empty()) {
                ImGui::TextWrapped("%s", fg_status.c_str());
            } else if (FrameGen() == FrameGenMode::Fsr3) {
                ImGui::TextWrapped("Frame generation: one interpolated frame between real frames (FSR 3.1); the game runs at half the refresh rate.");
            } else if (FrameGen() == FrameGenMode::Fsr3Dynamic) {
                ImGui::TextWrapped("Frame generation: interpolated frames (FSR 3.1) only for the refreshes the game misses; none at 60 FPS.");
            }
            if (level > 15) {
                ImGui::TextWrapped("Below 1920 x 1080 (the game's lowest built-in level) is experimental.");
            }
            if (s.output_h != 0 && s.upscaler == UpscalerKind::Game) {
                ImGui::TextWrapped("Output %dp applies to HLX; the game's FSR3 always outputs at the game's resolution.", s.output_h);
            } else if (s.output_h != 0 && level < 0) {
                ImGui::TextWrapped("Output %dp needs a fixed preset; with the game's dynamic resolution HLX outputs at the game's resolution.",
                                   s.output_h);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.28f, 1.0f)); // amber
            ImGui::TextWrapped("Warning: changing the upscaler during play restarts it and can crash the system on the BC-250 when memory is "
                               "tight (with frame generation in particular). Choose it in the launcher before starting the game.");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("Left / Right: switch upscaler (next frame). Preset and output resolution: game settings in the launcher.");
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
    } else if (state.page == Page::Cheats) {
        ImGui::TextUnformatted("Cheats / Patches");
        const auto cheats = Loader::GamePatch::ListCheats();
        if (cheats.empty())
            ImGui::TextDisabled("%s", Loader::GamePatch::CheatStatus().c_str());
        for (size_t index = 0; index < cheats.size(); ++index) {
            const auto& c = cheats[index];
            // Applied is what is running; enabled is saved for the next launch.
            std::string value;
            if (c.applied != c.enabled) {
                value = c.enabled ? "ON (next)" : "OFF (next)";
            } else if (c.live) {
                value = c.enabled ? "ON  LIVE" : "OFF LIVE";
            } else {
                value = c.applied ? "ON  ACTIVE" : "OFF";
            }
            row(static_cast<int>(index), c.name.c_str(), value.c_str());
        }
        row(static_cast<int>(cheats.size()), "Back", nullptr);
        ImGui::Separator();
        ImGui::PushFont(nullptr, 16.0f * scale);
        const auto message = CheatMessage();
        if (!message.empty())
            ImGui::TextWrapped("%s", message.c_str());
        else
            ImGui::TextWrapped("LIVE: safe one-byte patch only. Code hooks require game restart.");
        ImGui::PopFont();
	} else {
		ImGui::TextUnformatted("Exit the game?");
		ImGui::TextDisabled("Progress the game has not saved is lost.");
		row(ConfirmExitGame, "Exit game", nullptr);
		row(ConfirmCancel, "Cancel", nullptr);
	}
	ImGui::Separator();
	ImGui::PushFont(nullptr, 17.0f * scale);
	ImGui::TextDisabled("Cross / Enter: select    Circle / Esc: back    PS / F10: close");
	ImGui::PopFont();
	ImGui::End();
	ImGui::PopFont();
	ImGui::PopStyleVar(3);
}

} // namespace Libs::Graphics
