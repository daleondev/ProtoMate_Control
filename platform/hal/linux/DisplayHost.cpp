#include "DisplayHost.hpp"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <pthread.h>

namespace hal::linux::display
{
    namespace
    {
        constexpr unsigned WIDTH{ 1024 }, HEIGHT{ 600 }, INPUT_CAPACITY{ 32 };
        constexpr std::uint64_t FRAME_NS{ 16'666'667 }, SECOND_NS{ 1'000'000'000 };
        constexpr double HALF{ .5 };
        constexpr SDL_Color LETTERBOX{ 8, 10, 14, 255 };
        // This platform macro supplies the complete braced initializer.
        // Native SDL/ThreadX handoff state: Lock serializes ordinary fields; shutdown flags are atomic.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        pthread_mutex_t mutex PTHREAD_MUTEX_INITIALIZER;
        struct Lock
        {
            Lock(const Lock&) = delete;
            Lock& operator=(const Lock&) = delete;
            Lock(Lock&&) = delete;
            Lock& operator=(Lock&&) = delete;
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
        // Native SDL/ThreadX handoff state: Lock serializes ordinary fields; shutdown flags are atomic.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        std::array<std::array<std::uint16_t, static_cast<std::size_t>(WIDTH) * HEIGHT>, 2> buffers{};
        // Native SDL/ThreadX handoff state: Lock serializes ordinary fields; shutdown flags are atomic.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        hal::display::State state{};
        // Native SDL/ThreadX handoff state: Lock serializes ordinary fields; shutdown flags are atomic.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        std::array<Pointer, INPUT_CAPACITY> input{};
        // Native SDL/ThreadX handoff state: Lock serializes ordinary fields; shutdown flags are atomic.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        unsigned head{}, count{}, drops{}, duty{};
        // Native SDL/ThreadX handoff state: Lock serializes ordinary fields; shutdown flags are atomic.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        Pointer last{};
        // Native SDL/ThreadX handoff state: Lock serializes ordinary fields; shutdown flags are atomic.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        std::array<char, INPUT_CAPACITY> keys{};
        // Native SDL/ThreadX handoff state: Lock serializes ordinary fields; shutdown flags are atomic.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        unsigned key_count{};
        // Native SDL/ThreadX handoff state: Lock serializes ordinary fields; shutdown flags are atomic.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        std::uint32_t key_session{};
        // Native SDL/ThreadX handoff state: Lock serializes ordinary fields; shutdown flags are atomic.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        bool key_cancelled{};
        // Native SDL/ThreadX handoff state: Lock serializes ordinary fields; shutdown flags are atomic.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        bool enabled{}, leased{};
        // Native SDL/ThreadX handoff state: Lock serializes ordinary fields; shutdown flags are atomic.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        Clock peripheral_clock{};
        // Native SDL/ThreadX handoff state: Lock serializes ordinary fields; shutdown flags are atomic.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        std::uint64_t next_vsync{};
        // Native SDL/ThreadX handoff state: Lock serializes ordinary fields; shutdown flags are atomic.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        std::atomic_bool finished{}, cleaned{};
        // Native SDL/ThreadX handoff state: Lock serializes ordinary fields; shutdown flags are atomic.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        std::atomic_int exit_status{};
        std::uint64_t now_ns()
        {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            return (static_cast<std::uint64_t>(now.tv_sec) * SECOND_NS) + now.tv_nsec;
        }
        struct PointerLocation
        {
            int x{};
            int y{};
        };
        void input_event(bool down,
                         PointerLocation location,
                         SDL_Point window_size,
                         bool cancelled = false,
                         float wheel = 0)
        {
            const auto [x, y]{ location };
            const auto [w, h]{ window_size };
            const auto scale{ std::min(double(w) / WIDTH, double(h) / HEIGHT) };
            const auto left{ (w - WIDTH * scale) * HALF };
            const auto top{ (h - HEIGHT * scale) * HALF };
            const auto px{ scale > 0 ? (x - left) / scale : -1 };
            const auto py{ scale > 0 ? (y - top) / scale : -1 };
            Lock lock{};
            Pointer point{ .down = down && px >= 0 && py >= 0 && px < WIDTH && py < HEIGHT,
                           .x = static_cast<std::uint16_t>(std::clamp(px, 0., double(WIDTH - 1))),
                           .y = static_cast<std::uint16_t>(std::clamp(py, 0., double(HEIGHT - 1))),
                           .time_ns = peripheral_clock ? peripheral_clock() : now_ns(),
                           .cancelled = cancelled || px < 0 || py < 0 || px >= WIDTH || py >= HEIGHT,
                           .wheel = wheel };
            // Keep press/release edges; coalesce consecutive drag coordinates.
            if (count && input[(head + count - 1) % input.size()].down == point.down &&
                input[(head + count - 1) % input.size()].cancelled == point.cancelled &&
                ((point.wheel == 0 && input[(head + count - 1) % input.size()].wheel == 0) ||
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
        void key_event(char key)
        {
            Lock lock{};
            if (!key_session || key_cancelled) {
                return;
            }
            if (key == '\x1b' || key_count == keys.size()) {
                if (key_count == keys.size()) {
                    ++drops;
                }
                keys[0] = '\x1b';
                key_count = 1;
                key_cancelled = true;
            }
            else {
                keys[key_count++] = key;
            }
        }
        void* kernel_entry(void* argument)
        {
            (*static_cast<void (**)()>(argument))();
            std::abort();
        }
        bool update_texture(SDL_Texture* texture, std::uint64_t now)
        {
            bool updated{};
            {
                Lock lock{};
                const auto peripheral_now{ peripheral_clock ? peripheral_clock() : now };
                if (leased && peripheral_now >= next_vsync) {
                    next_vsync = peripheral_now + FRAME_NS;
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
            return updated;
        }
        void present(SDL_Renderer* renderer, SDL_Texture* texture)
        {
            int rw{};
            int rh{};
            SDL_GetRendererOutputSize(renderer, &rw, &rh);
            const auto scale{ std::min(double(rw) / WIDTH, double(rh) / HEIGHT) };
            SDL_Rect destination{ (rw - int(WIDTH * scale)) / 2,
                                  (rh - int(HEIGHT * scale)) / 2,
                                  int(WIDTH * scale),
                                  int(HEIGHT * scale) };
            SDL_SetRenderDrawColor(renderer, LETTERBOX.r, LETTERBOX.g, LETTERBOX.b, LETTERBOX.a);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, &destination);
            SDL_RenderPresent(renderer);
        }
        struct InputState
        {
            bool down{}, focused{ true };
        };
        void keyboard_event(const SDL_KeyboardEvent& event)
        {
            if (event.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) {
                return;
            }
            const auto key{ event.keysym.sym };
            const bool shifted{ (event.keysym.mod & KMOD_SHIFT) != 0 };
            // ASCII keydown only: TEXTINPUT would duplicate numeric input.
            if (!shifted && key >= SDLK_0 && key <= SDLK_9) {
                key_event(char('0' + key - SDLK_0));
            }
            else if (key >= SDLK_KP_1 && key <= SDLK_KP_9) {
                key_event(char('1' + key - SDLK_KP_1));
            }
            else if (key == SDLK_KP_0) {
                key_event('0');
            }
            else if (key == SDLK_KP_PERIOD || (!shifted && key == SDLK_PERIOD)) {
                key_event('.');
            }
            else if (key == SDLK_KP_COMMA || (!shifted && key == SDLK_COMMA)) {
                key_event(',');
            }
            else if (key == SDLK_KP_MINUS || (!shifted && key == SDLK_MINUS)) {
                key_event('-');
            }
            else if (key == SDLK_KP_PLUS || key == SDLK_PLUS || (shifted && key == SDLK_EQUALS)) {
                key_event('+');
            }
            else if (key == SDLK_BACKSPACE) {
                key_event('\b');
            }
            else if (!event.repeat) {
                if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                    key_event('\n');
                }
                else if (key == SDLK_ESCAPE) {
                    key_event('\x1b');
                }
                else if (key == SDLK_DELETE) {
                    key_event('\x7f');
                }
            }
        }
        void wheel_event(const SDL_MouseWheelEvent& event, const InputState& pointer, SDL_Point size)
        {
            int x{};
            int y{};
#if SDL_VERSION_ATLEAST(2, 26, 0)
            x = event.mouseX;
            y = event.mouseY;
#else
            SDL_GetMouseState(&x, &y);
#endif
            input_event(pointer.down,
                        { .x = x, .y = y },
                        size,
                        false,
                        event.preciseY * (event.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.F : 1.F));
        }
        void window_event(const SDL_WindowEvent& event, InputState& pointer, SDL_Point size)
        {
            if (event.event == SDL_WINDOWEVENT_FOCUS_LOST || event.event == SDL_WINDOWEVENT_LEAVE) {
                if (event.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    pointer.focused = false;
                    key_event('\x1b');
                }
                pointer.down = false;
                input_event(false, {}, size, true);
            }
            if (event.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                pointer.focused = true;
            }
        }
        // SDL_Event is a tagged union. Every payload below is selected by its SDL event tag.
        // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
        void dispatch_event(const SDL_Event& event, InputState& pointer, SDL_Point size)
        {
            switch (event.type) {
                case SDL_KEYDOWN:
                    if (pointer.focused) {
                        keyboard_event(event.key);
                    }
                    break;
                case SDL_QUIT: {
                    Lock lock{};
                    state.closed = true;
                    break;
                }
                case SDL_MOUSEBUTTONDOWN:
                    if (pointer.focused && event.button.button == SDL_BUTTON_LEFT) {
                        pointer.down = true;
                        input_event(true, { .x = event.button.x, .y = event.button.y }, size);
                    }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        pointer.down = false;
                        input_event(false, { .x = event.button.x, .y = event.button.y }, size);
                    }
                    break;
                case SDL_MOUSEWHEEL:
                    if (pointer.focused) {
                        wheel_event(event.wheel, pointer, size);
                    }
                    break;
                case SDL_MOUSEMOTION:
                    if (pointer.focused && pointer.down) {
                        input_event(true, { .x = event.motion.x, .y = event.motion.y }, size);
                    }
                    break;
                case SDL_WINDOWEVENT:
                    window_event(event.window, pointer, size);
                    break;
                default:
                    break;
            }
        }
        // NOLINTEND(cppcoreguidelines-pro-type-union-access)
    }
    bool cleanup_complete() noexcept { return cleaned.load(std::memory_order_acquire); }
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
        peripheral_clock = clock;
        next_vsync = clock ? clock() : now_ns();
        state = {};
        head = count = drops = duty = 0;
        key_count = key_session = 0;
        key_cancelled = false;
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
        key_count = key_session = 0;
        peripheral_clock = nullptr;
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
        const auto result{ last };
        last.wheel = 0;
        return result;
    }
    std::uint32_t input_drops() noexcept
    {
        Lock lock{};
        return drops;
    }
    unsigned keyboard(std::uint32_t session, std::span<char> output) noexcept
    {
        Lock lock{};
        if (key_session != session) {
            key_session = session;
            key_count = 0;
            key_cancelled = false;
        }
        const auto n{ std::min(key_count, unsigned(output.size())) };
        std::copy_n(keys.begin(), n, output.begin());
        std::move(keys.begin() + n, keys.begin() + key_count, keys.begin());
        key_count -= n;
        return n;
    }
    void brightness(unsigned percent) noexcept
    {
        Lock lock{};
        duty = percent;
    }
    void completed(int status) noexcept
    {
        exit_status.store(status);
        finished.store(true, std::memory_order_release);
    }

    int run(void (*kernel)(), Observer observer, void* context)
    {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            // Keep failure diagnostics nonthrowing, including allocation-failure paths.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
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
            // Keep failure diagnostics nonthrowing, including allocation-failure paths.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
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
        if (pthread_create(&thread, nullptr, kernel_entry, static_cast<void*>(&kernel)) != 0) {
            SDL_DestroyTexture(texture);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return EXIT_FAILURE;
        }
        pthread_detach(thread); // ThreadX scheduler ends with the process, after application cleanup.
        InputState pointer_state{};
        auto next_frame{ now_ns() };
        while (!finished.load(std::memory_order_acquire)) {
            SDL_Event event{};
            int w{};
            int h{};
            SDL_GetWindowSize(window, &w, &h);
            while (SDL_PollEvent(&event)) {
                dispatch_event(event, pointer_state, { w, h });
            }
            const auto now{ now_ns() };
            if (now >= next_frame) {
                next_frame = now + FRAME_NS;
                const auto updated{ update_texture(texture, now) };
                if (observer) {
                    observer(context, snapshot(), SDL_GetWindowID(window));
                }
                if (updated || SDL_GetWindowFlags(window) & SDL_WINDOW_SHOWN) {
                    present(renderer, texture);
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
        return exit_status.load();
    }
}
