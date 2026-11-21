#include "context.h"
#include "kernel/string.h"
#include "paging.h"
#include <stddef.h>

typedef struct {
    uint64_t f[32], fcsr;
} FloatContext;

typedef struct {
    uint64_t vl, vtype, vstart, vcsr;
} VectorControl;

static_assert(offsetof(FloatContext, fcsr) == RS_FP_CSR_OFFSET);
static_assert(sizeof(VectorControl) == 32);
static FloatContext fp[USER_TASK_COUNT];
static VectorControl vector[USER_TASK_COUNT];
extern uint8_t __context_begin[], __context_limit[];
static uint8_t *registers[USER_TASK_COUNT];
static size_t vector_bytes;
static uint64_t enabled;
static int owner = -1;

uint64_t enable_extensions(size_t *vlenb);
void save_float(FloatContext *state);
void restore_float(const FloatContext *state);
void save_vector(VectorControl *state, uint8_t *registers);
void restore_vector(const VectorControl *state, const uint8_t *registers);

bool context_init(void)
{
    size_t vlenb = 0;
    enabled = enable_extensions(&vlenb);
    uintptr_t begin = (uintptr_t)__context_begin, end = (uintptr_t)__context_limit;
    if (begin > end || vlenb > (end - begin) / (32 * USER_TASK_COUNT)) {
        return false;
    }
    vector_bytes = 32 * vlenb;
    for (unsigned i = 0; i < USER_TASK_COUNT; ++i) {
        registers[i] = (uint8_t *)(begin + i * vector_bytes);
    }
    return true;
}

void context_drop(unsigned slot)
{
    if (owner == (int)slot) {
        owner = -1;
    }
}

uint64_t context_reset(unsigned slot)
{
    context_drop(slot);
    memset(&fp[slot], 0, sizeof(fp[slot]));
    vector[slot] = (VectorControl){.vtype = UINT64_C(1) << 63};
    memset(registers[slot], 0, vector_bytes);
    return enabled;
}

static void save(unsigned slot)
{
    if (enabled & RS_FS_INITIAL) {
        save_float(&fp[slot]);
    }
    if (vector_bytes) {
        save_vector(&vector[slot], registers[slot]);
    }
}

void context_select(unsigned slot)
{
    if (owner == (int)slot) {
        return;
    }
    if (owner >= 0) {
        save((unsigned)owner);
    }
    if (enabled & RS_FS_INITIAL) {
        restore_float(&fp[slot]);
    }
    if (vector_bytes) {
        restore_vector(&vector[slot], registers[slot]);
    }
    owner = (int)slot;
}

void context_fork(unsigned parent, unsigned child)
{
    /* fork 可能在两次切换之间发生，父进程最新的值仍在 hart 上。 */
    save(parent);
    fp[child] = fp[parent];
    vector[child] = vector[parent];
    memcpy(registers[child], registers[parent], vector_bytes);
}
