# StreamCache Two-Layer Memory Management

## Overview

StreamCache replaces the kernel's buddy allocator on the page cache allocation
hot path. Pages are pre-allocated into per-CPU regions at enable time, then
served from small per-file caches that batch-refill from the pool. Evicted
pages are automatically recycled back to the pool (detected via the `PG_scache`
page flag).

**Result**: near-zero allocation latency for order-0 page cache pages on both
the read and write paths.

**Filesystem support**: The hooks are in the generic VFS/mm layer
(`__filemap_get_folio`, `page_cache_ra_unbounded`, `__folio_put_small`,
`inode_init_always`/`evict`). Any filesystem that uses the standard page cache
benefits automatically — ext4, XFS, btrfs, etc. No per-filesystem opt-in is
needed.

## Quick Start

```bash
# Enable with defaults (nr_regions = num CPUs, 2GB per region)
echo 1 > /sys/fs/sc_memory/enabled

# Verify
cat /sys/fs/sc_memory/pool_free_pages
cat /sys/fs/sc_memory/memory_flag       # should show "normal"

# Disable (frees all pool memory back to the kernel)
echo 0 > /sys/fs/sc_memory/enabled
```

Once enabled, all standard read/write page cache allocations automatically
use the pool. No per-file or per-filesystem configuration is needed.

## sysfs Interface

All nodes are under `/sys/fs/sc_memory/`.

### Read-Write Parameters

| Node | Default | Description |
|------|---------|-------------|
| `enabled` | `0` | `0` = disabled (normal buddy allocator), `1` = pool active |
| `nr_regions` | `num_possible_cpus()` | Number of per-CPU pool regions. Change while disabled. |
| `pages_per_region` | `524288` (2GB) | Pages per region. Change while disabled. |
| `threshold_low` | `20` | Free ratio (%) below which memory flag becomes LOW |
| `threshold_urgent` | `5` | Free ratio (%) below which memory flag becomes URGENT |
| `low_mark` | `50` | Per-file cache batch refill size (pages) |
| `high_mark` | `100` | Per-file cache maximum capacity (pages) |

### Read-Only Status

| Node | Description |
|------|-------------|
| `memory_flag` | Current pressure level: `normal`, `low`, `urgent`, or `disabled` |
| `pool_free_pages` | Total free pages across all regions |

## Configuration

### Adjusting Pool Size

Pool size parameters (`nr_regions`, `pages_per_region`) can only be changed
while the pool is disabled:

```bash
echo 0 > /sys/fs/sc_memory/enabled

# Example: 32 regions x 1GB each = 32GB total
echo 32 > /sys/fs/sc_memory/nr_regions
echo 262144 > /sys/fs/sc_memory/pages_per_region   # 262144 * 4KB = 1GB

echo 1 > /sys/fs/sc_memory/enabled
```

### Tuning Per-File Cache Watermarks

These take effect for newly initialized inodes. Can be changed at any time:

```bash
echo 64  > /sys/fs/sc_memory/low_mark     # refill 64 pages at a time
echo 128 > /sys/fs/sc_memory/high_mark    # keep up to 128 pages per file
```

Larger values reduce refill frequency (fewer spinlock acquisitions) but
increase per-inode memory overhead for idle files.

### Tuning Memory Pressure Thresholds

These can be changed while the pool is enabled:

```bash
echo 30 > /sys/fs/sc_memory/threshold_low
echo 10 > /sys/fs/sc_memory/threshold_urgent
```

A background monitor thread should periodically call `sc_update_memory_flag()`
to update the flag. When the flag reaches URGENT, calling
`sc_shrink_all_caches()` returns half of each per-file cache to the pool.

## Page Lifecycle

```
                    batch refill (low_mark pages)
  Pool Region ──────────────────────────────────────> Per-File Cache
  (per-CPU,                                          (per-inode,
   spinlock)                                          no lock needed)
      ^                                                    |
      |                                             sc_alloc_page()
      |                                                    |
      |                                                    v
      |                                              Page Cache
      |                                            (struct page in
      |                                             xarray, PG_scache
      |                                             flag set)
      |                                                    |
      |                                              eviction by
      |                                              kswapd / reclaim /
      |                                              truncate
      |                                                    |
      |                                                    v
      |                                           __folio_put_small()
      |                                            checks PG_scache
      |                                            zeros page
      +<───────────────────────────────────────── returns to pool
```

## Scope of Integration

The pool is currently wired into:

- **`__filemap_get_folio()`** (`mm/filemap.c`) — the `FGP_CREAT` allocation
  path used by buffered writes and synchronous reads. Only order-0 allocations
  are intercepted; higher-order folio allocations use the normal path.

- **`page_cache_ra_unbounded()`** (`mm/readahead.c`) — the readahead engine.
  All readahead allocations are order-0 here.

- **`__folio_put_small()`** (`mm/swap.c`) — the final free path for order-0
  pages. SC-allocated pages are recycled to the pool instead of the buddy
  allocator.

- **`struct inode`** (`include/linux/fs.h`) — each inode embeds a
  `sc_per_file_cache`. Initialized in `inode_init_always()`, destroyed in
  `evict()`.

**Not** integrated (by design):

- The background buffered write path (`mm/filemap_bg.c`) — uses its own folio
  pool.
- Order > 0 folio allocations (large folios, THP).

## Self-Tests

```bash
# Build with CONFIG_SCACHE_MEMORY_TEST=m
insmod drivers/scache/sc_test.ko
dmesg | grep sc_test
```

The test module runs 8 tests at load time and auto-unloads:

1. Pool init/destroy accounting
2. Basic alloc/free with batch refill trigger
3. Explicit batch refill
4. High-mark overflow (excess pages return to pool)
5. Memory flag transitions (NORMAL -> LOW -> URGENT)
6. URGENT shrink (halves each per-file cache)
7. Pool exhaustion (returns NULL)
8. Per-file cache destroy (all pages returned to pool)

Expected output:
```
sc_test: === Results: 8/8 tests passed ===
```

## Build Configuration

```
CONFIG_SCACHE_MEMORY=y          # or =m for module
CONFIG_SCACHE_MEMORY_TEST=m     # optional, for self-tests
```

## Files

| File | Purpose |
|------|---------|
| `include/linux/sc_memory.h` | Public header: structs, constants, API |
| `include/linux/page-flags.h` | `PG_scache` flag definition |
| `include/trace/events/mmflags.h` | `PG_scache` trace name |
| `drivers/scache/sc_memory_core.c` | Pool, per-file cache, alloc/free |
| `drivers/scache/sc_sysfs.c` | sysfs interface |
| `drivers/scache/sc_init.c` | Module init/exit |
| `drivers/scache/sc_test.c` | Self-test module |
| `mm/filemap.c` | Write path hook |
| `mm/readahead.c` | Read path hook |
| `mm/swap.c` | Free path hook (page recycling) |
| `fs/inode.c` | Per-file cache init/destroy |
| `include/linux/fs.h` | `i_sc_cache` field in `struct inode` |
