#ifndef DSA_MM_H
#define DSA_MM_H

#include <linux/types.h>
#include <linux/mm.h>

// The following functions are defined in kernel but not exported. We export
// them in kernel and put definitions here.

extern void flush_tlb_mm_range(struct mm_struct *mm, unsigned long start,
			       unsigned long end, unsigned int stride_shift,
			       bool freed_tables);

extern void flush_tlb_mm_range_noinv(struct mm_struct *mm, unsigned long start,
				     unsigned long end,
				     unsigned int stride_shift,
				     bool freed_tables);

extern int do_mprotect_pkey(unsigned long start, size_t len, unsigned long prot,
			    int pkey, bool dsa);

unsigned long virt_2_phy(struct mm_struct *mm, unsigned long addr);

void dsa_set_page_prot(uint64_t addr, size_t len, bool set_user);

// Do something for each pte
int for_each_pte(uint64_t addr, size_t len, int (*fn)(pte_t *pte), bool flush,
		 bool flush_sec);

int is_all_addr_mapped(uint64_t addr, size_t len);

#endif
