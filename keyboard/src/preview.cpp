#include "framekeyboard/app.hpp"
#include "framekeyboard/updates.hpp"
#include <SDL.h>
#include <memory>
#include <stdexcept>

namespace framekeyboard {
namespace {
void require(bool ok) {
    if (!ok) {
        throw std::runtime_error(SDL_GetError());
    }
}
struct QuitSDL {
    ~QuitSDL() { SDL_Quit(); }
};
} // namespace
int run_preview(App& app, double duration) {
    require(SDL_Init(SDL_INIT_VIDEO) == 0);
    QuitSDL cleanup;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("Full Keyboard preview", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280,
                         800, SDL_WINDOW_RESIZABLE),
        SDL_DestroyWindow);
    require(window != nullptr);
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
        SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_SOFTWARE), SDL_DestroyRenderer);
    require(renderer != nullptr);
    require(SDL_RenderSetLogicalSize(renderer.get(), panel_width, texture_height) == 0);
    std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture(
        SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                          panel_width, texture_height),
        SDL_DestroyTexture);
    require(texture != nullptr);
    const double start = monotonic_seconds();
    bool done = false;
    while (!done && !app.quitting() && !interrupted) {
        const double now = monotonic_seconds();
        if (duration > 0 && now - start >= duration) {
            break;
        }
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, app.renderer.animating() ? 16 : 25)) {
            do {
                if (event.type == SDL_QUIT) {
                    done = true;
                }
                if (event.type == SDL_MOUSEMOTION) {
                    app.move(0, event.motion.x, event.motion.y - popup_margin);
                }
                if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                    app.down(0, event.button.x, event.button.y - popup_margin, now);
                }
                if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
                    app.up(0, event.button.x, event.button.y - popup_margin);
                }
                if (event.type == SDL_MOUSEWHEEL) {
                    const double direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
#if SDL_VERSION_ATLEAST(2, 0, 18)
                    double dx = -event.wheel.preciseX * direction;
                    double dy = -event.wheel.preciseY * direction;
#else
                    double dx = -event.wheel.x * direction, dy = -event.wheel.y * direction;
#endif
                    if (SDL_GetModState() & KMOD_SHIFT) {
                        dx = dy;
                        dy = 0;
                    }
                    app.scroll(0, dx, dy);
                }
                if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                    app.back();
                }
                if (event.type == SDL_WINDOWEVENT) {
                    if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                        event.window.event == SDL_WINDOWEVENT_LEAVE) {
                        app.cancel();
                    }
                    if (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                        event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        app.dirty = true;
                    }
                }
            } while (SDL_PollEvent(&event));
        }
        if (app.tick(now)) {
            app.paint(now);
            auto* surface = app.renderer.surface();
            require(SDL_UpdateTexture(texture.get(), nullptr, cairo_image_surface_get_data(surface),
                                      cairo_image_surface_get_stride(surface)) == 0);
            SDL_SetRenderDrawColor(renderer.get(), 25, 26, 28, 255);
            SDL_RenderClear(renderer.get());
            SDL_RenderCopy(renderer.get(), texture.get(), nullptr, nullptr);
            SDL_RenderPresent(renderer.get());
        }
        if (!done && !app.quitting() && !interrupted) {
            update_startup_ready(monotonic_seconds());
        }
    }
    app.cancel();
    return 0;
}
} // namespace framekeyboard
