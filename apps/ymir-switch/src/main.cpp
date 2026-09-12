// Ymir - Sega Saturn emulator frontend for the Nintendo Switch (devkitA64).
//
// Minimal homebrew UI:
//   1. Browses the SD card (sdmc:/) to pick a Saturn IPL/BIOS ROM (512 KiB).
//   2. Browses the SD card to pick a Saturn disc image (.cue/.chd/.iso/.ccd/.mds).
//   3. Boots the disc on the ymir-core emulator using the software VDP renderer,
//      presents frames through SDL2 and outputs audio through SDL2 as well.
//
// Controls while browsing:
//   D-Pad / Left stick  move
//   A                  select / enter
//   B                  parent directory / back to browser while in-game
//   +                  exit
//
// This is intentionally a single-threaded, minimal implementation. The game
// configuration surface (video/audio options, cartridges, saves, ...) is
// intentionally left out for a later iteration.

#include <SDL.h>
#include <SDL_ttf.h>

#include <switch.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

#include <ymir/ymir.hpp>

#include <ymir/core/types.hpp>
#include <ymir/sys/memory_defs.hpp>
#include <ymir/hw/vdp/vdp.hpp>
#include <ymir/hw/scsp/scsp.hpp>
#include <ymir/hw/smpc/peripheral/peripheral_callbacks.hpp>
#include <ymir/hw/smpc/peripheral/peripheral_report.hpp>

// -----------------------------------------------------------------------------
// Misc helpers
// -----------------------------------------------------------------------------

namespace {

std::string GetLastPathComponent(const std::string &path) {
    std::string p = path;
    while (!p.empty() && p.back() == '/') {
        p.pop_back();
    }
    if (p.empty()) {
        return p;
    }
    auto slash = p.find_last_of('/');
    if (slash == std::string::npos) {
        return p;
    }
    return p.substr(slash + 1);
}

std::string GetParentPath(const std::string &path) {
    std::string p = path;
    while (!p.empty() && p.back() == '/') {
        p.pop_back();
    }
    auto slash = p.find_last_of('/');
    if (slash == std::string::npos) {
        return "";
    }
    return p.substr(0, slash + 1);
}

bool IsSupportedDiscExtension(const std::string &name) {
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext;
    for (auto c : name.substr(dot + 1)) {
        ext.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
    }
    return ext == "cue" || ext == "chd" || ext == "iso" || ext == "ccd" || ext == "mds";
}

std::string ToLower(std::string s) {
    for (auto &c : s) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

} // namespace

// -----------------------------------------------------------------------------
// Application state
// -----------------------------------------------------------------------------

struct FileEntry {
    std::string name;
    std::string fullPath;
    bool isDir = false;
    bool isGame = false;
    long long size = 0;
};

enum class Phase {
    PickIPL,
    PickGame,
    Running,
};

struct App {
    // SDL objects
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_GameController *controller = nullptr;
    TTF_Font *font = nullptr;
    SDL_AudioDeviceID audioDevice = 0;

    // Audio queue chunking (samples are produced by the emulator thread, which is
    // this same thread, so no locking is required here).
    std::vector<uint8_t> audioChunk;
    int audioChunkSamples = 0;

    // Menu state
    Phase phase = Phase::PickIPL;
    // Saturn BIOS files are kept in the shared GBAStation layout.
    std::string currentDir = "sdmc:/GBAStation/bios/saturn/";
    std::vector<FileEntry> entries;
    int selection = 0;
    int scroll = 0;
    std::string statusLine;

    // Pad / navigation edge detection
    bool prevUp = false, prevDown = false, prevLeft = false, prevRight = false;
    bool prevA = false, prevB = false, prevX = false, prevStart = false, prevPlus = false;

    // Game input state (set while Running)
    bool padUp = false, padDown = false, padLeft = false, padRight = false;
    bool padA = false, padB = false, padC = false, padX = false, padY = false;
    bool padZ = false, padL = false, padR = false, padStart = false;

    // Emulator
    std::unique_ptr<ymir::Saturn> saturn;
    bool iplLoaded = false;
    std::vector<uint8_t> iplImage;

    // Video frame hand-off (written from the software renderer callback, which
    // runs on the same thread as RunFrame()).
    std::vector<uint32_t> frameBuffer;
    int frameWidth = 0;
    int frameHeight = 0;
    bool frameAvailable = false;

    // Display texture, recreated only when the emulated resolution changes.
    SDL_Texture *displayTexture = nullptr;
    int displayTexW = 0;
    int displayTexH = 0;

    // FPS bookkeeping
    uint64_t lastTime = 0;
    int fpsFrames = 0;
    int fps = 0;
};

static App g_app;

// -----------------------------------------------------------------------------
// Core callbacks
// -----------------------------------------------------------------------------

static void OnVDP2ResolutionChanged(uint32_t width, uint32_t height, void *) {
    App &app = g_app;
    app.frameWidth = static_cast<int>(width);
    app.frameHeight = static_cast<int>(height);
}

static void OnSoftwareFrame(uint32_t *fb, uint32_t width, uint32_t height, void *) {
    App &app = g_app;
    if (fb == nullptr || width == 0 || height == 0) {
        return;
    }
    app.frameWidth = static_cast<int>(width);
    app.frameHeight = static_cast<int>(height);
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (app.frameBuffer.size() < count) {
        app.frameBuffer.resize(count);
    }
    std::memcpy(app.frameBuffer.data(), fb, count * sizeof(uint32_t));
    app.frameAvailable = true;
}

static void OnOutputSample(sint16 left, sint16 right, void *) {
    App &app = g_app;
    if (app.audioDevice == 0) {
        return;
    }
    uint8_t *out = app.audioChunk.data() + app.audioChunkSamples * 4;
    std::memcpy(out, &left, sizeof(left));
    std::memcpy(out + 2, &right, sizeof(right));
    ++app.audioChunkSamples;
    if (app.audioChunkSamples >= static_cast<int>(app.audioChunk.size() / 4)) {
        SDL_QueueAudio(app.audioDevice, app.audioChunk.data(),
                       static_cast<uint32_t>(app.audioChunkSamples * 4));
        app.audioChunkSamples = 0;
    }
}

static void ReadControlPad(ymir::peripheral::PeripheralReport &report, void *) {
    App &app = g_app;
    auto &pad = report.report.controlPad;

    uint16_t pressed = 0;
#define SET(bit, cond) \
    do { \
        if (cond) { \
            pressed |= static_cast<uint16_t>(ymir::peripheral::Button::bit); \
        } \
    } while (0)
    SET(Right, app.padRight);
    SET(Left, app.padLeft);
    SET(Down, app.padDown);
    SET(Up, app.padUp);
    SET(Start, app.padStart);
    SET(A, app.padA);
    SET(C, app.padC);
    SET(B, app.padB);
    SET(R, app.padR);
    SET(X, app.padX);
    SET(Y, app.padY);
    SET(Z, app.padZ);
    SET(L, app.padL);
#undef SET

    // The Saturn digital pad reads 1 for released and 0 for pressed.
    const uint16_t released = static_cast<uint16_t>(~pressed) & 0xFFF8u;
    pad.buttons = static_cast<ymir::peripheral::Button>(released);
}

// -----------------------------------------------------------------------------
// SDL / UI helpers
// -----------------------------------------------------------------------------

static SDL_Texture *RenderText(const std::string &text, SDL_Color color) {
    SDL_Surface *surface = TTF_RenderUTF8_Blended(g_app.font, text.c_str(), color);
    if (surface == nullptr) {
        return nullptr;
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(g_app.renderer, surface);
    SDL_FreeSurface(surface);
    return texture;
}

static void DrawText(const std::string &text, int x, int y, SDL_Color color, int *outW = nullptr,
                     int *outH = nullptr) {
    SDL_Texture *texture = RenderText(text, color);
    if (texture == nullptr) {
        return;
    }
    int w = 0, h = 0;
    SDL_QueryTexture(texture, nullptr, nullptr, &w, &h);
    SDL_Rect dst{x, y, w, h};
    SDL_RenderCopy(g_app.renderer, texture, nullptr, &dst);
    SDL_DestroyTexture(texture);
    if (outW != nullptr) {
        *outW = w;
    }
    if (outH != nullptr) {
        *outH = h;
    }
}

static void ClearAudioQueue();

// -----------------------------------------------------------------------------
// File browser
// -----------------------------------------------------------------------------

static void RefreshDirectory(const std::string &path, bool gamePhase) {
    g_app.entries.clear();
    g_app.selection = 0;
    g_app.scroll = 0;
    g_app.currentDir = path;

    // Pseudo "parent" entry (unless at the SD root).
    if (GetParentPath(path) != path) {
        g_app.entries.push_back(FileEntry{std::string(".."), GetParentPath(path), true, false, 0});
    }

    DIR *dir = opendir(path.c_str());
    if (dir == nullptr) {
        g_app.statusLine = "Failed to open " + path;
        return;
    }

    while (struct dirent *ent = readdir(dir)) {
        const std::string name = ent->d_name;
        if (name.empty() || name == ".") {
            continue;
        }
        if (name == "..") {
            continue;
        }
        const std::string full = path + name;
        struct stat st;
        bool isDir = false;
        if (stat(full.c_str(), &st) == 0) {
            isDir = S_ISDIR(st.st_mode);
        } else {
            // Fall back to d_type if stat is not helpful.
            isDir = (ent->d_type == DT_DIR);
        }
        if (isDir) {
            g_app.entries.push_back(FileEntry{name, full + "/", true, false, 0});
        } else if (!gamePhase || IsSupportedDiscExtension(name)) {
            g_app.entries.push_back(
                FileEntry{name, full, false, IsSupportedDiscExtension(name), st.st_size});
        }
    }
    closedir(dir);

    std::stable_sort(g_app.entries.begin(), g_app.entries.end(),
                     [](const FileEntry &a, const FileEntry &b) {
                         if (a.isDir != b.isDir) {
                             return a.isDir; // directories first
                         }
                         return ToLower(a.name) < ToLower(b.name);
                     });

    g_app.statusLine = path + " - " + std::to_string(g_app.entries.size()) + " entries";
}

static void PickCurrentEntry() {
    App &app = g_app;
    if (app.entries.empty()) {
        return;
    }
    const FileEntry &entry = app.entries[app.selection];

    if (entry.isDir) {
        RefreshDirectory(entry.fullPath, app.phase == Phase::PickGame);
        return;
    }

    if (app.phase == Phase::PickIPL) {
        // Load an IPL ROM image.
        FILE *f = fopen(entry.fullPath.c_str(), "rb");
        if (f == nullptr) {
            app.statusLine = "Could not open " + entry.name;
            return;
        }
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size != static_cast<long>(ymir::sys::kIPLSize)) {
            fclose(f);
            app.statusLine = entry.name +
                " is not a Saturn IPL ROM (expected exactly 512 KiB, got " + std::to_string(size) + " bytes)";
            return;
        }
        std::vector<uint8_t> image(static_cast<size_t>(size));
        const size_t got = fread(image.data(), 1, image.size(), f);
        fclose(f);
        if (got != image.size()) {
            app.statusLine = "Read error while loading " + entry.name;
            return;
        }
        app.iplImage = std::move(image);
        app.iplLoaded = true;
        app.statusLine = "IPL loaded: " + entry.name + ". Now pick a disc image.";
        app.phase = Phase::PickGame;
        RefreshDirectory(app.currentDir, true);
        return;
    }

    if (app.phase == Phase::PickGame) {
        if (!IsSupportedDiscExtension(entry.name)) {
            app.statusLine = entry.name + " is not a supported disc image.";
            return;
        }
        // Attempt to boot the selected game.
        ClearAudioQueue();

        app.saturn = std::make_unique<ymir::Saturn>();

        auto &cfg = app.saturn->configuration;
        cfg.swRenderer.threadedVDP1 = false;
        cfg.swRenderer.threadedVDP2 = false;
        cfg.swRenderer.threadedDeinterlacer = false;
        cfg.system.autodetectRegion = true;

        app.saturn->SetVideoStandard(ymir::core::config::sys::VideoStandard::NTSC);
        app.saturn->UsePreferredRegion();

        if (app.iplLoaded) {
            app.saturn->LoadIPL(std::span<uint8_t, ymir::sys::kIPLSize>(app.iplImage.data()));
        }

        ymir::media::Disc disc;
        bool loaded = false;
        ymir::media::CbLoaderMessage cb = [](ymir::media::MessageType type, std::string msg) {
            if (type != ymir::media::MessageType::Debug) {
                g_app.statusLine = msg;
            }
        };
        if (!app.iplLoaded) {
            app.statusLine = "No IPL ROM loaded";
            loaded = false;
        } else {
            loaded = ymir::media::LoadDisc(entry.fullPath, disc, true, cb);
        }

        if (!loaded) {
            app.statusLine = "Failed to load " + entry.name;
            app.saturn.reset();
            return;
        }

        app.saturn->LoadDisc(std::move(disc));
        app.saturn->AutodetectRegion();

        // Configure the software renderer.
        auto rendererResult = app.saturn->VDP.UseSoftwareRenderer();
        if (!rendererResult) {
            app.statusLine = "Failed to initialize the software renderer";
            app.saturn.reset();
            return;
        }

        auto &renderer = app.saturn->VDP.GetRenderer();
        renderer.Callbacks.VDP2ResolutionChanged.Bind(&app, OnVDP2ResolutionChanged);
        app.saturn->VDP.SetSoftwareRenderCallback({nullptr, OnSoftwareFrame});
        app.saturn->SCSP.SetSampleCallback({nullptr, OnOutputSample});

        // Attach a control pad to port 1 and feed it from the Joy-Con/Pro controller.
        auto &port1 = app.saturn->SMPC.GetPeripheralPort1();
        port1.SetPeripheralReportCallback(ymir::peripheral::CBPeripheralReport{nullptr, ReadControlPad});
        port1.ConnectControlPad();

        app.frameWidth = 0;
        app.frameHeight = 0;
        app.frameAvailable = false;
        ClearAudioQueue();
        app.statusLine = "Running: " + entry.name + "  (B: back to menu)";
        app.phase = Phase::Running;
        app.lastTime = SDL_GetPerformanceCounter();
        app.fpsFrames = 0;
        return;
    }
}

// -----------------------------------------------------------------------------
// Input
// -----------------------------------------------------------------------------

static bool IsPressed(int sdlButton) {
    if (g_app.controller != nullptr) {
        return SDL_GameControllerGetButton(g_app.controller, static_cast<SDL_GameControllerButton>(sdlButton));
    }
    return false;
}

static int Axis(int sdlAxis) {
    if (g_app.controller != nullptr) {
        return SDL_GameControllerGetAxis(g_app.controller, static_cast<SDL_GameControllerAxis>(sdlAxis));
    }
    return 0;
}

static void ClearAudioQueue() {
    if (g_app.audioDevice != 0) {
        SDL_ClearQueuedAudio(g_app.audioDevice);
    }
}

static void UpdatePadState() {
    App &app = g_app;
    if (app.controller == nullptr) {
        return;
    }

    const int deadzone = 16000;
    const int lx = Axis(SDL_CONTROLLER_AXIS_LEFTX);
    const int ly = Axis(SDL_CONTROLLER_AXIS_LEFTY);

    const bool stickUp = ly < -deadzone;
    const bool stickDown = ly > deadzone;
    const bool stickLeft = lx < -deadzone;
    const bool stickRight = lx > deadzone;

    app.padUp = IsPressed(SDL_CONTROLLER_BUTTON_DPAD_UP) || stickUp;
    app.padDown = IsPressed(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || stickDown;
    app.padLeft = IsPressed(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || stickLeft;
    app.padRight = IsPressed(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || stickRight;
    app.padA = IsPressed(SDL_CONTROLLER_BUTTON_A);
    app.padB = IsPressed(SDL_CONTROLLER_BUTTON_B);
    app.padC = IsPressed(SDL_CONTROLLER_BUTTON_X);
    app.padX = IsPressed(SDL_CONTROLLER_BUTTON_Y);
    app.padY = IsPressed(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    app.padZ = IsPressed(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    app.padL = IsPressed(SDL_CONTROLLER_BUTTON_LEFTSTICK) || IsPressed(SDL_CONTROLLER_BUTTON_BACK);
    app.padR = IsPressed(SDL_CONTROLLER_BUTTON_RIGHTSTICK) || IsPressed(SDL_CONTROLLER_BUTTON_GUIDE);
    app.padStart = IsPressed(SDL_CONTROLLER_BUTTON_START);
}

// -----------------------------------------------------------------------------
// Rendering
// -----------------------------------------------------------------------------

static void PresentGameFrame() {
    App &app = g_app;
    if (!app.frameAvailable) {
        return;
    }

    int winW = 0, winH = 0;
    SDL_GetWindowSize(app.window, &winW, &winH);

    const int srcW = app.frameWidth;
    const int srcH = app.frameHeight;
    if (srcW <= 0 || srcH <= 0) {
        return;
    }

    // Compute the destination rectangle that preserves the aspect ratio.
    SDL_Rect dst;
    const float aspect = static_cast<float>(srcW) / static_cast<float>(srcH);
    if (static_cast<float>(winW) / static_cast<float>(winH) > aspect) {
        dst.h = winH;
        dst.w = static_cast<int>(winH * aspect);
    } else {
        dst.w = winW;
        dst.h = static_cast<int>(winW / aspect);
    }
    dst.x = (winW - dst.w) / 2;
    dst.y = (winH - dst.h) / 2;

    SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
    SDL_RenderClear(app.renderer);

    // Recreate the streaming texture only when the emulated resolution changes.
    if (app.displayTexture == nullptr || app.displayTexW != srcW || app.displayTexH != srcH) {
        if (app.displayTexture != nullptr) {
            SDL_DestroyTexture(app.displayTexture);
            app.displayTexture = nullptr;
        }
        app.displayTexture =
            SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, srcW, srcH);
        app.displayTexW = (app.displayTexture != nullptr) ? srcW : 0;
        app.displayTexH = (app.displayTexture != nullptr) ? srcH : 0;
        if (app.displayTexture != nullptr) {
            SDL_SetTextureBlendMode(app.displayTexture, SDL_BLENDMODE_BLEND);
        }
    }
    if (app.displayTexture != nullptr) {
        SDL_UpdateTexture(app.displayTexture, nullptr, app.frameBuffer.data(), srcW * sizeof(uint32_t));
        SDL_RenderCopy(app.renderer, app.displayTexture, nullptr, &dst);
    }

    // Simple status text.
    if (app.font != nullptr) {
        app.fpsFrames++;
        const uint64_t now = SDL_GetPerformanceCounter();
        const uint64_t elapsed = now - app.lastTime;
        if (elapsed >= SDL_GetPerformanceFrequency()) {
            app.fps = app.fpsFrames;
            app.fpsFrames = 0;
            app.lastTime = now;
        }
        DrawText(app.statusLine, 8, 4, SDL_Color{255, 255, 255, 255});
        const std::string perf = "FPS: " + std::to_string(app.fps) + "  " + std::to_string(srcW) + "x" +
                                 std::to_string(srcH);
        int tw = 0, th = 0;
        DrawText(perf, winW - 8, 4, SDL_Color{200, 200, 200, 255}, &tw, &th);
    }

    SDL_RenderPresent(app.renderer);
    app.frameAvailable = false;
}

static void PresentMenu() {
    App &app = g_app;
    int winW = 0, winH = 0;
    SDL_GetWindowSize(app.window, &winW, &winH);

    SDL_SetRenderDrawColor(app.renderer, 0x10, 0x14, 0x20, 255);
    SDL_RenderClear(app.renderer);

    if (app.font == nullptr) {
        SDL_RenderPresent(app.renderer);
        return;
    }

    const int titleSize = app.font != nullptr ? TTF_FontHeight(app.font) : 0;
    const int lineH = titleSize + 6;
    int y = 8;

    const char *phaseText = app.phase == Phase::PickIPL ? "Select a Saturn IPL / BIOS ROM (512 KiB)"
                                                        : "Select a Saturn disc image";
    DrawText(phaseText, 12, y, SDL_Color{0x8f, 0xc7, 0xff, 255});
    y += lineH + 4;

    const int visibleRows = (winH - y - 8 - lineH) / lineH;
    if (app.selection < app.scroll) {
        app.scroll = app.selection;
    }
    if (app.selection >= app.scroll + visibleRows) {
        app.scroll = app.selection - visibleRows + 1;
    }

    for (int i = app.scroll; i < static_cast<int>(app.entries.size()) && i < app.scroll + visibleRows; ++i) {
        const FileEntry &entry = app.entries[i];
        const bool selected = (i == app.selection);

        if (selected) {
            SDL_SetRenderDrawColor(app.renderer, 0x2a, 0x3f, 0x60, 255);
            SDL_Rect bar{0, y, winW, lineH};
            SDL_RenderFillRect(app.renderer, &bar);
        }

        std::string label = entry.isDir ? "[D] " : "[F] ";
        label += entry.name;
        if (!entry.isDir && entry.isGame) {
            label += "  (" + std::to_string(entry.size / 1024 / 1024) + " MB)";
        }

        SDL_Color color = selected ? SDL_Color{255, 255, 255, 255}
                                   : (entry.isDir ? SDL_Color{0x9f, 0xd4, 0xff, 255}
                                                  : SDL_Color{0xd0, 0xd0, 0xd0, 255});
        DrawText(label, 14, y + 3, color);
        y += lineH;
    }

    // Footer / help.
    SDL_SetRenderDrawColor(app.renderer, 0x08, 0x0a, 0x12, 255);
    SDL_Rect footer{0, winH - lineH - 8, winW, lineH + 8};
    SDL_RenderFillRect(app.renderer, &footer);

    std::string help = app.phase == Phase::PickIPL
                           ? "A: pick  |  B: up  |  +: exit"
                           : "A: boot disc  |  B: up  |  +: exit";
    if (!app.statusLine.empty()) {
        help += "    " + app.statusLine;
    }
    DrawText(help, 12, winH - lineH - 4, SDL_Color{0x9a, 0xa4, 0xb8, 255});

    SDL_RenderPresent(app.renderer);
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Services: SD card access and the bundled romfs (contains font.ttf).
    sdmcInit();
    romfsInit();

    App &app = g_app;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS) < 0) {
        return -1;
    }
    if (TTF_Init() < 0) {
        SDL_Quit();
        return -1;
    }

    app.window = SDL_CreateWindow("Ymir", 0, 0, 1280, 720, 0);
    if (app.window == nullptr) {
        TTF_Quit();
        SDL_Quit();
        return -1;
    }
    app.renderer =
        SDL_CreateRenderer(app.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (app.renderer == nullptr) {
        app.renderer = SDL_CreateRenderer(app.window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (app.renderer == nullptr) {
        SDL_DestroyWindow(app.window);
        TTF_Quit();
        SDL_Quit();
        return -1;
    }

    app.font = TTF_OpenFont("romfs:/font.ttf", 26);
    if (app.font == nullptr) {
        // Fall back to a built-in font if available (usually not on Switch).
        app.font = TTF_OpenFont("/System/share/fonts/font.ttf", 26);
    }

    // Open the first game controller (handheld or first Pro controller).
    app.controller = SDL_GameControllerOpen(0);

    // Audio: 44100 Hz, stereo, s16.
    {
        SDL_AudioSpec want{};
        want.freq = 44100;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        SDL_AudioSpec have{};
        app.audioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (app.audioDevice != 0) {
            app.audioChunk.assign(2048 * 4, 0); // room for 2048 stereo samples
            app.audioChunkSamples = 0;
            SDL_PauseAudioDevice(app.audioDevice, 0);
        }
    }

    // Start directly in the fixed BIOS directory requested by the frontend
    // layout. If it is missing, the directory browser will show an error and
    // the user can navigate back to sdmc:/ with B.
    RefreshDirectory("sdmc:/GBAStation/bios/saturn/", false);
    app.statusLine = "Pick a Saturn IPL (BIOS) ROM first";

    app.lastTime = SDL_GetPerformanceCounter();

    while (appletMainLoop()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                goto cleanup;
            }
        }

        UpdatePadState();

        if (app.phase == Phase::Running) {
            // --- In game ---------------------------------------------------
            // Leave back to the browser on B.
            const bool pressedB = app.padB && !app.prevB;
            if (pressedB) {
                if (app.displayTexture != nullptr) {
                    SDL_DestroyTexture(app.displayTexture);
                    app.displayTexture = nullptr;
                }
                app.saturn.reset();
                app.phase = Phase::PickGame;
                RefreshDirectory(app.currentDir, true);
                app.statusLine = "Select a Saturn disc image";
                ClearAudioQueue();
            } else {
                // Pace emulation against the audio queue (only relevant if the
                // emulator manages to run faster than real time, which is unlikely
                // on Switch hardware).
                while (SDL_GetQueuedAudioSize(app.audioDevice) > 44100 * 2 /* ~0.5 s */) {
                    SDL_Delay(2);
                }
                app.saturn->RunFrame();
                PresentGameFrame();
            }
        } else {
            // --- File browser ----------------------------------------------
            const bool navUp = app.padUp && !app.prevUp;
            const bool navDown = app.padDown && !app.prevDown;
            const bool pressedA = app.padA && !app.prevA;
            const bool pressedB = app.padB && !app.prevB;
            const bool pressedPlus = app.padStart && !app.prevStart;

            if (navUp) {
                if (app.selection > 0) {
                    app.selection--;
                }
            } else if (navDown) {
                if (app.selection < static_cast<int>(app.entries.size()) - 1) {
                    app.selection++;
                }
            }

            if (pressedA) {
                PickCurrentEntry();
            } else if (pressedB) {
                if (GetParentPath(app.currentDir) != app.currentDir) {
                    RefreshDirectory(GetParentPath(app.currentDir), app.phase == Phase::PickGame);
                }
            }

            if (pressedPlus) {
                goto cleanup;
            }

            PresentMenu();
        }

        app.prevUp = app.padUp;
        app.prevDown = app.padDown;
        app.prevA = app.padA;
        app.prevB = app.padB;
        app.prevX = app.padX;
        app.prevStart = app.padStart;
        app.prevPlus = app.padStart;
    }

cleanup:
    if (app.controller != nullptr) {
        SDL_GameControllerClose(app.controller);
    }
    if (app.audioDevice != 0) {
        SDL_CloseAudioDevice(app.audioDevice);
    }
    if (app.font != nullptr) {
        TTF_CloseFont(app.font);
    }
    if (app.displayTexture != nullptr) {
        SDL_DestroyTexture(app.displayTexture);
    }
    if (app.renderer != nullptr) {
        SDL_DestroyRenderer(app.renderer);
    }
    if (app.window != nullptr) {
        SDL_DestroyWindow(app.window);
    }
    TTF_Quit();
    SDL_Quit();

    romfsExit();
    sdmcExit();

    return 0;
}
