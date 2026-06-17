// SPDX-License-Identifier: GPL-2.0
/*
 * High-level sync()-related operations
 */

#include "linux/dsa_emu.h"
#include <linux/blkdev.h>
#include <linux/kernel.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/namei.h>
#include <linux/sched.h>
#include <linux/writeback.h>
#include <linux/syscalls.h>
#include <linux/linkage.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>
#include <linux/backing-dev.h>
#include <linux/pagemap_bg.h>
#include <linux/dsa_sl.h>
#include <linux/math64.h>
#include <linux/stats.h>
#include "internal.h"

#define VALID_FLAGS (SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE| \
			SYNC_FILE_RANGE_WAIT_AFTER)

#ifdef CONFIG_MISC_STATS
#define FSYNC_FB_STAT_INC(name) this_cpu_inc(stats_##name##_count)
#define FSYNC_FB_STAT_ADD(name, count) this_cpu_add(stats_##name##_count, count)
#else
#define FSYNC_FB_STAT_INC(name) \
	do {                    \
	} while (0)
#define FSYNC_FB_STAT_ADD(name, count) \
	do {                         \
	} while (0)
#endif

static bool filemap_bg_fsync_fallback_threshold_hit(u64 writes, int pending,
						    u32 numerator,
						    u32 denominator)
{
	u64 scaled_pending;

	if (!numerator)
		numerator = 1;

	scaled_pending = (u64)pending * denominator;
	return div64_u64(scaled_pending, numerator) >= writes;
}

static void filemap_bg_update_fsync_fallback(struct file *file, int pending)
{
	u32 numerator = READ_ONCE(dsa_emu_fsync_fallback_numerator);
	u32 denominator = READ_ONCE(dsa_emu_fsync_fallback_denominator);
	u32 recovery_threshold =
		READ_ONCE(dsa_emu_fsync_fallback_recovery_writes);
	u64 writes = READ_ONCE(file->f_bg_writes_since_fsync);
	bool old_fallback = READ_ONCE(file->f_bg_fsync_fallback);
	bool fallback = false;

	FSYNC_FB_STAT_INC(fsync_fb_eval);
	FSYNC_FB_STAT_ADD(fsync_fb_writes_sum, writes);
	if (pending > 0)
		FSYNC_FB_STAT_ADD(fsync_fb_pending_sum, pending);

	WRITE_ONCE(file->f_bg_writes_since_fsync, 0);

	if (!denominator) {
		FSYNC_FB_STAT_INC(fsync_fb_disabled);
		WRITE_ONCE(file->f_bg_fsync_recovery_writes, 0);
		goto update;
	}

	if (!writes) {
		FSYNC_FB_STAT_INC(fsync_fb_no_writes);
		WRITE_ONCE(file->f_bg_fsync_recovery_writes, 0);
		goto update;
	}

	if (pending <= 0) {
		FSYNC_FB_STAT_INC(fsync_fb_pending_zero);
		if (old_fallback) {
			u64 recovery_writes =
				READ_ONCE(file->f_bg_fsync_recovery_writes);

			if (recovery_writes < U64_MAX - writes)
				recovery_writes += writes;
			else
				recovery_writes = U64_MAX;

			if (recovery_writes < recovery_threshold) {
				fallback = true;
				WRITE_ONCE(file->f_bg_fsync_recovery_writes,
					   recovery_writes);
			} else {
				WRITE_ONCE(file->f_bg_fsync_recovery_writes, 0);
			}
		}
		goto update;
	}

	WRITE_ONCE(file->f_bg_fsync_recovery_writes, 0);

	if (filemap_bg_fsync_fallback_threshold_hit(writes, pending, numerator,
						    denominator)) {
		FSYNC_FB_STAT_INC(fsync_fb_threshold_hit);
		fallback = true;
	} else {
		FSYNC_FB_STAT_INC(fsync_fb_threshold_miss);
	}

update:
	if (fallback) {
		if (old_fallback)
			FSYNC_FB_STAT_INC(fsync_fb_keep_on);
		else
			FSYNC_FB_STAT_INC(fsync_fb_set);
	} else {
		if (old_fallback)
			FSYNC_FB_STAT_INC(fsync_fb_clear);
		else
			FSYNC_FB_STAT_INC(fsync_fb_keep_off);
	}
	WRITE_ONCE(file->f_bg_fsync_fallback, fallback);
}

/*
 * Write out and wait upon all dirty data associated with this
 * superblock.  Filesystem data as well as the underlying block
 * device.  Takes the superblock lock.
 */
int sync_filesystem(struct super_block *sb)
{
	int ret = 0;

	/*
	 * We need to be protected against the filesystem going from
	 * r/o to r/w or vice versa.
	 */
	WARN_ON(!rwsem_is_locked(&sb->s_umount));

	/*
	 * No point in syncing out anything if the filesystem is read-only.
	 */
	if (sb_rdonly(sb))
		return 0;

	/*
	 * Do the filesystem syncing work.  For simple filesystems
	 * writeback_inodes_sb(sb) just dirties buffers with inodes so we have
	 * to submit I/O for these buffers via sync_blockdev().  This also
	 * speeds up the wait == 1 case since in that case write_inode()
	 * methods call sync_dirty_buffer() and thus effectively write one block
	 * at a time.
	 */
	writeback_inodes_sb(sb, WB_REASON_SYNC);
	if (sb->s_op->sync_fs) {
		ret = sb->s_op->sync_fs(sb, 0);
		if (ret)
			return ret;
	}
	ret = sync_blockdev_nowait(sb->s_bdev);
	if (ret)
		return ret;

	sync_inodes_sb(sb);
	if (sb->s_op->sync_fs) {
		ret = sb->s_op->sync_fs(sb, 1);
		if (ret)
			return ret;
	}
	return sync_blockdev(sb->s_bdev);
}
EXPORT_SYMBOL(sync_filesystem);

static void sync_inodes_one_sb(struct super_block *sb, void *arg)
{
	if (!sb_rdonly(sb))
		sync_inodes_sb(sb);
}

static void sync_fs_one_sb(struct super_block *sb, void *arg)
{
	if (!sb_rdonly(sb) && !(sb->s_iflags & SB_I_SKIP_SYNC) &&
	    sb->s_op->sync_fs)
		sb->s_op->sync_fs(sb, *(int *)arg);
}

/*
 * Sync everything. We start by waking flusher threads so that most of
 * writeback runs on all devices in parallel. Then we sync all inodes reliably
 * which effectively also waits for all flusher threads to finish doing
 * writeback. At this point all data is on disk so metadata should be stable
 * and we tell filesystems to sync their metadata via ->sync_fs() calls.
 * Finally, we writeout all block devices because some filesystems (e.g. ext2)
 * just write metadata (such as inodes or bitmaps) to block device page cache
 * and do not sync it on their own in ->sync_fs().
 */
void ksys_sync(void)
{
	int nowait = 0, wait = 1;

	wakeup_flusher_threads(WB_REASON_SYNC);
	iterate_supers(sync_inodes_one_sb, NULL);
	iterate_supers(sync_fs_one_sb, &nowait);
	iterate_supers(sync_fs_one_sb, &wait);
	sync_bdevs(false);
	sync_bdevs(true);
	if (unlikely(laptop_mode))
		laptop_sync_completion();
}

SYSCALL_DEFINE0(sync)
{
	ksys_sync();
	return 0;
}

static void do_sync_work(struct work_struct *work)
{
	int nowait = 0;

	/*
	 * Sync twice to reduce the possibility we skipped some inodes / pages
	 * because they were temporarily locked
	 */
	iterate_supers(sync_inodes_one_sb, &nowait);
	iterate_supers(sync_fs_one_sb, &nowait);
	sync_bdevs(false);
	iterate_supers(sync_inodes_one_sb, &nowait);
	iterate_supers(sync_fs_one_sb, &nowait);
	sync_bdevs(false);
	printk("Emergency Sync complete\n");
	kfree(work);
}

void emergency_sync(void)
{
	struct work_struct *work;

	work = kmalloc(sizeof(*work), GFP_ATOMIC);
	if (work) {
		INIT_WORK(work, do_sync_work);
		schedule_work(work);
	}
}

/*
 * sync a single super
 */
SYSCALL_DEFINE1(syncfs, int, fd)
{
	struct fd f = fdget(fd);
	struct super_block *sb;
	int ret, ret2;

	if (!f.file)
		return -EBADF;
	sb = f.file->f_path.dentry->d_sb;

	down_read(&sb->s_umount);
	ret = sync_filesystem(sb);
	up_read(&sb->s_umount);

	ret2 = errseq_check_and_advance(&sb->s_wb_err, &f.file->f_sb_err);

	fdput(f);
	return ret ? ret : ret2;
}

/**
 * vfs_fsync_range - helper to sync a range of data & metadata to disk
 * @file:		file to sync
 * @start:		offset in bytes of the beginning of data range to sync
 * @end:		offset in bytes of the end of data range (inclusive)
 * @datasync:		perform only datasync
 *
 * Write back data in range @start..@end and metadata for @file to disk.  If
 * @datasync is set only metadata needed to access modified file data is
 * written.
 */
int vfs_fsync_range(struct file *file, loff_t start, loff_t end, int datasync)
{
	if (!file->f_op->fsync) {
		return -EINVAL;
	}
	struct inode *inode = file->f_mapping->host;

	if (dsa_emu_fsync_skip) {
		return 0;
	}

	if (filemap_bg_compatible(inode)) {
		int drain_pending;

		/*
		 * Adaptive bg-fsync enabler (debug_bg_fsync == 2 "auto"):
		 * tasks that repeatedly fsync while the inode BG queue is
		 * calm get per-task bg_fsync_on (their subsequent writes are
		 * kernel-paced); heavy backlog turns it off.  Keyed to the
		 * fsync CALLER, so flush/compaction writers are paced while
		 * WAL appenders (who rarely fsync) stay untaxed.
		 * Gated on bg_pace_safe: filesystems whose write path takes
		 * an exclusive per-inode lock per entry (xfs ILOCK_EXCL)
		 * convoy behind paced writeback, so never flag tasks there.
		 */
		if (READ_ONCE(dsa_emu_debug_bg_fsync) == 2 &&
		    inode->i_mapping->a_ops->bg_pace_safe) {
			int curr_ongoing = atomic_read(&inode->i_bg_pending);

			if (!current->bg_fsync_on) {
				int v = 0;

				if (curr_ongoing < 1000)
					v = atomic_inc_return(
						&current->bg_fsync_count);
				else
					atomic_dec(&current->bg_fsync_count);
				if (v > 1)
					current->bg_fsync_on = true;
			} else if (curr_ongoing > 256) {
				current->bg_fsync_on = false;
				atomic_set(&current->bg_fsync_count, 0);
			}
		}

		FSYNC_FB_STAT_INC(fsync_bg_compatible);
		if (READ_ONCE(file->f_bg_fsync_fallback))
			FSYNC_FB_STAT_INC(fsync_fb_entry_on);
		else
			FSYNC_FB_STAT_INC(fsync_fb_entry_off);
		FSYNC_FB_STAT_INC(fsync_drain_called);

		drain_pending = dsa_emu_drain(inode);
		if (drain_pending > 0)
			FSYNC_FB_STAT_INC(fsync_drain_pending);
		else
			FSYNC_FB_STAT_INC(fsync_drain_empty);

		filemap_bg_update_fsync_fallback(file, drain_pending);
	}

	if (!datasync && (inode->i_state & I_DIRTY_TIME))
		mark_inode_dirty_sync(inode);
	int ret = file->f_op->fsync(file, start, end, datasync);

	return ret;
}
EXPORT_SYMBOL(vfs_fsync_range);

/**
 * vfs_fsync - perform a fsync or fdatasync on a file
 * @file:		file to sync
 * @datasync:		only perform a fdatasync operation
 *
 * Write back data and metadata for @file to disk.  If @datasync is
 * set only metadata needed to access modified file data is written.
 */
int vfs_fsync(struct file *file, int datasync)
{
	return vfs_fsync_range(file, 0, LLONG_MAX, datasync);
}
EXPORT_SYMBOL(vfs_fsync);

static int do_fsync(unsigned int fd, int datasync)
{
	struct fd f = fdget(fd);
	int ret = -EBADF;

	if (f.file) {
		ret = vfs_fsync(f.file, datasync);
		fdput(f);
	}
	return ret;
}

SYSCALL_DEFINE1(fsync, unsigned int, fd)
{
	return do_fsync(fd, 0);
}

SYSCALL_DEFINE1(fdatasync, unsigned int, fd)
{
	return do_fsync(fd, 1);
}

int sync_file_range(struct file *file, loff_t offset, loff_t nbytes,
		    unsigned int flags)
{
	int ret;
	struct address_space *mapping;
	loff_t endbyte;			/* inclusive */
	umode_t i_mode;

	ret = -EINVAL;
	if (flags & ~VALID_FLAGS)
		goto out;

	endbyte = offset + nbytes;

	if ((s64)offset < 0)
		goto out;
	if ((s64)endbyte < 0)
		goto out;
	if (endbyte < offset)
		goto out;

	if (sizeof(pgoff_t) == 4) {
		if (offset >= (0x100000000ULL << PAGE_SHIFT)) {
			/*
			 * The range starts outside a 32 bit machine's
			 * pagecache addressing capabilities.  Let it "succeed"
			 */
			ret = 0;
			goto out;
		}
		if (endbyte >= (0x100000000ULL << PAGE_SHIFT)) {
			/*
			 * Out to EOF
			 */
			nbytes = 0;
		}
	}

	if (nbytes == 0)
		endbyte = LLONG_MAX;
	else
		endbyte--;		/* inclusive */

	i_mode = file_inode(file)->i_mode;
	ret = -ESPIPE;
	if (!S_ISREG(i_mode) && !S_ISBLK(i_mode) && !S_ISDIR(i_mode) &&
			!S_ISLNK(i_mode))
		goto out;

	mapping = file->f_mapping;
	ret = 0;

	/*
	 * PBW: write() returns before BG marks pages dirty, so a range-write
	 * hint racing the BG pipeline skips the request-sized islands BG has
	 * not completed yet (~4 pages each per 1MB).  Those islands dirty
	 * later and the final fsync pays one map_blocks round + journal
	 * handle per island.
	 * Drain the inode's BG backlog first so the hint writes the full
	 * contiguous range.
	 */
	if (READ_ONCE(dsa_emu_sfr_drain) && (flags & SYNC_FILE_RANGE_WRITE) &&
	    filemap_bg_compatible(mapping->host))
		dsa_emu_drain(mapping->host);

	if (flags & SYNC_FILE_RANGE_WAIT_BEFORE) {
		ret = file_fdatawait_range(file, offset, endbyte);
		if (ret < 0)
			goto out;
	}

	if (flags & SYNC_FILE_RANGE_WRITE) {
		int sync_mode = WB_SYNC_NONE;

		if ((flags & SYNC_FILE_RANGE_WRITE_AND_WAIT) ==
			     SYNC_FILE_RANGE_WRITE_AND_WAIT)
			sync_mode = WB_SYNC_ALL;

		ret = __filemap_fdatawrite_range(mapping, offset, endbyte,
						 sync_mode);
		if (ret < 0)
			goto out;
	}

	if (flags & SYNC_FILE_RANGE_WAIT_AFTER)
		ret = file_fdatawait_range(file, offset, endbyte);

out:
	return ret;
}

/*
 * ksys_sync_file_range() permits finely controlled syncing over a segment of
 * a file in the range offset .. (offset+nbytes-1) inclusive.  If nbytes is
 * zero then ksys_sync_file_range() will operate from offset out to EOF.
 *
 * The flag bits are:
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE: wait upon writeout of all pages in the range
 * before performing the write.
 *
 * SYNC_FILE_RANGE_WRITE: initiate writeout of all those dirty pages in the
 * range which are not presently under writeback. Note that this may block for
 * significant periods due to exhaustion of disk request structures.
 *
 * SYNC_FILE_RANGE_WAIT_AFTER: wait upon writeout of all pages in the range
 * after performing the write.
 *
 * Useful combinations of the flag bits are:
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE: ensures that all pages
 * in the range which were dirty on entry to ksys_sync_file_range() are placed
 * under writeout.  This is a start-write-for-data-integrity operation.
 *
 * SYNC_FILE_RANGE_WRITE: start writeout of all dirty pages in the range which
 * are not presently under writeout.  This is an asynchronous flush-to-disk
 * operation.  Not suitable for data integrity operations.
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE (or SYNC_FILE_RANGE_WAIT_AFTER): wait for
 * completion of writeout of all pages in the range.  This will be used after an
 * earlier SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE operation to wait
 * for that operation to complete and to return the result.
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE|SYNC_FILE_RANGE_WAIT_AFTER
 * (a.k.a. SYNC_FILE_RANGE_WRITE_AND_WAIT):
 * a traditional sync() operation.  This is a write-for-data-integrity operation
 * which will ensure that all pages in the range which were dirty on entry to
 * ksys_sync_file_range() are written to disk.  It should be noted that disk
 * caches are not flushed by this call, so there are no guarantees here that the
 * data will be available on disk after a crash.
 *
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE and SYNC_FILE_RANGE_WAIT_AFTER will detect any
 * I/O errors or ENOSPC conditions and will return those to the caller, after
 * clearing the EIO and ENOSPC flags in the address_space.
 *
 * It should be noted that none of these operations write out the file's
 * metadata.  So unless the application is strictly performing overwrites of
 * already-instantiated disk blocks, there are no guarantees here that the data
 * will be available after a crash.
 */
int ksys_sync_file_range(int fd, loff_t offset, loff_t nbytes,
			 unsigned int flags)
{
	int ret;
	struct fd f;

	ret = -EBADF;
	f = fdget(fd);
	if (f.file)
		ret = sync_file_range(f.file, offset, nbytes, flags);

	fdput(f);
	return ret;
}

SYSCALL_DEFINE4(sync_file_range, int, fd, loff_t, offset, loff_t, nbytes,
				unsigned int, flags)
{
	return ksys_sync_file_range(fd, offset, nbytes, flags);
}

#if defined(CONFIG_COMPAT) && defined(__ARCH_WANT_COMPAT_SYNC_FILE_RANGE)
COMPAT_SYSCALL_DEFINE6(sync_file_range, int, fd, compat_arg_u64_dual(offset),
		       compat_arg_u64_dual(nbytes), unsigned int, flags)
{
	return ksys_sync_file_range(fd, compat_arg_u64_glue(offset),
				    compat_arg_u64_glue(nbytes), flags);
}
#endif

/* It would be nice if people remember that not all the world's an i386
   when they introduce new system calls */
SYSCALL_DEFINE4(sync_file_range2, int, fd, unsigned int, flags,
				 loff_t, offset, loff_t, nbytes)
{
	return ksys_sync_file_range(fd, offset, nbytes, flags);
}
