#ifndef CHIP8_H
#define CHIP8_H

#include <stdbool.h>
#include <stdint.h>

#define CHIP8_MEMORY 4096
#define CHIP8_START_ADDR 0x200
#define CHIP8_FONT_ADDR 0x050
#define CHIP8_WIDTH 64
#define CHIP8_HEIGHT 32
#define CHIP8_NUM_REGS 16
#define CHIP8_STACK_SIZE 16
#define CHIP8_NUM_KEYS 16

typedef enum {
  CHIP8_STATE_READY = 0, // Initialized, font loaded, waiting for ROM
  CHIP8_STATE_RUNNING,   // ROM loaded, executing cycles
  CHIP8_STATE_FAULT,     // Halted due to a runtime fault
} Chip8State;

typedef enum {
  CHIP8_FAULT_NONE = 0,
  CHIP8_FAULT_PC_OUT_OF_BOUNDS,
  CHIP8_FAULT_MEMORY_OUT_OF_BOUNDS,
  CHIP8_FAULT_UNKNOWN_OPCODE,
} Chip8Fault;

typedef enum {
  CHIP8_DISPLAY_CLEAN = 0, // Framebuffer matches last rendered frame
  CHIP8_DISPLAY_DIRTY,     // Framebuffer modified; awaiting render
} Chip8DisplayState;

typedef struct {
  uint8_t memory[CHIP8_MEMORY];
  uint8_t V[CHIP8_NUM_REGS];
  uint16_t I;
  uint16_t pc;
  uint16_t stack[CHIP8_STACK_SIZE];
  uint8_t sp;
  uint8_t delay_timer;
  uint8_t sound_timer;
  uint8_t display[CHIP8_WIDTH * CHIP8_HEIGHT];
  uint8_t keypad[CHIP8_NUM_KEYS];

  Chip8State state;
  Chip8Fault fault;
  uint16_t fault_opcode;
  Chip8DisplayState display_state;
} Chip8;

void chip8_init(Chip8 *c8);
bool chip8_load_rom(Chip8 *c8, const char *path);
void chip8_cycle(Chip8 *c8);
bool chip8_consume_frame(Chip8 *c8);
const char *chip8_fault_str(Chip8Fault fault);

#endif
