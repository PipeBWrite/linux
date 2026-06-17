#ifndef DSA_EMU_FOLIO_ALLOC_POOL
#define DSA_EMU_FOLIO_ALLOC_POOL

#include <linux/types.h>

int init_folio_alloc_task(void);
void init_static_folio_pool(void);
int get_static_folio_bulk(int *folio_idx, int count, struct folio **arr);
extern uint32_t dsa_emu_static_folio_pool_size;
int dsa_emu_static_folio_pool_resize(unsigned int size);
void put_order_folio(struct folio *f, unsigned int order, int idx);

#endif
