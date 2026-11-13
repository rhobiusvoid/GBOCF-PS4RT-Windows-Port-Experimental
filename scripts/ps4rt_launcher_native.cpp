// SPDX-License-Identifier: GPL-2.0-or-later
// NoisyFairy launcher: SDL3 + Dear ImGui front end for scripts/ps4rt_launch.py (Windows).
// Picks the game folder, graphics overrides, and starts the game or the DLC installer.
#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "ps4rt_png.h"

namespace fs = std::filesystem;

// Load the project's existing scalable font at a slightly larger size.
// ImGui's built-in font is 13px and becomes pixelated when enlarged.
static void configure_setup_font() {
    const char* base = SDL_GetBasePath();
    const fs::path binary_dir = base && *base ? fs::path(base) : fs::current_path();
    const fs::path paths[] = {
        binary_dir / "../gpu/third_party/fonts/DejaVuSans.ttf",
        binary_dir / "gpu/third_party/fonts/DejaVuSans.ttf",
        binary_dir / "assets/fonts/DejaVuSans.ttf",
        binary_dir / "fonts/DejaVuSans.ttf",
    };
    std::error_code ec;
    for (const auto& path : paths) {
        if (!fs::is_regular_file(path, ec)) continue;
        if (ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str(), 16.0f)) {
            std::fprintf(stderr, "MOBILE SUIT GUNDAM BATTLE OPERATION Code Fairy launcher font: %s\n", path.string().c_str());
            return;
        }
    }
    // Nonfatal fallback for an incomplete portable archive.
    ImGui::GetStyle().FontScaleMain = 1.16f;
}

// Use the same MOBILE SUIT GUNDAM BATTLE OPERATION Code Fairy image and FFmpeg PNG loader as the in-game overlay.
// Missing image is nonfatal: the plain plum background is a safe fallback.
static SDL_Texture* load_setup_art(SDL_Renderer* renderer) {
    const char* custom = std::getenv("PS4RT_OVERLAY_BACKGROUND");
    const char* base = SDL_GetBasePath();
    const fs::path binary_dir = base && *base ? fs::path(base) : fs::current_path();
    std::vector<fs::path> candidates;
    if (custom && *custom) candidates.emplace_back(custom);
    candidates.emplace_back(binary_dir / "../gpu/assets/overlay.png");
    candidates.emplace_back(binary_dir / "gpu/assets/overlay.png");
    candidates.emplace_back(binary_dir / "assets/overlay.png");
    candidates.emplace_back(binary_dir / "../assets/overlay.png");

    std::error_code ec;
    for (const auto& path : candidates) {
        if (!fs::is_regular_file(path, ec)) continue;
        Ps4RtPng::Image image;
        if (!Ps4RtPng::LoadRgba(path.string().c_str(), image)) continue;
        SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                                SDL_TEXTUREACCESS_STATIC,
                                                image.width, image.height);
        if (!texture) continue;
        if (!SDL_UpdateTexture(texture, nullptr, image.rgba.data(),
                               image.width * 4)) {
            SDL_DestroyTexture(texture);
            continue;
        }
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        std::fprintf(stderr, "MOBILE SUIT GUNDAM BATTLE OPERATION Code Fairy launcher art: %s\n", path.string().c_str());
        return texture;
    }
    std::fprintf(stderr, "MOBILE SUIT GUNDAM BATTLE OPERATION Code Fairy launcher: PNG background not found; using plum fallback.\n");
    return nullptr;
}

static void draw_setup_background(SDL_Renderer* renderer, SDL_Texture* art) {
    SDL_SetRenderDrawColor(renderer, 33, 15, 31, 255);
    SDL_RenderClear(renderer);
    int width = 0, height = 0;
    SDL_GetRenderOutputSize(renderer, &width, &height);
    if (width <= 0 || height <= 0) return;
    if (art) {
        float image_w = 0, image_h = 0;
        if (SDL_GetTextureSize(art, &image_w, &image_h) && image_w > 0 && image_h > 0) {
            // Center-crop to fill the setup window without stretching the artwork.
            const float scale = std::max(float(width) / image_w, float(height) / image_h);
            const float visible_w = float(width) / scale;
            const float visible_h = float(height) / scale;
            SDL_FRect source{(image_w - visible_w) * .5f,
                             (image_h - visible_h) * .5f, visible_w, visible_h};
            SDL_FRect dest{0, 0, float(width), float(height)};
            SDL_RenderTexture(renderer, art, &source, &dest);
        }
    }
    // Keep the illustration visible while protecting text contrast.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 27, 9, 27, 135);
    SDL_RenderFillRect(renderer, nullptr);
    // The readability overlay must cover the entire renderer surface.
    // A former 78%-width rectangle created a visible vertical seam.
    const SDL_FRect text_panel{0, 0, float(width), float(height)};
    SDL_SetRenderDrawColor(renderer, 24, 7, 24, 125);
    SDL_RenderFillRect(renderer, &text_panel);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

static const char* kTitleId = "CUSA28763";

static bool valid_game_dir(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p / "sce_sys" / "param.sfo", ec);
}

static std::map<std::string, std::string> read_ini(const fs::path& path) {
    std::map<std::string, std::string> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        auto eq = line.find('=');
        if (eq != std::string::npos) out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}

static void write_ini(const fs::path& path, const std::map<std::string, std::string>& kv) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    for (auto& [k, v] : kv) f << k << '=' << v << '\n';
}

struct Choice { const char* label; const char* arg; };
static const Choice kUpscalers[] = {{"Game setting (default)", ""}, {"Off", "off"},
                                    {"FSR 3.1", "fsr3"}, {"FSR 4", "fsr4"}};
static const Choice kResolutions[] = {{"Game setting (default)", ""}, {"1080p", "1080"},
                                      {"1440p", "1440"}, {"4K", "4k"}};

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::wstring quote(const std::wstring& a) {
    // CommandLineToArgvW-compatible quoting.
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') { r.append(bs * 2 + 1, L'\\'); r += c; }
        else { r.append(bs, L'\\'); r += c; }
        bs = 0;
    }
    r.append(bs * 2, L'\\');
    return r + L"\"";
}
// Start the Python launcher; returns a process handle or null (error filled in).
static HANDLE spawn_launcher(const fs::path& root, const std::vector<std::string>& args,
                             std::string& error) {
    const fs::path python = root / "python" / "python.exe";
    const fs::path script = root / "scripts" / "ps4rt_launch.py";
    std::error_code ec;
    if (!fs::is_regular_file(python, ec) || !fs::is_regular_file(script, ec)) {
        error = "Launcher files are missing (python\\python.exe or scripts\\ps4rt_launch.py). Re-extract the full package.";
        return nullptr;
    }
    std::wstring cmd = quote(python.wstring()) + L" " + quote(script.wstring());
    for (auto& a : args) cmd += L" " + quote(widen(a));
    // Same environment as Play-Game.bat.
    std::wstring path = (root / "bin").wstring();
    wchar_t old[32768];
    DWORD n = GetEnvironmentVariableW(L"PATH", old, 32768);
    if (n > 0 && n < 32768) path += L";" + std::wstring(old, n);
    SetEnvironmentVariableW(L"PATH", path.c_str());
    SetEnvironmentVariableW(L"PS4RT_FSR4_DIR", (root / "fsr4_shaders").wstring().c_str());
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                        root.wstring().c_str(), &si, &pi)) {
        error = "Couldn't start the game launcher (error " + std::to_string(GetLastError()) + ").";
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
#endif

struct FolderPicker {
    std::mutex m;
    bool waiting = false, completed = false;
    std::string path;
};
static void SDLCALL folder_cb(void* ud, const char* const* paths, int) {
    auto& s = *static_cast<FolderPicker*>(ud);
    std::lock_guard lock(s.m);
    s.path = (paths && paths[0]) ? paths[0] : "";
    s.completed = true;
}

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return 2;
    const char* base = SDL_GetBasePath();
    fs::path bindir = base && *base ? fs::path(base) : fs::current_path();
    while (!bindir.empty() && (bindir.filename().empty() || bindir.filename() == ".")) bindir = bindir.parent_path();
    const fs::path root = bindir.parent_path();
    const fs::path game_file = root / "out" / kTitleId / "game-folder.txt";
    const fs::path ini_file = root / "out" / "launcher.ini";

    auto ini = read_ini(ini_file);
    int upscaler = 0, resolution = 0;
    for (int i = 0; i < 4; ++i) {
        if (ini["upscaler"] == kUpscalers[i].arg) upscaler = i;
        if (ini["resolution"] == kResolutions[i].arg) resolution = i;
    }
    char game[4096] = {};
    {
        std::ifstream f(game_file);
        std::string line;
        if (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            std::snprintf(game, sizeof(game), "%s", line.c_str());
        }
    }

    SDL_Window* window = SDL_CreateWindow("NoisyFairy | Mobile Suit Gundam Battle Operation Code Fairy",
                                          760, 460, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) { SDL_Quit(); return 2; }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    configure_setup_font();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 9.0f;
    style.FrameRounding = 5.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(.14f, .08f, .15f, 0);
    style.Colors[ImGuiCol_Button] = ImVec4(.58f, .21f, .40f, 1);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(.77f, .33f, .55f, 1);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(.88f, .41f, .60f, 1);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(.24f, .14f, .23f, 1);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    SDL_Texture* art = load_setup_art(renderer);

    FolderPicker picker;
    std::string error;
#ifdef _WIN32
    HANDLE child = nullptr;
    bool child_dlc = false;
#endif
    bool done = false;
    while (!done) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) done = true;
        }
        if (picker.waiting) {
            std::lock_guard lock(picker.m);
            if (picker.completed) {
                picker.completed = false;
                picker.waiting = false;
                if (!picker.path.empty()) { std::snprintf(game, sizeof(game), "%s", picker.path.c_str()); error.clear(); }
            }
        }
#ifdef _WIN32
        if (child && WaitForSingleObject(child, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(child, &code);
            CloseHandle(child);
            child = nullptr;
            if (code != 0 && !child_dlc)
                error = "The game exited with code " + std::to_string(code) +
                        ". Check the console window text (or docs\\RUNNING_WINDOWS.md) for details.";
            SDL_RestoreWindow(window);
            SDL_RaiseWindow(window);
        }
        const bool busy = child != nullptr;
#else
        const bool busy = false;
#endif
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored({.98f, .55f, .75f, 1}, "NOISYFAIRY / MOBILE SUIT GUNDAM BATTLE OPERATION Code Fairy");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("Game folder (extracted %s)", kTitleId);
        ImGui::SetNextItemWidth(io.DisplaySize.x - 190);
        ImGui::InputText("##game", game, sizeof(game));
        ImGui::SameLine();
        ImGui::BeginDisabled(picker.waiting || busy);
        if (ImGui::Button("Browse...", {120, 0})) {
            picker.waiting = true;
            SDL_ShowOpenFolderDialog(folder_cb, &picker, window, game[0] ? game : nullptr, false);
        }
        ImGui::EndDisabled();
        const bool ok = valid_game_dir(fs::path(game));
        if (!game[0]) ImGui::TextColored({1, .8f, .4f, 1}, "Choose the folder that contains sce_sys\\param.sfo.");
        else if (ok) ImGui::TextColored({.4f, .9f, .5f, 1}, "Game folder found.");
        else ImGui::TextColored({1, .5f, .5f, 1}, "No sce_sys\\param.sfo here. Pick the extracted CUSA folder itself.");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("Upscaler", kUpscalers[upscaler].label)) {
            for (int i = 0; i < 4; ++i)
                if (ImGui::Selectable(kUpscalers[i].label, i == upscaler)) upscaler = i;
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("Output resolution", kResolutions[resolution].label)) {
            for (int i = 0; i < 4; ++i)
                if (ImGui::Selectable(kResolutions[i].label, i == resolution)) resolution = i;
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("\"Game setting\" keeps whatever you chose in the in-game menu.");
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::BeginDisabled(!ok || busy);
        bool play = ImGui::Button(busy ? "Running..." : "Play", {210, 46});
        ImGui::SameLine();
        bool dlc = ImGui::Button("Install DLC...", {170, 46});
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Quit", {110, 46}) && !busy) done = true;
        if (!ok && game[0]) ImGui::TextDisabled("Play is enabled once the folder is valid.");
        if (busy) ImGui::TextColored({.98f, .55f, .75f, 1}, child_dlc ? "Installing DLC..." : "Game running. Close it to return here.");
        if (!error.empty()) { ImGui::Spacing(); ImGui::TextColored({1, .5f, .5f, 1}, "%s", error.c_str());  }
        ImGui::Spacing();
        ImGui::TextDisabled("First run on v01.00 files asks for the v01.03 update PKG. Back up your game folder before updating.");
        ImGui::End();
        ImGui::Render();
        draw_setup_background(renderer, art);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

#ifdef _WIN32
        if ((play || dlc) && ok && !busy) {
            ini["upscaler"] = kUpscalers[upscaler].arg;
            ini["resolution"] = kResolutions[resolution].arg;
            write_ini(ini_file, ini);
            std::vector<std::string> args = {"--game", game};
            if (dlc) args.push_back("--install-dlc");
            else {
                if (*kUpscalers[upscaler].arg) { args.push_back("--upscaler"); args.push_back(kUpscalers[upscaler].arg); }
                if (*kResolutions[resolution].arg) { args.push_back("--output-resolution"); args.push_back(kResolutions[resolution].arg); }
            }
            error.clear();
            child_dlc = dlc;
            child = spawn_launcher(root, args, error);
            if (child && play) SDL_MinimizeWindow(window);
        }
#endif
        SDL_Delay(16);
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_Quit();
    return 0;
}
