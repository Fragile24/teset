#include "chip8.h"
#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SCALE 12

typedef enum {
  APP_STATE_RUNNING = 0, // Stepping CPU and presenting frames
  APP_STATE_HALTED,      // CPU faulted; keep window open until user quits
  APP_STATE_QUIT,        // Exiting application
} AppState;

typedef struct {
  bool sdl_ready;
  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Texture *texture;
} Platform;

static void platform_shutdown(Platform *plat) {
  if (plat->texture != NULL) {
    SDL_DestroyTexture(plat->texture);
    plat->texture = NULL;
  }
  if (plat->renderer != NULL) {
    SDL_DestroyRenderer(plat->renderer);
    plat->renderer = NULL;
  }
  if (plat->window != NULL) {
    SDL_DestroyWindow(plat->window);
    plat->window = NULL;
  }
  if (plat->sdl_ready) {
    SDL_Quit();
    plat->sdl_ready = false;
  }
}

static bool platform_init(Platform *plat) {
  memset(plat, 0, sizeof(*plat));

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }
  plat->sdl_ready = true;

  plat->window = SDL_CreateWindow(
      "CHIP-8", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      CHIP8_WIDTH * SCALE, CHIP8_HEIGHT * SCALE, SDL_WINDOW_SHOWN);
  if (plat->window == NULL) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    platform_shutdown(plat);
    return false;
  }

  plat->renderer =
      SDL_CreateRenderer(plat->window, -1, SDL_RENDERER_ACCELERATED);
  if (plat->renderer == NULL) {
    fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    platform_shutdown(plat);
    return false;
  }

  plat->texture = SDL_CreateTexture(
      plat->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      CHIP8_WIDTH, CHIP8_HEIGHT);
  if (plat->texture == NULL) {
    fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
    platform_shutdown(plat);
    return false;
  }

  return true;
}

static void platform_render(Platform *plat, const uint8_t *display) {
  uint32_t pixels[CHIP8_WIDTH * CHIP8_HEIGHT];
  for (size_t i = 0; i < CHIP8_WIDTH * CHIP8_HEIGHT; i++) {
    pixels[i] = display[i] ? 0xFFFFFFFF : 0xFF000000;
  }
  SDL_UpdateTexture(plat->texture, NULL, pixels,
                    CHIP8_WIDTH * (int)sizeof(uint32_t));
  SDL_RenderClear(plat->renderer);
  SDL_RenderCopy(plat->renderer, plat->texture, NULL, NULL);
  SDL_RenderPresent(plat->renderer);
}

static void handle_events(AppState *app_state) {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT) {
      *app_state = APP_STATE_QUIT;
    } else if (event.type == SDL_KEYDOWN &&
               event.key.keysym.sym == SDLK_ESCAPE) {
      *app_state = APP_STATE_QUIT;
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <rom_path>\n", argv[0]);
    return 1;
  }

  Chip8 c8;
  chip8_init(&c8);
  if (!chip8_load_rom(&c8, argv[1])) {
    return 1;
  }

  Platform plat;
  if (!platform_init(&plat)) {
    return 1;
  }

  AppState app_state = APP_STATE_RUNNING;

  while (app_state != APP_STATE_QUIT) {
    handle_events(&app_state);

    switch (app_state) {
    case APP_STATE_RUNNING:
      chip8_cycle(&c8);
      if (chip8_consume_frame(&c8)) {
        platform_render(&plat, c8.display);
      }
      if (c8.state == CHIP8_STATE_FAULT) {
        fprintf(stderr, "CHIP-8 fault: %s (opcode 0x%04X, pc 0x%03X)\n",
                chip8_fault_str(c8.fault), c8.fault_opcode, c8.pc);
        app_state = APP_STATE_HALTED;
      }
      SDL_Delay(2);
      break;
    case APP_STATE_HALTED:
      SDL_Delay(16);
      break;
    case APP_STATE_QUIT:
      break;
    }
  }

  platform_shutdown(&plat);
  return 0;
}
