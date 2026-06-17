#ifndef DSA_EMU_FBG_H
#define DSA_EMU_FBG_H

#include <linux/compiler.h>

// Enable background allocation thread
#define DSA_EMU_BG_ALLOC_THREAD

// Whether to use static folio pool
#define DSA_EMU_STATIC_FOLIO_POOL

// The number of entries to submit as a request
#define DSA_EMU_SUBMIT_REQ_ENTRIES 16

// Per-CPU FG batching thresholds for tiny writes
#define DSA_EMU_FG_BATCH_MAX_ENTRIES 32
#define DSA_EMU_FG_BATCH_MAX_BYTES (32 * 4096)
#define DSA_EMU_FG_BATCH_SMALL_WRITE_MAX 2048

// Number of entries in o-list
#define ONGOING_HASH_NUM 1024

// The number of wqs (threads) for BG tasks
#define DSA_EMU_BG_WQ_COUNT 1

// Max number of threads
#define DSA_EMU_NUM_THREADS_MAX 160

// Folio pool configuration (order 1..8 default)
#define DSA_EMU_FOLIO_POOL_ORDER_MIN 1
#define DSA_EMU_FOLIO_POOL_ORDER_MAX 8
#define DSA_EMU_FOLIO_POOL_SIZE 32

// Static folio pool size (order 0, 4KB) - larger for ext4 which only uses 4KB folios
#define DSA_EMU_STATIC_FOLIO_POOL_SIZE 1024
#define DSA_EMU_STATIC_FOLIO_POOL_SIZE_MAX 1024

// Default dirty threshold for debug
#define DSA_EMU_DEBUG_DIRTY_THRESHOLD_DEFAULT 65536

// Default pending-entry threshold for using the foreground folio pool allocator
#define DSA_EMU_FG_ALLOC_THRESHOLD_DEFAULT 100

// Number of snapshot counter for BG allocation
#define DSA_EMU_BG_SNAPSHOT_COUNTER 4

#ifdef CONFIG_INTEL_DSA_SL_EMU
extern uint32_t dsa_emu_folio_pool_order_max;
#endif

static inline unsigned int dsa_emu_folio_pool_order_max_read(void)
{
#ifdef CONFIG_INTEL_DSA_SL_EMU
	return READ_ONCE(dsa_emu_folio_pool_order_max);
#else
	return DSA_EMU_FOLIO_POOL_ORDER_MAX;
#endif
}

#endif
