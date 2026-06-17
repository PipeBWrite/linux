#include "asm/page.h"
#include "asm/page_types.h"
#include "asm/pgtable.h"
#include "linux/pfn.h"
#include "linux/stddef.h"
#include <linux/types.h>
#include <linux/mm.h>
#include "mm.h"

static int set_pte_user(pte_t *pte)
{
	*pte = pte_set_flags(*pte, _PAGE_USER);
	return 0;
}

static int clear_pte_user(pte_t *pte)
{
	*pte = pte_clear_flags(*pte, _PAGE_USER);
	return 0;
}

void dsa_set_page_prot(uint64_t addr, size_t len, bool set_user)
{
	int ret;

	ret = for_each_pte(addr, len, set_user ? set_pte_user : clear_pte_user,
			   true, false);

	if (ret) {
		pr_err("Failed to set page prot\n");
	}

	return;
}

unsigned long virt_2_phy(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	struct page *page;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return 0;

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return 0;

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud))
		return 0;

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return 0;

	pte = pte_offset_map(pmd, addr);

	page = pte_page(*pte);

	if (page == NULL)
		return 0;

	return PFN_PHYS(page_to_pfn(page)) + (addr & ~PAGE_MASK);
}

int for_each_pte(uint64_t addr, size_t len, int (*fn)(pte_t *pte), bool flush,
		 bool flush_sec)
{
	unsigned long start = addr & PAGE_MASK;
	unsigned long end = (addr + len + PAGE_SIZE - 1) & PAGE_MASK;
	unsigned long i;
	int ret;

	pte_t *pte;
	pmd_t *pmd;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;

	for (i = start; i < end; i += PAGE_SIZE) {
		pgd = pgd_offset(current->mm, i);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			goto err;

		p4d = p4d_offset(pgd, i);
		if (p4d_none(*p4d) || p4d_bad(*p4d))
			goto err;

		pud = pud_offset(p4d, i);
		if (pud_none(*pud) || pud_bad(*pud))
			goto err;

		pmd = pmd_offset(pud, i);
		if (pmd_none(*pmd) || pmd_bad(*pmd))
			goto err;

		pte = pte_offset_map(pmd, i);
		ret = fn(pte);
		if (ret)
			goto err;

		pte_unmap(pte);
	}

	if (flush) {
		if (flush_sec)
			flush_tlb_mm_range(current->mm, start, end, PAGE_SHIFT,
					   false);
		else
			flush_tlb_mm_range_noinv(current->mm, start, end,
						 PAGE_SHIFT, false);
	}
	return 0;

err:
	pte_unmap(pte);
	return 1;
}

static int is_page_unmapped(pte_t *pte)
{
	if (pte_present(*pte)) {
		return 0;
	}

	return 1;
}

int is_all_addr_mapped(uint64_t addr, size_t len)
{
	int ret;
	ret = for_each_pte(addr, len, is_page_unmapped, false, false);

	return !ret;
}
