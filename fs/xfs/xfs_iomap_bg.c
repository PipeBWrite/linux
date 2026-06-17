// SPDX-License-Identifier: GPL-2.0
/*
 * Background buffered write path for XFS iomap.
 *
 * Foreground: memcpy into pagecache folios with minimal bookkeeping.
 * Background: iomap write_begin/write_end to update metadata and dirtying.
 *
 * Background writes may fall back to the original path when the start page
 * cannot be safely handled by the bg path.
 */

#include "linux/printk.h"
#include "xfs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_iomap.h"
#include "xfs_iomap_bg.h"
#include "xfs_bmap.h"
#include "xfs_bmap_util.h"
#include "xfs_reflink.h"
#include "xfs_errortag.h"
#include "xfs_error.h"

#include <linux/backing-dev.h>
#include <linux/dsa_sl.h>
#include <linux/iomap.h>
#include <linux/kernel.h>
#include <linux/pagemap_bg.h>
#include <linux/workqueue.h>
#include <linux/writeback.h>

/*
 * Foreground blk_write_begin for XFS: read partial blocks from disk before
 * the memcpy so the background iomap_write_begin_bg doesn't overwrite user
 * data with stale disk contents.  Folio must be locked by caller.
 */
int xfs_blk_write_begin_bg(struct folio *folio, loff_t pos, unsigned len)
{
	struct inode *inode = folio->mapping->host;
	struct iomap_iter iter = {
		.inode	= inode,
		.pos	= pos,
		.len	= len,
		.flags	= IOMAP_WRITE,
	};
	int ret;

	while ((ret = iomap_iter(&iter, &xfs_buffered_write_iomap_ops)) > 0) {
		size_t bytes = min_t(size_t, iomap_length(&iter),
				     folio_size(folio) -
				     offset_in_folio(folio, iter.pos));

		ret = iomap_write_begin_folio(&iter, iter.pos, bytes, folio);
		if (ret) {
			iter.processed = ret;
			continue;
		}
		iter.processed = bytes;
	}

	return ret < 0 ? ret : 0;
}

static int xfs_iomap_bg_write_entry(struct dsa_emu_memcpy_req *req,
				    struct dsa_emu_req_entry *ent)
{
	struct address_space *mapping = req->mapping;
	struct inode *inode = mapping->host;
	struct folio *folio = page_folio(ent->page);
	struct iomap_iter iter = {
		.inode = inode,
		.pos = ent->pos,
		.len = ent->bytes,
		.flags = IOMAP_WRITE,
	};
	gfp_t gfp = mapping_gfp_mask(mapping);
	bool folio_lock_owned = false;
	bool full_folio_write;
	int ret;

	if ((iter.flags & IOMAP_WRITE) && mapping_can_writeback(mapping))
		gfp |= __GFP_WRITE;

	full_folio_write = offset_in_folio(folio, ent->pos) == 0 &&
			   ent->bytes == folio_size(folio);

	if (ent->initial_alloc && ent->page == &folio->page) {
		if (full_folio_write)
			ret = filemap_add_folio_test_dirty(mapping, folio,
							   ent->index, gfp);
		else
			ret = filemap_add_folio_test(mapping, folio, ent->index,
						     gfp);
		if (ret)
			goto out_put;
		/* Both add helpers return with the folio locked on success. */
	} else {
		folio_lock(folio);
	}
	folio_lock_owned = true;

	/*
	 * Hold the folio lock CONTINUOUSLY across all extents of this entry.
	 * The previous scheme (unlock in write_end, re-infer ownership via
	 * folio_test_locked in the next write_begin) allowed another locker
	 * to slip in between extents and have its lock stolen, leaving a
	 * permanently locked folio behind.  begin/end _bg variants neither
	 * lock nor unlock; we unlock exactly once below.
	 */
	while ((ret = iomap_iter(&iter, &xfs_buffered_write_iomap_bg_ops)) > 0) {
		size_t bytes = iomap_length(&iter);
		size_t offset = offset_in_folio(folio, iter.pos);

		if (bytes > folio_size(folio) - offset)
			bytes = folio_size(folio) - offset;

		ret = iomap_write_begin_bg(&iter, iter.pos, bytes, folio);
		if (ret) {
			iter.processed = ret;
			continue;
		}
		if (iter.iomap.flags & IOMAP_F_STALE) {
			iter.processed = 0;
			continue;
		}

		bytes = iomap_write_end_bg_locked(&iter, iter.pos, bytes,
						  bytes, folio);
		iter.processed = bytes;
	}

out_put:
	if (folio_lock_owned)
		folio_unlock(folio);
	/* Drop the request entry's folio reference. */
	folio_put(folio);

	if (ret < 0)
		return ret;

	return 0;
}

ssize_t xfs_iomap_file_buffered_write_bg(struct kiocb *iocb,
					 struct iov_iter *from)
{
	size_t total = iov_iter_count(from);

	if (!total)
		return 0;

	return generic_perform_write_bg_cb(iocb, from,
					   xfs_iomap_bg_write_entry, true);
}
