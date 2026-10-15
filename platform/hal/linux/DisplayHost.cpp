#include "DisplayHost.hpp"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <pthread.h>

namespace hal::linux::display
{
    namespace
    {
        constexpr unsigned WIDTH = 1024, HEIGHT = 600;
        pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
        struct Lock
        {
            Lock() { pthread_mutex_lock(&mutex); }
            ~Lock() { pthread_mutex_unlock(&mutex); }
        };
        std::array<std::array<std::uint16_t, WIDTH * HEIGHT>, 2> buffers{};
        hal::display::State state;
        std::array<Pointer, 32> input{};
        unsigned head{}, count{}, drops{}, duty{};
        Pointer last{};
        bool enabled{}, leased{};
        std::atomic_bool finished{}, cleaned{};
        std::atomic_int exitStatus{};
        std::uint64_t nowNs()
        {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            return static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL + now.tv_nsec;
        }
        void inputEvent(bool down, int x, int y, int w, int h, bool cancelled = false)
        {
            const auto scale = std::min(double(w) / WIDTH, double(h) / HEIGHT);
            const auto left = (w - WIDTH * scale) * .5, top = (h - HEIGHT * scale) * .5;
            const auto px = scale > 0 ? (x - left) / scale : -1;
            const auto py = scale > 0 ? (y - top) / scale : -1;
            Pointer point{ down && px >= 0 && py >= 0 && px < WIDTH && py < HEIGHT,
                           static_cast<std::uint16_t>(std::clamp(px, 0., double(WIDTH - 1))),
                           static_cast<std::uint16_t>(std::clamp(py, 0., double(HEIGHT - 1))),
                           nowNs(),
                           cancelled || px < 0 || py < 0 || px >= WIDTH || py >= HEIGHT };
            Lock lock;
            // Keep press/release edges; coalesce consecutive drag coordinates.
            if (count && input[(head + count - 1) % input.size()].down == point.down) {
                input[(head + count - 1) % input.size()] = point;
            }
            else if (count < input.size()) {
                input[(head + count++) % input.size()] = point;
            }
            else {
                ++drops;
                head = 0;
                count = 1;
                input[0] = { .cancelled = true }; // fail released
            }
        }
        void* kernelEntry(void* argument)
        {
            (*static_cast<void (**)()>(argument))();
            std::abort();
        }
    }
    bool cleanupComplete() noexcept { return cleaned.load(std::memory_order_acquire); }
    bool available() noexcept
    {
        Lock lock;
        return enabled;
    }
    bool acquire() noexcept
    {
        Lock lock;
        if (!enabled || leased) {
            return false;
        }
        leased = true;
        state = {};
        head = count = drops = duty = 0;
        last = {};
        for (auto& buffer : buffers) {
            std::ranges::fill(buffer, 0);
        }
        return true;
    }
    void release() noexcept
    {
        Lock lock;
        leased = false;
        state.pending.reset();
        duty = 0;
    }
    std::span<std::uint16_t> framebuffer(unsigned slot) noexcept
    {
        return slot < 2 ? std::span<std::uint16_t>{ buffers[slot] } : std::span<std::uint16_t>{};
    }
    bool submit(unsigned slot) noexcept
    {
        Lock lock;
        if (!leased || state.closed || state.pending || slot > 1 || slot == state.active) {
            return false;
        }
        state.pending = slot;
        return true;
    }
    hal::display::State snapshot() noexcept
    {
        Lock lock;
        return state;
    }
    Pointer pointer() noexcept
    {
        Lock lock;
        if (count) {
            last = input[head];
            head = (head + 1) % input.size();
            --count;
        }
        return last;
    }
    std::uint32_t inputDrops() noexcept
    {
        Lock lock;
        return drops;
    }
    void brightness(unsigned percent) noexcept
    {
        Lock lock;
        duty = percent;
    }
    void completed(int status) noexcept
    {
        exitStatus.store(status);
        finished.store(true, std::memory_order_release);
    }

    int run(void (*kernel)(), Observer observer, void* context)
    {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            std::fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
            return EXIT_FAILURE;
        }
        SDL_Window* window = SDL_CreateWindow("ProtoMate | Interface studio",
                                              SDL_WINDOWPOS_CENTERED,
                                              SDL_WINDOWPOS_CENTERED,
                                              WIDTH,
                                              HEIGHT,
                                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : nullptr;
        if (!renderer && window) {
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        }
        SDL_Texture* texture =
          renderer
            ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT)
            : nullptr;
        if (!texture) {
            std::fprintf(stderr, "SDL display failed: %s\n", SDL_GetError());
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return EXIT_FAILURE;
        }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        {
            Lock lock;
            enabled = true;
        }
        finished.store(false);
        cleaned.store(false);
        pthread_t thread{};
        if (pthread_create(&thread, nullptr, kernelEntry, &kernel) != 0) {
            SDL_DestroyTexture(texture);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return EXIT_FAILURE;
        }
        pthread_detach(thread); // ThreadX scheduler ends with the process, after application cleanup.
        bool down{};
        auto nextFrame = nowNs();
        while (!finished.load(std::memory_order_acquire)) {
            SDL_Event event;
            int w{}, h{};
            SDL_GetWindowSize(window, &w, &h);
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) {
                    Lock lock;
                    state.closed = true;
                }
                if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                    down = true;
                    inputEvent(true, event.button.x, event.button.y, w, h);
                }
                if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
                    down = false;
                    inputEvent(false, event.button.x, event.button.y, w, h);
                }
                if (event.type == SDL_MOUSEMOTION && down) {
                    inputEvent(true, event.motion.x, event.motion.y, w, h);
                }
                if (event.type == SDL_WINDOWEVENT && (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                                                      event.window.event == SDL_WINDOWEVENT_LEAVE)) {
                    down = false;
                    inputEvent(false, 0, 0, w, h, true);
                }
            }
            const auto now = nowNs();
            if (now >= nextFrame) {
                nextFrame = now + 16666667;
                bool updated{};
                {
                    Lock lock;
                    if (leased) {
                        ++state.vsyncs;
                        if (state.pending) {
                            const auto slot = *state.pending;
                            // SDL finishes copying before the old framebuffer is
                            // released. Neither GUI rendering nor shutdown can race it.
                            if (SDL_UpdateTexture(texture, nullptr, buffers[slot].data(), WIDTH * 2) == 0) {
                                state.active = slot;
                                ++state.presentations;
                                updated = true;
                            }
                            else {
                                ++state.errors;
                            }
                            state.pending.reset();
                        }
                    }
                }
                if (observer) {
                    observer(context, snapshot(), SDL_GetWindowID(window));
                }
                if (updated || SDL_GetWindowFlags(window) & SDL_WINDOW_SHOWN) {
                    int rw{}, rh{};
                    SDL_GetRendererOutputSize(renderer, &rw, &rh);
                    const auto scale = std::min(double(rw) / WIDTH, double(rh) / HEIGHT);
                    SDL_Rect destination{ (rw - int(WIDTH * scale)) / 2,
                                          (rh - int(HEIGHT * scale)) / 2,
                                          int(WIDTH * scale),
                                          int(HEIGHT * scale) };
                    SDL_SetRenderDrawColor(renderer, 8, 10, 14, 255);
                    SDL_RenderClear(renderer);
                    SDL_RenderCopy(renderer, texture, nullptr, &destination);
                    SDL_RenderPresent(renderer);
                }
            }
            SDL_Delay(1);
        }
        {
            Lock lock;
            enabled = false;
        }
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        cleaned.store(true, std::memory_order_release);
        return exitStatus.load();
    }
}
