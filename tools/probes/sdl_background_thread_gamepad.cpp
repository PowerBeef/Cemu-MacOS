#include <SDL3/SDL.h>
#include <cstdio>
#include <thread>
#include <chrono>
int main() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    std::thread t([]{
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC)) {
            printf("SDL_InitSubSystem failed on background thread: %s\n", SDL_GetError()); return;
        }
        printf("SDL gamepad subsystem initialised on a NON-MAIN thread: ok\n");
        for (int i = 0; i < 6; i++) {
            SDL_Event e; while (SDL_PollEvent(&e)) {}
            int n = 0; SDL_JoystickID* ids = SDL_GetGamepads(&n);
            printf("  poll %d: %d gamepad(s)", i, n);
            for (int k = 0; k < n; k++) printf("  [%s]", SDL_GetGamepadNameForID(ids[k]));
            printf("\n"); fflush(stdout);
            SDL_free(ids);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC);
    });
    t.join();
    return 0;
}
