#include "chip8.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint8_t FONTSET[80] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
    0x20, 0x60, 0x20, 0x20, 0x70, // 1
    0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
    0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
    0x90, 0x90, 0xF0, 0x10, 0x10, // 4
    0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
    0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
    0xF0, 0x10, 0x20, 0x40, 0x40, // 7
    0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
    0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
    0xF0, 0x90, 0xF0, 0x90, 0x90, // A
    0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
    0xF0, 0x80, 0x80, 0x80, 0xF0, // C
    0xE0, 0x90, 0x90, 0x90, 0xE0, // D
    0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
    0xF0, 0x80, 0xF0, 0x80, 0x80  // F
};
typedef struct {
  uint16_t raw;
  uint16_t group;
  uint16_t nnn;
  uint8_t nn;
  uint8_t n;
  uint8_t x;
  uint8_t y;
} Chip8Instruction;

static void chip8_enter_fault(Chip8 *c8, Chip8Fault fault, uint16_t opcode) {
  c8->state = CHIP8_STATE_FAULT;
  c8->fault = fault;
  c8->fault_opcode = opcode;
}

static bool chip8_fetch(Chip8 *c8, uint16_t *out_opcode) {
  if ((size_t)c8->pc + 1 >= CHIP8_MEMORY) {
    chip8_enter_fault(c8, CHIP8_FAULT_PC_OUT_OF_BOUNDS, 0);
    return false;
  }
  *out_opcode =
      ((uint16_t)c8->memory[c8->pc] << 8) | c8->memory[c8->pc + 1];
  c8->pc += 2;
  return true;
}

static Chip8Instruction chip8_decode(uint16_t opcode) {
  Chip8Instruction inst;
  inst.raw = opcode;
  inst.group = opcode & 0xF000;
  inst.nnn = opcode & 0x0FFF;
  inst.nn = opcode & 0x00FF;
  inst.n = opcode & 0x000F;
  inst.x = (opcode & 0x0F00) >> 8;
  inst.y = (opcode & 0x00F0) >> 4;
  return inst;
}

static void chip8_execute(Chip8 *c8, Chip8Instruction inst) {
  switch (inst.group) {
  case 0x0000:
    if (inst.raw == 0x00E0) {
      memset(c8->display, 0, sizeof(c8->display));
      c8->display_state = CHIP8_DISPLAY_DIRTY;
    } else {
      chip8_enter_fault(c8, CHIP8_FAULT_UNKNOWN_OPCODE, inst.raw);
    }
    break;
  case 0x1000:
    c8->pc = inst.nnn;
    break;
  case 0x6000:
    c8->V[inst.x] = inst.nn;
    break;
  case 0x7000:
    c8->V[inst.x] += inst.nn;
    break;
  case 0xA000:
    c8->I = inst.nnn;
    break;
  case 0xD000: {
    uint8_t x_pos = c8->V[inst.x] % CHIP8_WIDTH;
    uint8_t y_pos = c8->V[inst.y] % CHIP8_HEIGHT;
    c8->V[0xF] = 0;

    for (uint8_t row = 0; row < inst.n; row++) {
      if (y_pos + row >= CHIP8_HEIGHT) {
        break;
      }
      if ((size_t)c8->I + row >= CHIP8_MEMORY) {
        chip8_enter_fault(c8, CHIP8_FAULT_MEMORY_OUT_OF_BOUNDS, inst.raw);
        return;
      }
      uint8_t sprite_byte = c8->memory[c8->I + row];
      for (uint8_t col = 0; col < 8; col++) {
        if (x_pos + col >= CHIP8_WIDTH) {
          break;
        }
        if (sprite_byte & (0x80 >> col)) {
          size_t idx = (size_t)(y_pos + row) * CHIP8_WIDTH + (x_pos + col);
          if (c8->display[idx] == 1) {
            c8->V[0xF] = 1;
          }
          c8->display[idx] ^= 1;
        }
      }
    }
    c8->display_state = CHIP8_DISPLAY_DIRTY;
    break;
  }
  default:
    chip8_enter_fault(c8, CHIP8_FAULT_UNKNOWN_OPCODE, inst.raw);
    break;
  }
}

void chip8_init(Chip8 *c8) {
  memset(c8, 0, sizeof(*c8));
  c8->pc = CHIP8_START_ADDR;
  memcpy(&c8->memory[CHIP8_FONT_ADDR], FONTSET, sizeof(FONTSET));
  c8->state = CHIP8_STATE_READY;
  c8->fault = CHIP8_FAULT_NONE;
  c8->display_state = CHIP8_DISPLAY_CLEAN;
}

bool chip8_load_rom(Chip8 *c8, const char *path) {
  if (c8->state != CHIP8_STATE_READY) {
    chip8_init(c8);
  }

  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    perror("Failed to open ROM");
    return false;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  rewind(f);
  size_t max_rom_size = CHIP8_MEMORY - CHIP8_START_ADDR;
  if (size <= 0 || (size_t)size > max_rom_size) {
    fprintf(stderr, "Invalid ROM size: %ld bytes (max %zu)\n", size,
            max_rom_size);
    fclose(f);
    return false;
  }
  size_t bytes_read = fread(&c8->memory[CHIP8_START_ADDR], 1, (size_t)size, f);
  fclose(f);
  if (bytes_read != (size_t)size) {
    fprintf(stderr, "Failed to read ROM: read %zu of %ld bytes\n", bytes_read,
            size);
    return false;
  }

  c8->state = CHIP8_STATE_RUNNING;
  return true;
}

void chip8_cycle(Chip8 *c8) {
  switch (c8->state) {
  case CHIP8_STATE_READY:
  case CHIP8_STATE_FAULT:
    return;
  case CHIP8_STATE_RUNNING: {
    uint16_t opcode = 0;
    if (!chip8_fetch(c8, &opcode)) {
      return;
    }
    Chip8Instruction inst = chip8_decode(opcode);
    chip8_execute(c8, inst);
    break;
  }
  }
}

bool chip8_consume_frame(Chip8 *c8) {
  if (c8->display_state != CHIP8_DISPLAY_DIRTY) {
    return false;
  }
  c8->display_state = CHIP8_DISPLAY_CLEAN;
  return true;
}

const char *chip8_fault_str(Chip8Fault fault) {
  switch (fault) {
  case CHIP8_FAULT_NONE:
    return "no fault";
  case CHIP8_FAULT_PC_OUT_OF_BOUNDS:
    return "program counter out of bounds";
  case CHIP8_FAULT_MEMORY_OUT_OF_BOUNDS:
    return "memory access out of bounds";
  case CHIP8_FAULT_UNKNOWN_OPCODE:
    return "unknown opcode";
  }
  return "invalid fault";
}
