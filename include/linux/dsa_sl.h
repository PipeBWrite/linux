#ifndef DSA_SL_KERN_API_H
#define DSA_SL_KERN_API_H

#include "linux/uio.h"
#include <linux/cache.h>
#include <linux/types.h>
#include <linux/kthread.h>
#include <linux/fbg.h>
#include <linux/llist.h>
#include <linux/slab.h>

#ifdef CONFIG_INTEL_DSA_SL_EMU

struct ongoing_node;

/*
 * dsa_emu_req_entry: Request entry for FG/BG buffered write separation.
 *
 * Structure layout is optimized to reduce cache line bouncing between
 * FG (writer) and BG (reader) threads:
 *
 * Cache line 0 (0x00-0x3F): FG-write hot fields
 *   - Written by FG in __filemap_get_folio_bg
 *   - Read by BG in process_req/dsa_emu_process_req_entries
 *   - Dedicated cache line to minimize RFO stalls on FG writes
 *
 * Cache line 1 (0x40-0x7F): BG-read/write fields + list
 *   - pos, bytes, copied: written later in prepare_dsa_emu_req_entry
 *   - fsdata: set during prepare, read by BG
 *   - list: o-list operations (spin_lock protected)
 */
struct dsa_emu_req_entry {
	/*
	 * === Cache line 0: FG-write hot fields ===
	 * These are written by FG thread in __filemap_get_folio_bg.
	 * Grouped together so a single prefetchw_for_write() brings
	 * the entire write set into exclusive cache state.
	 */
	struct page *page;		/* 0x00: folio page pointer */
	unsigned long index;		/* 0x08: page cache index */
	u8 order;			/* 0x10: folio order */
	bool initial_alloc;		/* 0x11: true if newly allocated */
	bool static_folio;		/* 0x12: from static folio pool */
	u8 __pad0;			/* 0x13: padding */
	s16 folio_idx;			/* 0x14: pool CPU index (-1 if none) */
	s16 target_wq_idx;		/* 0x16: BG queue hint */
	/* 0x18-0x3F: reserved for future FG fields */
	u64 __reserved_fg[5];		/* 0x18: padding to cache line */

	/*
	 * === Cache line 1: BG-access fields + list ===
	 * Written later in prepare_dsa_emu_req_entry, read by BG.
	 */
	void *fsdata;			/* 0x48: filesystem private data */
	loff_t pos;			/* 0x50: file position */
	u32 bytes;			/* 0x58: bytes to write */
	u32 copied;			/* 0x5C: bytes actually copied */
	struct ongoing_node *onode;	/* 0x60: decoupled o-list node */
	u64 __onode_pad;		/* 0x68: maintain 128B struct size */
	u32 flags;			/* 0x70: entry flags */
	u32 __pad2;			/* 0x74: padding */
	u64 __reserved_bg[1];		/* 0x78: padding to cache line */
} ____cacheline_aligned;

#ifdef CONFIG_SMP
/* Each entry is exactly 2 cache lines (128 bytes on 64-byte cache line) */
static_assert(sizeof(struct dsa_emu_req_entry) == 2 * SMP_CACHE_BYTES);
#endif

#define DSA_EMU_REQ_F_FG_BWB	BIT(0) /* blk_write_begin done in FG */

static inline bool dsa_emu_entry_covers_index(struct dsa_emu_req_entry *ent,
					      pgoff_t index)
{
	unsigned int pages = 1U << ent->order;

	return index >= ent->index && index < ent->index + pages;
}

struct dsa_emu_memcpy_req;

/*
 * Callback type for filesystem-specific write operations.
 * Called by the generic dsa_emu_process_req_entries() for each entry.
 * The full request is provided so callbacks can access req->file and
 * other shared context without re-plumbing arguments.
 * Returns 0 on success, negative errno on failure.
 */
typedef int (*dsa_emu_write_entry_fn_t)(struct dsa_emu_memcpy_req *req,
					struct dsa_emu_req_entry *ent);

struct dsa_emu_memcpy_req {
	struct llist_node lnode;	/* lockless list for submission */

	struct address_space *mapping;
	struct file *file;

	int n_ents;
	pgoff_t start_index;
	pgoff_t end_index;

	bool from_pool;
	int pool_cpu;

	int target_wq_idx;

	bool fsync;

	/* Filesystem-specific write callback for batched processing */
	dsa_emu_write_entry_fn_t write_fn;

	/* Inline storage for request entries. */
	struct dsa_emu_req_entry entries[DSA_EMU_SUBMIT_REQ_ENTRIES];
};

int dsa_emu_thread_init(void);
int dsa_emu_thread_stop(void);
struct dsa_emu_memcpy_req *dsa_emu_get_req_from_pool(int idx);
void dsa_emu_put_req(struct dsa_emu_memcpy_req *req);

int dsa_emu_select_wq(int hint);
bool dsa_emu_is_backed_off(int wq_idx);
bool dsa_emu_should_use_orig_fallback(void);
void dsa_emu_submit(struct dsa_emu_memcpy_req *req, struct inode *inode);
void dsa_emu_submit_in_drain(struct dsa_emu_memcpy_req *req,
			     struct inode *inode);
void dsa_emu_submit_batch(struct dsa_emu_memcpy_req **reqs, unsigned int nr,
			  struct inode *inode);
void dsa_emu_submit_batch_in_drain(struct dsa_emu_memcpy_req **reqs,
				   unsigned int nr, struct inode *inode);
int dsa_emu_drain(struct inode *inode);

/*
 * Generic request processing function.
 * Handles: entry iteration, busy waiting, o-list release, static folio
 * replenishment, balance_dirty_pages, iput, cleanup.
 * Calls write_fn for each entry that needs filesystem-specific write handling.
 */
void dsa_emu_process_req_entries(struct dsa_emu_memcpy_req *req,
				 dsa_emu_write_entry_fn_t write_fn);
void dsa_emu_process_req_sync(struct dsa_emu_memcpy_req *req,
			      struct inode *inode);

#ifdef DSA_EMU_BG_ALLOC_THREAD
int get_folio(unsigned int order, bool force, bool *static_folio,
	      int *folio_idx, int count,
	      struct folio **folios);
bool check_and_preget_head(struct inode *inode);
void put_static_folio(struct folio *f, int idx);
#endif

#endif

#endif
