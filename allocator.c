#define _DEFAULT_SOURCE
#include "allocator.h"
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define ALIGNMENT 16
#define PAGE_SIZE 4096
#define DEFAULT_CHUNK_SIZE (64 * 1024)

typedef enum {
  BLOCK_FREE,
  BLOCK_ALLOCATED,
} BlockState;

typedef enum {
  MALLOC_VALIDATE,
  MALLOC_SEARCH,
  MALLOC_REQUEST_OS,
  MALLOC_COMMIT,
  MALLOC_FAIL,
} MallocState;

typedef enum {
  COALESCE_NONE,
  COALESCE_NEXT,
  COALESCE_PREV,
  COALESCE_BOTH,
} CoalesceCase;

typedef enum {
  REALLOC_ALLOC_NEW,
  REALLOC_FREE_BLOCK,
  REALLOC_INVALID,
  REALLOC_KEEP_OR_SHRINK,
  REALLOC_GROW_IN_PLACE,
  REALLOC_RELOCATE,
} ReallocAction;

typedef struct block_header {
  size_t size;
  BlockState state;
  struct block_header *next;
  struct block_header *prev;
} block_header_t;

_Static_assert(sizeof(block_header_t) % ALIGNMENT == 0,
               "Header must be 16-byte aligned");

static block_header_t *header = NULL;
static block_header_t *tail = NULL;

static size_t align_up(size_t n, size_t align) {
  return (n + (align - 1)) & ~(align - 1);
}

static void *header_to_payload(block_header_t *hdr) {
  return (void *)(hdr + 1);
}

static block_header_t *payload_to_header(void *ptr) {
  return (block_header_t *)ptr - 1;
}

static int are_adjacent(const block_header_t *left,
                        const block_header_t *right) {
  return (const char *)(left + 1) + left->size == (const char *)right;
}

static int can_merge_next(const block_header_t *block) {
  return block->next != NULL && block->next->state == BLOCK_FREE &&
         are_adjacent(block, block->next);
}

static int can_merge_prev(const block_header_t *block) {
  return block->prev != NULL && block->prev->state == BLOCK_FREE &&
         are_adjacent(block->prev, block);
}

static block_header_t *request_space(size_t size) {
  if (size > SIZE_MAX - sizeof(block_header_t) - PAGE_SIZE) {
    return NULL;
  }

  size_t total = sizeof(block_header_t) + size;
  size_t map_size = (total < DEFAULT_CHUNK_SIZE)
                        ? DEFAULT_CHUNK_SIZE
                        : align_up(total, PAGE_SIZE);

  void *mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    return NULL;
  }

  block_header_t *block = (block_header_t *)mem;
  block->size = map_size - sizeof(block_header_t);
  block->state = BLOCK_FREE;
  block->next = NULL;
  block->prev = NULL;

  if (header == NULL) {
    header = block;
    tail = block;
  } else {
    tail->next = block;
    block->prev = tail;
    tail = block;
  }
  return block;
}

static block_header_t *find_free_block(size_t size) {
  block_header_t *curr = header;
  while (curr != NULL) {
    if (curr->state == BLOCK_FREE && curr->size >= size) {
      return curr;
    }
    curr = curr->next;
  }
  return NULL;
}

static void split_block(block_header_t *block, size_t size) {
  if (block->size - size < sizeof(block_header_t) + ALIGNMENT) {
    return;
  }

  block_header_t *new_block =
      (block_header_t *)((char *)header_to_payload(block) + size);

  new_block->size = block->size - size - sizeof(block_header_t);
  new_block->state = BLOCK_FREE;
  block->size = size;

  new_block->next = block->next;
  new_block->prev = block;
  if (block->next != NULL) {
    block->next->prev = new_block;
  } else {
    tail = new_block;
  }
  block->next = new_block;
}

static void merge_with_next(block_header_t *block) {
  block_header_t *next = block->next;
  block->size += sizeof(block_header_t) + next->size;
  block->next = next->next;
  if (next->next != NULL) {
    next->next->prev = block;
  } else {
    tail = block;
  }
}

static CoalesceCase classify_coalesce(const block_header_t *block) {
  int merge_next = can_merge_next(block);
  int merge_prev = can_merge_prev(block);

  if (merge_next && merge_prev) {
    return COALESCE_BOTH;
  }
  if (merge_next) {
    return COALESCE_NEXT;
  }
  if (merge_prev) {
    return COALESCE_PREV;
  }
  return COALESCE_NONE;
}

static block_header_t *coalesce(block_header_t *block) {
  switch (classify_coalesce(block)) {
  case COALESCE_NONE:
    break;

  case COALESCE_NEXT:
    merge_with_next(block);
    break;

  case COALESCE_PREV:
    block = block->prev;
    merge_with_next(block);
    break;

  case COALESCE_BOTH:
    merge_with_next(block);
    block = block->prev;
    merge_with_next(block);
    break;
  }
  return block;
}

void *my_malloc(size_t size) {
  MallocState state = MALLOC_VALIDATE;
  size_t aligned_size = 0;
  block_header_t *block = NULL;

  for (;;) {
    switch (state) {
    case MALLOC_VALIDATE:
      if (size == 0) {
        state = MALLOC_FAIL;
        break;
      }
      aligned_size = align_up(size, ALIGNMENT);
      if (aligned_size < size) {
        state = MALLOC_FAIL;
        break;
      }
      state = MALLOC_SEARCH;
      break;

    case MALLOC_SEARCH:
      block = find_free_block(aligned_size);
      state = (block != NULL) ? MALLOC_COMMIT : MALLOC_REQUEST_OS;
      break;

    case MALLOC_REQUEST_OS:
      block = request_space(aligned_size);
      state = (block != NULL) ? MALLOC_COMMIT : MALLOC_FAIL;
      break;

    case MALLOC_COMMIT:
      split_block(block, aligned_size);
      block->state = BLOCK_ALLOCATED;
      return header_to_payload(block);

    case MALLOC_FAIL:
      return NULL;
    }
  }
}

void my_free(void *ptr) {
  if (ptr == NULL) {
    return;
  }

  block_header_t *block = payload_to_header(ptr);
  if (block->state == BLOCK_FREE) {
    return;
  }

  block->state = BLOCK_FREE;
  coalesce(block);
}

void *my_calloc(size_t nmemb, size_t size) {
  if (nmemb == 0 || size == 0) {
    return NULL;
  }
  if (nmemb > SIZE_MAX / size) {
    return NULL;
  }

  size_t total = nmemb * size;
  void *ptr = my_malloc(total);
  if (ptr == NULL) {
    return NULL;
  }
  memset(ptr, 0, total);
  return ptr;
}

static ReallocAction classify_realloc(void *ptr, size_t size,
                                      size_t aligned_size) {
  if (ptr == NULL) {
    return REALLOC_ALLOC_NEW;
  }
  if (size == 0) {
    return REALLOC_FREE_BLOCK;
  }
  if (aligned_size < size) {
    return REALLOC_INVALID;
  }

  block_header_t *block = payload_to_header(ptr);
  if (block->size >= aligned_size) {
    return REALLOC_KEEP_OR_SHRINK;
  }
  if (can_merge_next(block) &&
      block->size + sizeof(block_header_t) + block->next->size >=
          aligned_size) {
    return REALLOC_GROW_IN_PLACE;
  }
  return REALLOC_RELOCATE;
}

void *my_realloc(void *ptr, size_t size) {
  size_t aligned_size = align_up(size, ALIGNMENT);

  switch (classify_realloc(ptr, size, aligned_size)) {
  case REALLOC_ALLOC_NEW:
    return my_malloc(size);

  case REALLOC_FREE_BLOCK:
    my_free(ptr);
    return NULL;

  case REALLOC_INVALID:
    return NULL;

  case REALLOC_KEEP_OR_SHRINK: {
    block_header_t *block = payload_to_header(ptr);
    split_block(block, aligned_size);
    if (block->next != NULL && block->next->state == BLOCK_FREE) {
      coalesce(block->next);
    }
    return ptr;
  }

  case REALLOC_GROW_IN_PLACE: {
    block_header_t *block = payload_to_header(ptr);
    merge_with_next(block);
    split_block(block, aligned_size);
    return ptr;
  }

  case REALLOC_RELOCATE: {
    block_header_t *block = payload_to_header(ptr);
    void *new_ptr = my_malloc(size);
    if (new_ptr == NULL) {
      return NULL;
    }
    memcpy(new_ptr, ptr, block->size);
    my_free(ptr);
    return new_ptr;
  }
  }
  return NULL;
}
