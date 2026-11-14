// SPDX-License-Identifier: GPL-2.0-or-later
// NoisyFairy launcher: borderless SDL3 + Dear ImGui front end for scripts/ps4rt_launch.py (Windows).
// Two tabs: Launcher (game folder, Play, DLC) and Options (the same ps4rt.ini keys as the in-game menu,
// plus an optional log file under out/CUSA28763/user/log).
#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "ps4rt_png.h"

namespace fs = std::filesystem;

static const char* kTitleId = "CUSA28763";
static const char* kVersion = "0.5 dev";
static constexpr int kW = 1152, kH = 648;
static constexpr float kCol = 452.0f;       // width of the left column
static const ImVec4 kPurple{.60f, .27f, .88f, 1}, kPurpleHi{.70f, .38f, .95f, 1},
                    kPurpleLo{.45f, .17f, .72f, 1}, kWhite{1, 1, 1, 1},
                    kDim{.06f, .04f, .10f, .72f}, kLabel{.80f, .70f, 1.0f, 1};

// ---------------------------------------------------------------- ini helpers
struct Ini {
    fs::path path;
    std::vector<std::string> lines;
    void load() {
        lines.clear();
        std::ifstream f(path);
        std::string l;
        while (std::getline(f, l)) {
            while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
            lines.push_back(l);
        }
    }
    bool find(const std::string& key, std::string& value) const {
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            auto eq = it->find('=');
            if (eq != std::string::npos && it->compare(0, eq, key) == 0) {
                value = it->substr(eq + 1);
                return true;
            }
        }
        return false;
    }
    std::string str(const std::string& key, const std::string& def) const {
        std::string v;
        return find(key, v) ? v : def;
    }
    int num(const std::string& key, int def) const {
        std::string v;
        if (!find(key, v)) return def;
        try { return std::stoi(v); } catch (...) { return def; }
    }
    void set(const std::string& key, const std::string& value) {
        bool done = false;
        for (auto& l : lines) {
            auto eq = l.find('=');
            if (eq != std::string::npos && l.compare(0, eq, key) == 0) { l = key + "=" + value; done = true; }
        }
        if (!done) lines.push_back(key + "=" + value);
        save();
    }
    void save() const {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream f(path, std::ios::trunc);
        for (auto& l : lines) f << l << '\n';
    }
};

struct Opt { const char* label; const char* key; };   // key stored in ini
static const Opt kDisplayModes[] = {{"Windowed", "windowed"}, {"Borderless", "borderless"}, {"Fullscreen", "fullscreen"}};
static const Opt kFpsLimits[] = {{"60 (original)", "0"}, {"30", "30"}};
// Internal resolutions (ini key, width, height), same order as the in-game menu.
struct Res { const char* label; const char* key; int w, h; };
static const Res kResolutions[] = {
    {"1024 x 576", "540p", 1024, 576}, {"1280 x 720", "720p", 1280, 720},
    {"1920 x 1080 (original)", "native", 1920, 1080}, {"2560 x 1440", "1440p", 2560, 1440},
    {"3840 x 2160 (4K)", "2160p", 3840, 2160}, {"2560 x 1080 (21:9)", "2560x1080", 2560, 1080},
    {"3440 x 1440 (21:9)", "3440x1440", 3440, 1440}, {"3840 x 1440 (24:9)", "3840x1440", 3840, 1440},
    {"3840 x 1080 (32:9)", "3840x1080", 3840, 1080}, {"5120 x 1440 (32:9)", "5120x1440", 5120, 1440}};
static const Res kOutputs[] = {
    {"Native (AA only)", "native", 0, 0}, {"1920 x 1080", "1920x1080", 1920, 1080},
    {"2560 x 1440", "2560x1440", 2560, 1440}, {"3840 x 2160 (4K)", "3840x2160", 3840, 2160},
    {"3440 x 1440", "3440x1440", 3440, 1440}, {"3840 x 1440", "3840x1440", 3840, 1440},
    {"5120 x 1440", "5120x1440", 5120, 1440}};
static const Opt kUpscalers[] = {{"Off", "off"}, {"FSR 1", "fsr1"}, {"FSR 3.1", "fsr3"}, {"FSR 4 v07", "fsr4"}};
static const Opt kQualities[] = {{"Max (original)", "max"}, {"Medium", "medium"}, {"Low", "low"}};
static const Opt kAniso[] = {{"Game default", "0"}, {"2x", "2"}, {"4x", "4"}, {"8x", "8"}, {"16x", "16"}};
#define COUNT(a) (int(sizeof(a) / sizeof((a)[0])))

// ---------------------------------------------------------------- misc helpers
static bool valid_game_dir(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p / "sce_sys" / "param.sfo", ec);
}

static SDL_Texture* load_texture(SDL_Renderer* r, const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return nullptr;
    Ps4RtPng::Image image;
    if (!Ps4RtPng::LoadRgba(path.string().c_str(), image)) return nullptr;
    SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, image.width, image.height);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, image.rgba.data(), image.width * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    return t;
}

// Soft black gradient that is strongest in the bottom-left corner and fades out toward the right
// and the top, leaving the artwork's characters untouched.
static SDL_Texture* make_gradient(SDL_Renderer* r) {
    constexpr int w = 384, h = 216;
    std::vector<unsigned char> px(w * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float u = (x + .5f) / w, v = (y + .5f) / h;
            const float d = std::sqrt((u / .84f) * (u / .84f) + ((1.f - v) / 2.0f) * ((1.f - v) / 2.0f));
            float a = d >= 1.f ? 0.f : 1.f - std::pow(d, 1.35f);
            a = .97f * a;
            unsigned char* p = &px[(y * w + x) * 4];
            p[0] = 6; p[1] = 4; p[2] = 12; p[3] = (unsigned char)std::lround(std::clamp(a, 0.f, 1.f) * 255.f);
        }
    }
    SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, px.data(), w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    return t;
}

struct Fonts { ImFont* body = nullptr; ImFont* bold = nullptr; ImFont* big = nullptr; };
static Fonts load_fonts(const fs::path& assets) {
    Fonts f;
    std::vector<fs::path> regular, bold;
    if (const char* win = std::getenv("WINDIR")) {
        regular.push_back(fs::path(win) / "Fonts" / "segoeui.ttf");
        bold.push_back(fs::path(win) / "Fonts" / "segoeuib.ttf");
    }
    regular.push_back(assets / "DejaVuSans.ttf");
    bold.push_back(assets / "DejaVuSans.ttf");
    ImGuiIO& io = ImGui::GetIO();
    std::error_code ec;
    for (auto& p : regular)
        if (fs::is_regular_file(p, ec) && (f.body = io.Fonts->AddFontFromFileTTF(p.string().c_str(), 17.f))) break;
    for (auto& p : bold)
        if (fs::is_regular_file(p, ec)) {
            f.bold = io.Fonts->AddFontFromFileTTF(p.string().c_str(), 18.f);
            f.big = io.Fonts->AddFontFromFileTTF(p.string().c_str(), 28.f);
            if (f.bold && f.big) break;
        }
    return f;
}

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

// ---------------------------------------------------------------- process launch (Windows)
#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::wstring quote(const std::wstring& a) {
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

// Starts the Python launcher. With log_path set, the console is hidden and stdout/stderr go to that file
// (so crash details survive); otherwise the usual console window opens.
static HANDLE spawn_launcher(const fs::path& root, const std::vector<std::string>& args,
                             const fs::path& log_path, std::string& error) {
    const fs::path python = root / "python" / "python.exe";
    const fs::path script = root / "scripts" / "ps4rt_launch.py";
    std::error_code ec;
    if (!fs::is_regular_file(python, ec) || !fs::is_regular_file(script, ec)) {
        error = "Launcher files are missing (python\\python.exe, scripts\\ps4rt_launch.py). Re-extract the full package.";
        return nullptr;
    }
    std::wstring cmd = quote(python.wstring()) + L" " + quote(script.wstring());
    for (auto& a : args) cmd += L" " + quote(widen(a));
    std::wstring path = (root / "bin").wstring();
    wchar_t old[32768];
    DWORD n = GetEnvironmentVariableW(L"PATH", old, 32768);
    if (n > 0 && n < 32768) path += L";" + std::wstring(old, n);
    SetEnvironmentVariableW(L"PATH", path.c_str());
    SetEnvironmentVariableW(L"PS4RT_FSR4_DIR", (root / "fsr4_shaders").wstring().c_str());
    SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    DWORD flags = CREATE_NEW_CONSOLE;
    BOOL inherit = FALSE;
    HANDLE log = INVALID_HANDLE_VALUE, nul = INVALID_HANDLE_VALUE;
    if (!log_path.empty()) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        fs::create_directories(log_path.parent_path(), ec);
        log = CreateFileW(log_path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        if (log != INVALID_HANDLE_VALUE && nul != INVALID_HANDLE_VALUE) {
            std::string head = "NoisyFairy launcher log\nCommand: " + std::string(cmd.begin(), cmd.end()) + "\n\n";
            DWORD written = 0;
            WriteFile(log, head.data(), (DWORD)head.size(), &written, nullptr);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = nul;
            si.hStdOutput = log;
            si.hStdError = log;
            flags = CREATE_NO_WINDOW;
            inherit = TRUE;
        } else {
            if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
            if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
            log = nul = INVALID_HANDLE_VALUE;
        }
    }
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, inherit, flags, nullptr,
                                   root.wstring().c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        error = "Couldn't start the game launcher (Windows error " + std::to_string(GetLastError()) + ").";
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static SDL_HitTestResult SDLCALL hit_test(SDL_Window*, const SDL_Point* p, void*) {
    // Drag from the top strip, except over the minimize/close buttons.
    if (p->y < 64 && p->x < kW - 120) return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}
#endif

// ---------------------------------------------------------------- widgets
static bool nf_button(const char* label, ImVec2 size, bool active = true, bool filled = true) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, kWhite);
    if (filled) {
        ImGui::PushStyleColor(ImGuiCol_Button, active ? kPurple : ImVec4(.20f, .12f, .32f, .80f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kPurpleHi);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kPurpleLo);
    }
    const bool pressed = ImGui::Button(label, size);
    if (filled) ImGui::PopStyleColor(3);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    return pressed;
}

static void section(const char* title) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kLabel);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

static bool combo(const char* label, const char* id, const Opt* opts, int count, Ini& ini, const char* key,
                  const char* def) {
    const std::string cur = ini.str(key, def);
    int sel = 0;
    for (int i = 0; i < count; ++i) if (cur == opts[i].key) sel = i;
    bool changed = false;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(190);
    ImGui::SetNextItemWidth(kCol - 190 - 12);
    if (ImGui::BeginCombo(id, opts[sel].label)) {
        for (int i = 0; i < count; ++i) {
            if (ImGui::Selectable(opts[i].label, i == sel)) { ini.set(key, opts[i].key); changed = true; }
        }
        ImGui::EndCombo();
    }
    return changed;
}

static void checkbox(const char* label, const char* id, Ini& ini, const char* key, bool def) {
    bool v = ini.num(key, def ? 1 : 0) != 0;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(190);
    if (ImGui::Checkbox(id, &v)) ini.set(key, v ? "1" : "0");
}

// Volume unlock keys are stored as add-on folders (player-content\dlc, or a pre-extracted dlc\CUSA28763).
static int installed_keys(const fs::path& root) {
    int n = 0;
    std::error_code ec;
    for (const fs::path& dir : {root / "out" / kTitleId / "player-content" / "dlc", root / "dlc" / kTitleId}) {
        for (auto& e : fs::directory_iterator(dir, ec))
            if (e.is_directory(ec) && fs::is_regular_file(e.path() / "sce_sys" / "param.sfo", ec)) ++n;
    }
    return n;
}
static constexpr int kKeysNeeded = 3;

static std::string stamp() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::localtime(&t));
    return buf;
}

static void prune_logs(const fs::path& dir, size_t keep) {
    std::error_code ec;
    std::vector<fs::path> logs;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".log" &&
            e.path().filename().string().rfind("NoisyFairy-", 0) == 0) logs.push_back(e.path());
    std::sort(logs.begin(), logs.end());
    while (logs.size() > keep) { fs::remove(logs.front(), ec); logs.erase(logs.begin()); }
}

// ---------------------------------------------------------------- main
int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return 2;
    const char* base = SDL_GetBasePath();
    fs::path bindir = base && *base ? fs::path(base) : fs::current_path();
    while (!bindir.empty() && (bindir.filename().empty() || bindir.filename() == ".")) bindir = bindir.parent_path();
    const fs::path root = bindir.parent_path();
    const fs::path assets = root / "launcher";
    const fs::path out_dir = root / "out" / kTitleId;
    const fs::path game_file = out_dir / "game-folder.txt";
    const fs::path log_dir = out_dir / "user" / "log";

    Ini game_ini{out_dir / "ps4rt.ini"};      // shared with the in-game menu
    game_ini.load();
    Ini launcher_ini{root / "out" / "launcher.ini"};
    launcher_ini.load();

    char game[4096] = {};
    {
        std::ifstream f(game_file);
        std::string line;
        if (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            std::snprintf(game, sizeof(game), "%s", line.c_str());
        }
    }

    SDL_Window* window = SDL_CreateWindow("NoisyFairy", kW, kH, SDL_WINDOW_BORDERLESS);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) { SDL_Quit(); return 2; }
#ifdef _WIN32
    {   // Rounded window corners.
        HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        if (hwnd) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int radius = std::max(16, int((rc.right - rc.left) * 28 / kW));
            SetWindowRgn(hwnd, CreateRoundRectRgn(0, 0, rc.right - rc.left + 1, rc.bottom - rc.top + 1, radius, radius), TRUE);
        }
        SDL_SetWindowHitTest(window, hit_test, nullptr);
    }
#endif
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    const Fonts fonts = load_fonts(assets);
    if (!fonts.body) io.Fonts->AddFontDefault();
    auto& st = ImGui::GetStyle();
    st.WindowRounding = 0; st.FrameRounding = 9; st.GrabRounding = 9; st.PopupRounding = 9; st.ChildRounding = 9;
    st.ScrollbarRounding = 9;
    st.FramePadding = {12, 7}; st.ItemSpacing = {10, 9}; st.ScrollbarSize = 12;
    st.PopupBorderSize = 1; st.FrameBorderSize = 1;
    auto& c = st.Colors;
    c[ImGuiCol_WindowBg] = {0, 0, 0, 0};
    c[ImGuiCol_ChildBg] = {0, 0, 0, 0};
    c[ImGuiCol_PopupBg] = {.08f, .05f, .14f, .97f};
    c[ImGuiCol_Border] = {.75f, .62f, 1.f, .55f};
    c[ImGuiCol_FrameBg] = kDim;
    c[ImGuiCol_FrameBgHovered] = {.20f, .12f, .32f, .85f};
    c[ImGuiCol_FrameBgActive] = {.28f, .16f, .44f, .90f};
    c[ImGuiCol_Button] = kPurple; c[ImGuiCol_ButtonHovered] = kPurpleHi; c[ImGuiCol_ButtonActive] = kPurpleLo;
    c[ImGuiCol_Header] = {.45f, .20f, .70f, .70f}; c[ImGuiCol_HeaderHovered] = {.60f, .27f, .88f, .85f};
    c[ImGuiCol_HeaderActive] = kPurple;
    c[ImGuiCol_CheckMark] = {1, 1, 1, 1};
    c[ImGuiCol_SliderGrab] = kPurpleHi; c[ImGuiCol_SliderGrabActive] = kPurple;
    c[ImGuiCol_Separator] = {.75f, .62f, 1.f, .35f};
    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0}; c[ImGuiCol_ScrollbarGrab] = {.60f, .27f, .88f, .8f};
    c[ImGuiCol_Text] = {.97f, .96f, 1.f, 1};
    c[ImGuiCol_TextDisabled] = {.72f, .68f, .82f, 1};
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    SDL_Texture* background = load_texture(renderer, assets / "background.png");
    SDL_Texture* logo = load_texture(renderer, assets / "logo.png");
    SDL_Texture* gradient = make_gradient(renderer);

    FolderPicker picker;
    std::string error;
    fs::path last_log;
    int keys = installed_keys(root);
    std::string notice;
    int tab = 0;
    bool log_to_file = launcher_ini.num("log_to_file", 1) != 0;
    bool child_dlc = false;
#ifdef _WIN32
    HANDLE child = nullptr;
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
        bool busy = false;
#ifdef _WIN32
        if (child && WaitForSingleObject(child, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(child, &code);
            CloseHandle(child);
            child = nullptr;
            game_ini.load();   // the game rewrites ps4rt.ini
            keys = installed_keys(root);
            char hex[32];
            std::snprintf(hex, sizeof(hex), "0x%08lX", (unsigned long)code);
            if (child_dlc) {
                if (code == 10) notice = "Volume keys installed. They take effect the next time the game starts.";
                else if (code != 0)
                    error = std::string("Installing the volume keys failed (exit code ") + hex +
                            "). Details are in the newest log (Options > Open log folder).";
            } else if (code != 0) {
                error = std::string("The game closed unexpectedly (exit code ") + hex + ").";
                if (!last_log.empty()) error += " Details were saved to the log file; use Options > Open log folder.";
                else error += " Turn on \"Write log file\" in Options to capture details next time.";
            }
            SDL_RestoreWindow(window);
            SDL_RaiseWindow(window);
        }
        busy = child != nullptr;
#endif
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
        ImGui::SetNextWindowSize({float(kW), float(kH)}, ImGuiCond_Always);
        ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (fonts.body) ImGui::PushFont(fonts.body, 17.f);

        // Window controls, top right.
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, kWhite);
            const ImVec2 sz{40, 34};
            ImGui::SetCursorPos({kW - 2 * 40 - 10 - 24, 18});
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (ImGui::Button("##min", sz)) SDL_MinimizeWindow(window);
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            dl->AddLine({a.x + 12, (a.y + b.y) / 2 + 3}, {b.x - 12, (a.y + b.y) / 2 + 3}, IM_COL32_WHITE, 2.5f);
            ImGui::SameLine(0, 10);
            if (ImGui::Button("##close", sz) && !busy) done = true;
            a = ImGui::GetItemRectMin(); b = ImGui::GetItemRectMax();
            dl->AddLine({a.x + 13, a.y + 10}, {b.x - 13, b.y - 10}, IM_COL32_WHITE, 2.5f);
            dl->AddLine({b.x - 13, a.y + 10}, {a.x + 13, b.y - 10}, IM_COL32_WHITE, 2.5f);
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }

        const float left = 36.f, top = 188.f;
        ImGui::SetCursorPos({left, top});
        ImGui::BeginChild("##content", {kCol, kH - top - 96.f}, false, ImGuiWindowFlags_NoBackground);

        if (tab == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, kLabel);
            ImGui::TextUnformatted("GAME FOLDER");
            ImGui::PopStyleColor();
            ImGui::SetNextItemWidth(kCol - 140);
            ImGui::InputText("##game", game, sizeof(game));
            ImGui::SameLine();
            ImGui::BeginDisabled(picker.waiting || busy);
            if (nf_button("Browse", {120, 0})) {
                picker.waiting = true;
                SDL_ShowOpenFolderDialog(folder_cb, &picker, window, game[0] ? game : nullptr, false);
            }
            ImGui::EndDisabled();
            const bool ok = valid_game_dir(fs::path(game));
            if (!game[0]) ImGui::TextColored({1, .85f, .45f, 1}, "Choose your extracted %s folder.", kTitleId);
            else if (ok) ImGui::TextColored({.50f, .95f, .60f, 1}, "Game folder found.");
            else ImGui::TextColored({1, .55f, .55f, 1}, "No sce_sys\\param.sfo in that folder.");
            ImGui::Dummy({0, 10});

            ImGui::BeginDisabled(!ok || busy);
            if (fonts.big) ImGui::PushFont(fonts.big, 28.f);
            const bool play = nf_button(busy ? (child_dlc ? "INSTALLING KEYS..." : "RUNNING...") : "PLAY", {kCol - 12, 68});
            if (fonts.big) ImGui::PopFont();
            if (fonts.bold) ImGui::PushFont(fonts.bold, 18.f);
            const bool show_keys = keys < kKeysNeeded;
            const bool dlc = show_keys && nf_button("Install Volume Keys...", {kCol - 12, 46});
            if (fonts.bold) ImGui::PopFont();
            ImGui::EndDisabled();

            if (!notice.empty() && error.empty()) {
                ImGui::Dummy({0, 4});
                ImGui::TextColored({.50f, .95f, .60f, 1}, "%s", notice.c_str());
            }
            if (!error.empty()) {
                ImGui::Dummy({0, 4});
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .62f, .62f, 1));
                ImGui::PushTextWrapPos(kCol - 8);
                ImGui::TextUnformatted(error.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            } else if (busy) {
                ImGui::TextDisabled("%s", child_dlc ? "Installing volume keys..." : "Game running. This window returns when it closes.");
            }
            ImGui::Dummy({0, 2});
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(kCol - 8);
            ImGui::TextUnformatted("First run on v01.00 files asks for the v01.03 update PKG. Back up your game folder before updating.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
#ifdef _WIN32
            if ((play || dlc) && ok && !busy) {
                std::vector<std::string> args = {"--game", game};
                if (dlc) args.push_back("--install-dlc");
                error.clear();
                notice.clear();
                fs::path log;
                if (log_to_file || dlc) {   // the key installer always logs: it has no console of its own
                    log = log_dir / (std::string(dlc ? "NoisyFairy-keys-" : "NoisyFairy-") + stamp() + ".log");
                    std::error_code ec;
                    fs::create_directories(log_dir, ec);
                    prune_logs(log_dir, 9);
                }
                last_log = log;
                child_dlc = dlc;
                child = spawn_launcher(root, args, log, error);
                if (!child) last_log.clear();
                else if (play) SDL_MinimizeWindow(window);
            }
#endif
        } else {
            ImGui::BeginChild("##opts", {0, 0}, false, ImGuiWindowFlags_NoBackground);
            section("DISPLAY");
            combo("Window mode", "##display_mode", kDisplayModes, COUNT(kDisplayModes), game_ini, "display_mode", "windowed");
            combo("Frame rate limit", "##fps_limit", kFpsLimits, COUNT(kFpsLimits), game_ini, "fps_limit", "0");
            checkbox("FPS counter", "##show_fps", game_ini, "show_fps", false);

            section("RENDERING (applied on restart)");
            int res_sel = 2;
            {
                const std::string cur = game_ini.str("render_resolution", "native");
                for (int i = 0; i < COUNT(kResolutions); ++i) if (cur == kResolutions[i].key) res_sel = i;
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Internal resolution");
                ImGui::SameLine(190);
                ImGui::SetNextItemWidth(kCol - 190 - 12);
                if (ImGui::BeginCombo("##render_resolution", kResolutions[res_sel].label)) {
                    for (int i = 0; i < COUNT(kResolutions); ++i)
                        if (ImGui::Selectable(kResolutions[i].label, i == res_sel)) {
                            game_ini.set("render_resolution", kResolutions[i].key);
                            res_sel = i;
                        }
                    ImGui::EndCombo();
                }
            }
            combo("Graphics quality", "##graphics_quality", kQualities, COUNT(kQualities), game_ini,
                  "graphics_quality", "max");
            combo("Anisotropic filtering", "##anisotropy", kAniso, COUNT(kAniso), game_ini, "anisotropy", "0");

            section("UPSCALING");
            combo("FSR", "##upscaler", kUpscalers, COUNT(kUpscalers), game_ini, "upscaler", "off");
            {
                // Outputs larger than the internal resolution with the same aspect ratio (as in-game).
                const Res& in = kResolutions[res_sel];
                std::string cur = game_ini.str("output_res", "native");
                auto allowed = [&](const Res& o) {
                    if (o.w == 0) return true;
                    const double a = double(in.w) / in.h, b = double(o.w) / o.h;
                    return o.w > in.w && o.h > in.h && std::abs(b - a) <= 0.01 * a;
                };
                int sel = 0;
                for (int i = 0; i < COUNT(kOutputs); ++i) if (cur == kOutputs[i].key && allowed(kOutputs[i])) sel = i;
                if (cur != kOutputs[sel].key) game_ini.set("output_res", kOutputs[sel].key);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("FSR upscale output");
                ImGui::SameLine(190);
                ImGui::SetNextItemWidth(kCol - 190 - 12);
                if (ImGui::BeginCombo("##output_res", kOutputs[sel].label)) {
                    for (int i = 0; i < COUNT(kOutputs); ++i)
                        if (allowed(kOutputs[i]) && ImGui::Selectable(kOutputs[i].label, i == sel))
                            game_ini.set("output_res", kOutputs[i].key);
                    ImGui::EndCombo();
                }
            }
            {
                int strength = game_ini.num("sharpen", 0) ? int(std::atof(game_ini.str("sharpness", "0").c_str()) * 100.0 + 0.5) : 0;
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Sharpening");
                ImGui::SameLine(190);
                ImGui::SetNextItemWidth(kCol - 190 - 12);
                if (ImGui::SliderInt("##sharpening", &strength, 0, 100, "%d%%")) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%.2f", strength / 100.0);
                    game_ini.set("sharpness", buf);
                    game_ini.set("sharpen", strength ? "1" : "0");
                }
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(kCol - 20);
            ImGui::TextUnformatted("FSR output has no effect while FSR is off. Picking the same size as the internal resolution gives FSR anti-aliasing only.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();

            section("LOGGING");
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Write log file");
                ImGui::SameLine(190);
                if (ImGui::Checkbox("##log", &log_to_file)) launcher_ini.set("log_to_file", log_to_file ? "1" : "0");
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::PushTextWrapPos(kCol - 20);
                ImGui::TextUnformatted("Saves everything the game prints to out\\CUSA28763\\user\\log, so crash details survive. Keeps the 10 newest logs. Attach the file to bug reports.");
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
#ifdef _WIN32
                if (nf_button("Open log folder", {200, 0})) {
                    std::error_code ec;
                    fs::create_directories(log_dir, ec);
                    ShellExecuteW(nullptr, L"open", log_dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
#endif
            }
            ImGui::Dummy({0, 6});
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(kCol - 20);
            ImGui::TextUnformatted("Changes are saved immediately and apply the next time the game starts. The in-game menu (Insert) edits the same settings.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::EndChild();
        }
        ImGui::EndChild();

        // Tab buttons, bottom left.
        ImGui::SetCursorPos({left, kH - 74.f});
        if (fonts.bold) ImGui::PushFont(fonts.bold, 18.f);
        if (nf_button("LAUNCHER", {160, 46}, tab == 0)) tab = 0;
        ImGui::SameLine(0, 12);
        if (nf_button("OPTIONS", {160, 46}, tab == 1)) tab = 1;
        if (fonts.bold) ImGui::PopFont();
        ImGui::SameLine(0, 18);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("NoisyFairy %s", kVersion);

        if (fonts.body) ImGui::PopFont();
        ImGui::End();
        ImGui::Render();

        SDL_SetRenderDrawColor(renderer, 10, 6, 18, 255);
        SDL_RenderClear(renderer);
        int ow = 0, oh = 0;
        SDL_GetRenderOutputSize(renderer, &ow, &oh);
        const float sx = float(ow) / kW, sy = float(oh) / kH;
        if (background) { SDL_FRect d{0, 0, float(ow), float(oh)}; SDL_RenderTexture(renderer, background, nullptr, &d); }
        if (gradient) { SDL_FRect d{0, 0, float(ow), float(oh)}; SDL_RenderTexture(renderer, gradient, nullptr, &d); }
        if (logo) {
            const float lw = 390.f, lh = lw * 249.f / 720.f;
            SDL_FRect d{34.f * sx, 22.f * sy, lw * sx, lh * sy};
            SDL_RenderTexture(renderer, logo, nullptr, &d);
        }
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
#ifdef _WIN32
    if (child) CloseHandle(child);
#endif
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_Quit();
    return 0;
}
