/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __XFS_IOMAP_BG_H__
#define __XFS_IOMAP_BG_H__

#include <linux/fs.h>
#include <linux/uio.h>

ssize_t xfs_iomap_file_buffered_write_bg(struct kiocb *iocb,
					 struct iov_iter *from);
int xfs_blk_write_begin_bg(struct folio *folio, loff_t pos, unsigned len);

#endif /* __XFS_IOMAP_BG_H__ */
