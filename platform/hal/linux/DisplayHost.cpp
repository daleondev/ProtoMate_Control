#include "DisplayHost.hpp"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <pthread.h>
#include <signal.h>

namespace hal::linux::display
{
    namespace
    {
        constexpr unsigned WIDTH{ 1024 }, HEIGHT{ 600 };
        // This platform macro supplies the complete braced initializer.
        pthread_mutex_t mutex PTHREAD_MUTEX_INITIALIZER;
        struct Lock
        {
            sigset_t previous{};
            Lock()
            {
                // ThreadX/Linux preempts workers with SIGUSR1. Suspending a
                // worker while it owns this native mutex lets a higher-priority
                // worker block in pthread_mutex_lock without yielding ThreadX:
                // the owner can never resume. No ThreadX calls belong inside
                // this short native critical section (including SDL's side).
                sigset_t suspended{};
                sigemptyset(&suspended);
                sigaddset(&suspended, SIGUSR1);
                pthread_sigmask(SIG_BLOCK, &suspended, &previous);
                pthread_mutex_lock(&mutex);
            }
            ~Lock()
            {
                pthread_mutex_unlock(&mutex);
                pthread_sigmask(SIG_SETMASK, &previous, nullptr);
            }
        };
        std::array<std::array<std::uint16_t, WIDTH * HEIGHT>, 2> buffers{};
        hal::display::State state{};
        std::array<Pointer, 32> input{};
        unsigned head{}, count{}, drops{}, duty{};
        Pointer last{};
        std::array<char, 32> keys{};
        unsigned keyCount{};
        std::uint32_t keySession{};
        bool keyCancelled{};
        bool enabled{}, leased{};
        Clock peripheralClock{};
        std::uint64_t nextVsync{};
        std::atomic_bool finished{}, cleaned{};
        std::atomic_int exitStatus{};
        std::uint64_t nowNs()
        {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            return static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL + now.tv_nsec;
        }
        void inputEvent(bool down, int x, int y, int w, int h, bool cancelled = false, float wheel = 0)
        {
            const auto scale{ std::min(double(w) / WIDTH, double(h) / HEIGHT) };
            const auto left{ (w - WIDTH * scale) * .5 }, top{ (h - HEIGHT * scale) * .5 };
            const auto px{ scale > 0 ? (x - left) / scale : -1 };
            const auto py{ scale > 0 ? (y - top) / scale : -1 };
            Lock lock{};
            Pointer point{ down && px >= 0 && py >= 0 && px < WIDTH && py < HEIGHT,
                           static_cast<std::uint16_t>(std::clamp(px, 0., double(WIDTH - 1))),
                           static_cast<std::uint16_t>(std::clamp(py, 0., double(HEIGHT - 1))),
                           peripheralClock ? peripheralClock() : nowNs(),
                           cancelled || px < 0 || py < 0 || px >= WIDTH || py >= HEIGHT,
                           wheel };
            // Keep press/release edges; coalesce consecutive drag coordinates.
            if (count && input[(head + count - 1) % input.size()].down == point.down &&
                input[(head + count - 1) % input.size()].cancelled == point.cancelled &&
                ((!point.wheel && !input[(head + count - 1) % input.size()].wheel) ||
                 (point.x == input[(head + count - 1) % input.size()].x &&
                  point.y == input[(head + count - 1) % input.size()].y))) {
                point.wheel += input[(head + count - 1) % input.size()].wheel;
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
        void keyEvent(char key)
        {
            Lock lock{};
            if (!keySession || keyCancelled)
                return;
            if (key == '\x1b' || keyCount == keys.size()) {
                if (keyCount == keys.size())
                    ++drops;
                keys[0] = '\x1b';
                keyCount = 1;
                keyCancelled = true;
            }
            else
                keys[keyCount++] = key;
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
        Lock lock{};
        return enabled;
    }
    bool acquire(Clock clock) noexcept
    {
        Lock lock{};
        if (!enabled || leased) {
            return false;
        }
        leased = true;
        peripheralClock = clock;
        nextVsync = clock ? clock() : nowNs();
        state = {};
        head = count = drops = duty = 0;
        keyCount = keySession = 0;
        keyCancelled = false;
        last = {};
        for (auto& buffer : buffers) {
            std::ranges::fill(buffer, 0);
        }
        return true;
    }
    void release() noexcept
    {
        Lock lock{};
        leased = false;
        keyCount = keySession = 0;
        peripheralClock = nullptr;
        state.pending.reset();
        duty = 0;
    }
    std::span<std::uint16_t> framebuffer(unsigned slot) noexcept
    {
        return slot < 2 ? std::span<std::uint16_t>{ buffers[slot] } : std::span<std::uint16_t>{};
    }
    bool submit(unsigned slot) noexcept
    {
        Lock lock{};
        if (!leased || state.closed || state.pending || slot > 1 || slot == state.active) {
            return false;
        }
        state.pending = slot;
        return true;
    }
    hal::display::State snapshot() noexcept
    {
        Lock lock{};
        return state;
    }
    Pointer pointer() noexcept
    {
        Lock lock{};
        if (count) {
            last = input[head];
            head = (head + 1) % input.size();
            --count;
        }
        const auto result = last;
        last.wheel = 0;
        return result;
    }
    std::uint32_t inputDrops() noexcept
    {
        Lock lock{};
        return drops;
    }
    unsigned keyboard(std::uint32_t session, std::span<char> output) noexcept
    {
        Lock lock{};
        if (keySession != session) {
            keySession = session;
            keyCount = 0;
            keyCancelled = false;
        }
        const auto n = std::min(keyCount, unsigned(output.size()));
        std::copy_n(keys.begin(), n, output.begin());
        std::move(keys.begin() + n, keys.begin() + keyCount, keys.begin());
        keyCount -= n;
        return n;
    }
    void brightness(unsigned percent) noexcept
    {
        Lock lock{};
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
        SDL_Window* window{ SDL_CreateWindow("ProtoMate | Interface studio",
                                             SDL_WINDOWPOS_CENTERED,
                                             SDL_WINDOWPOS_CENTERED,
                                             WIDTH,
                                             HEIGHT,
                                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI) };
        SDL_Renderer* renderer{ window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : nullptr };
        if (!renderer && window) {
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        }
        SDL_Texture* texture{
            renderer ? SDL_CreateTexture(
                         renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT)
                     : nullptr
        };
        if (!texture) {
            std::fprintf(stderr, "SDL display failed: %s\n", SDL_GetError());
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return EXIT_FAILURE;
        }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        {
            Lock lock{};
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
        bool down{}, focused{ true };
        auto nextFrame{ nowNs() };
        while (!finished.load(std::memory_order_acquire)) {
            SDL_Event event{};
            int w{}, h{};
            SDL_GetWindowSize(window, &w, &h);
            while (SDL_PollEvent(&event)) {
                if (focused && event.type == SDL_KEYDOWN &&
                    !(event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI))) {
                    const auto key = event.key.keysym.sym;
                    // Numeric editors need ASCII keys only; do not also consume
                    // TEXTINPUT, which would insert each physical key twice.
                    const bool shifted = event.key.keysym.mod & KMOD_SHIFT;
                    if (!shifted && key >= SDLK_0 && key <= SDLK_9)
                        keyEvent(char('0' + key - SDLK_0));
                    else if (key >= SDLK_KP_1 && key <= SDLK_KP_9)
                        keyEvent(char('1' + key - SDLK_KP_1));
                    else if (key == SDLK_KP_0)
                        keyEvent('0');
                    else if (key == SDLK_KP_PERIOD || (!shifted && key == SDLK_PERIOD))
                        keyEvent('.');
                    else if (key == SDLK_KP_COMMA || (!shifted && key == SDLK_COMMA))
                        keyEvent(',');
                    else if (key == SDLK_KP_MINUS || (!shifted && key == SDLK_MINUS))
                        keyEvent('-');
                    else if (key == SDLK_KP_PLUS || key == SDLK_PLUS || (shifted && key == SDLK_EQUALS))
                        keyEvent('+');
                    else if (key == SDLK_BACKSPACE)
                        keyEvent('\b');
                    else if (!event.key.repeat) {
                        if (key == SDLK_RETURN || key == SDLK_KP_ENTER)
                            keyEvent('\n');
                        else if (key == SDLK_ESCAPE)
                            keyEvent('\x1b');
                        else if (key == SDLK_DELETE)
                            keyEvent('\x7f');
                    }
                }
                if (event.type == SDL_QUIT) {
                    Lock lock{};
                    state.closed = true;
                }
                if (focused && event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                    down = true;
                    inputEvent(true, event.button.x, event.button.y, w, h);
                }
                if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
                    down = false;
                    inputEvent(false, event.button.x, event.button.y, w, h);
                }
                if (focused && event.type == SDL_MOUSEWHEEL) {
                    int x{}, y{};
#if SDL_VERSION_ATLEAST(2, 26, 0)
                    x = event.wheel.mouseX;
                    y = event.wheel.mouseY;
#else
                    SDL_GetMouseState(&x, &y);
#endif
                    inputEvent(down,
                               x,
                               y,
                               w,
                               h,
                               false,
                               event.wheel.preciseY *
                                 (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.f : 1.f));
                }
                if (focused && event.type == SDL_MOUSEMOTION && down) {
                    inputEvent(true, event.motion.x, event.motion.y, w, h);
                }
                if (event.type == SDL_WINDOWEVENT && (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                                                      event.window.event == SDL_WINDOWEVENT_LEAVE)) {
                    if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        focused = false;
                        keyEvent('\x1b');
                    }
                    down = false;
                    inputEvent(false, 0, 0, w, h, true);
                }
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
                    focused = true;
            }
            const auto now{ nowNs() };
            if (now >= nextFrame) {
                nextFrame = now + 16666667;
                bool updated{};
                {
                    Lock lock{};
                    const auto peripheralNow{ peripheralClock ? peripheralClock() : now };
                    if (leased && peripheralNow >= nextVsync) {
                        nextVsync = peripheralNow + 16666667;
                        ++state.vsyncs;
                        if (state.pending) {
                            const auto slot{ *state.pending };
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
                    const auto scale{ std::min(double(rw) / WIDTH, double(rh) / HEIGHT) };
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
            Lock lock{};
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
