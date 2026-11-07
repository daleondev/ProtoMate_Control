// Linux-only black-box GUI input/capture adapter. Compiled with the native ABI;
// the optional FIFO is opened only when a test explicitly supplies its path.
#include "hal/linux/DisplayHost.hpp"
#include "ui/gui/hmi/Backend.hpp"
#include <SDL.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
namespace
{
    int descriptor = -1;
    bool attempted{};
    std::array<char, 1024> line{};
    unsigned used{};
    std::atomic_bool observing{};
    std::atomic_uint generation{}, flags{}, feedbackSources{}, controls{};
    std::array<std::atomic<double>, 12> pose{};
    void capturePresentation(const char* path)
    {
        std::array<double, 12> values{};
        unsigned state{}, sources{}, controlState{};
        bool coherent{};
        for (unsigned attempt = 0; attempt < 8 && !coherent; ++attempt) {
            const auto before = generation.load();
            if (before & 1U)
                continue;
            for (unsigned i = 0; i < values.size(); ++i)
                values[i] = pose[i].load();
            state = flags.load();
            sources = feedbackSources.load();
            controlState = controls.load();
            coherent = before == generation.load() && before != 0;
        }
        if (!coherent)
            return;
        char name[800];
        std::snprintf(name, sizeof(name), "%s.json", path);
        if (auto* file = std::fopen(name, "w")) {
            std::fprintf(file, "{\"flags\":%u,\"encoder_mask\":%u,\"controls\":%u,\"pose\":[",
                         state, sources, controlState);
            for (unsigned i = 0; i < values.size(); ++i)
                std::fprintf(file, "%s%.12g", i ? "," : "", values[i]);
            std::fputs("]}\n", file);
            std::fclose(file);
        }
    }
    void execute(const char* value, const hal::display::State& state, std::uint32_t window)
    {
        char command[32]{}, path[768]{};
        int x{}, y{};
        if (std::sscanf(value, "%31s", command) != 1)
            return;
        SDL_Event event{};
        if (!std::strcmp(command, "capture")) {
            if (std::sscanf(value, "%*s %767s", path) != 1)
                return;
            auto pixels = hal::linux::display::framebuffer(state.active);
            auto* surface =
              SDL_CreateRGBSurfaceFrom(pixels.data(), 1024, 600, 16, 2048, 0xf800, 0x7e0, 0x1f, 0);
            if (surface) {
                SDL_SaveBMP(surface, path);
                SDL_FreeSurface(surface);
                capturePresentation(path);
            }
            return;
        }
        if (!std::strcmp(command, "quit")) {
            event.type = SDL_QUIT;
            SDL_PushEvent(&event);
            return;
        }
        if (!std::strcmp(command, "focuslost")) {
            event.type = SDL_WINDOWEVENT;
            event.window.windowID = window;
            event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
            SDL_PushEvent(&event);
            return;
        }
        if (std::sscanf(value, "%*s %d %d", &x, &y) != 2)
            return;
        if (!std::strcmp(command, "size")) {
            SDL_SetWindowSize(SDL_GetWindowFromID(window), x, y);
            return;
        }
        if (!std::strcmp(command, "move")) {
            event.type = SDL_MOUSEMOTION;
            event.motion.x = x;
            event.motion.y = y;
            event.motion.state = SDL_BUTTON_LMASK;
        }
        else {
            event.type = !std::strcmp(command, "down") ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
            event.button.button = SDL_BUTTON_LEFT;
            event.button.x = x;
            event.button.y = y;
        }
        SDL_PushEvent(&event);
    }
}
extern "C" void hmi_native_observe(void*, const hal::display::State& state, std::uint32_t window)
{
    if (!attempted) {
        attempted = true;
        if (const auto* path = std::getenv("PROTOMATE_HMI_AUTOMATION"))
            descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        observing = descriptor >= 0;
    }
    if (descriptor < 0)
        return;
    char input[256];
    const auto n = read(descriptor, input, sizeof(input));
    for (ssize_t i = 0; i < n; ++i) {
        if (input[i] == '\n') {
            line[used] = 0;
            execute(line.data(), state, window);
            used = 0;
        }
        else if (used + 1 < line.size())
            line[used++] = input[i];
        else
            used = 0;
    }
}
extern "C" void hmi_native_close()
{
    observing = false;
    if (descriptor >= 0)
        close(descriptor);
    descriptor = -1;
}
extern "C" void hmi_observe_presentation(const ui::gui::hmi::Snapshot* snapshot)
{
    if (!observing.load())
        return;
    // Only atomics cross into the original OS/SDL thread. No ThreadX mutex,
    // allocation, IO or motion command is performed on the GUI worker here.
    generation.fetch_add(1);
    for (unsigned i = 0; i < 3; ++i) {
        pose[i] = snapshot->commanded[i];
        pose[3 + i] = snapshot->feedback[i];
        pose[6 + i] = snapshot->tcp[i];
        pose[9 + i] = snapshot->feedbackTcp[i];
    }
    flags = unsigned(snapshot->poseValid) | (unsigned(snapshot->feedbackValid) << 1) |
            (unsigned(snapshot->fresh) << 2);
    feedbackSources = unsigned(snapshot->axes[0].encoder) | (unsigned(snapshot->axes[1].encoder) << 1) |
                      (unsigned(snapshot->axes[2].encoder) << 2);
    controls = unsigned(snapshot->initialized) | (unsigned(snapshot->enabled) << 1) |
               (unsigned(snapshot->connected) << 2) | (unsigned(snapshot->fresh) << 3);
    generation.fetch_add(1);
}
