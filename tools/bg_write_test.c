// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <unistd.h>

static size_t max_size(size_t a, size_t b)
{
	return a > b ? a : b;
}

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : "bg_write_test.dat";
	struct statfs sfs;
	long page_sz;
	size_t io_sz;
	void *buf = NULL;
	int fd;
	ssize_t io;
	size_t len = 32;
	off_t off;
	off_t read_off;
	char *small;
	size_t file_sz;
	int bad = 0;

	page_sz = sysconf(_SC_PAGESIZE);
	if (page_sz <= 0) {
		fprintf(stderr, "sysconf(_SC_PAGESIZE) failed\n");
		return 1;
	}

	if (statfs(path, &sfs) != 0) {
		if (errno != ENOENT || statfs(".", &sfs) != 0) {
			perror("statfs");
			return 1;
		}
	}

	io_sz = max_size((size_t)page_sz, (size_t)sfs.f_bsize);

	/*
	 * Default offset is page-aligned to avoid bg fallback for unaligned
	 * first-page writes. Override with argv[2] if desired.
	 */
	off = (argc > 2) ? (off_t)strtoll(argv[2], NULL, 0) : (off_t)page_sz;
	if (argc > 3) {
		len = (size_t)strtoul(argv[3], NULL, 0);
		if (len == 0) {
			fprintf(stderr, "len must be > 0\n");
			return 1;
		}
	}

	small = malloc(len);
	if (!small) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}

	if (posix_memalign(&buf, io_sz, io_sz) != 0) {
		fprintf(stderr, "posix_memalign failed\n");
		free(small);
		return 1;
	}

	memset(buf, 'A', io_sz);

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_DIRECT, 0644);
	if (fd < 0) {
		perror("open O_DIRECT (write)");
		free(buf);
		free(small);
		return 1;
	}

	file_sz = (size_t)(off + (off_t)len);
	if (file_sz < io_sz)
		file_sz = io_sz;
	/* O_DIRECT requires alignment; write in io_sz chunks */
	for (size_t written = 0; written < file_sz; written += io_sz) {
		io = write(fd, buf, io_sz);
		if (io != (ssize_t)io_sz) {
			fprintf(stderr, "direct write failed: %zd (%s)\n",
				io, strerror(errno));
			close(fd);
			free(buf);
			free(small);
			return 1;
		}
	}
	if (fsync(fd) != 0) {
		perror("fsync after direct write");
		close(fd);
		free(buf);
		free(small);
		return 1;
	}
	close(fd);

	memset(small, 'B', len);
	fd = open(path, O_RDWR);
	if (fd < 0) {
		perror("open buffered");
		free(buf);
		free(small);
		return 1;
	}
	io = pwrite(fd, small, len, off);
	if (io != (ssize_t)len) {
		fprintf(stderr, "buffered pwrite failed: %zd (%s)\n",
			io, strerror(errno));
		close(fd);
		free(buf);
		free(small);
		return 1;
	}
	if (fsync(fd) != 0) {
		perror("fsync after buffered write");
		close(fd);
		free(buf);
		free(small);
		return 1;
	}
	close(fd);

	memset(buf, 0, io_sz);
	fd = open(path, O_RDONLY | O_DIRECT);
	if (fd < 0) {
		perror("open O_DIRECT (read)");
		free(buf);
		free(small);
		return 1;
	}
	read_off = (off / (off_t)io_sz) * (off_t)io_sz;
	if (lseek(fd, read_off, SEEK_SET) < 0) {
		perror("lseek");
		close(fd);
		free(buf);
		free(small);
		return 1;
	}
	io = read(fd, buf, io_sz);
	if (io != (ssize_t)io_sz) {
		fprintf(stderr, "direct read failed: %zd (%s)\n",
			io, strerror(errno));
		close(fd);
		free(buf);
		free(small);
		return 1;
	}
	close(fd);

	for (size_t i = 0; i < len; i++) {
		size_t idx = (size_t)(off - read_off + (off_t)i);
		if (((unsigned char *)buf)[idx] != 'B') {
			bad = 1;
			break;
		}
	}

	if (bad) {
		fprintf(stderr,
			"FAIL: data mismatch at offset %ld (expected 'B')\n",
			(long)off);
	} else {
		printf("OK: data preserved at offset %ld\n", (long)off);
	}

	free(buf);
	free(small);
	return bad ? 2 : 0;
}
