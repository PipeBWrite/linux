#ifndef DSA_EMU_LOCAL_H
#define DSA_EMU_LOCAL_H

#include <linux/types.h>
#include <linux/dsa_emu.h>

int dsa_emu_init_kthreads(uint32_t num_kthreads);
void dsa_emu_destroy_kthreads(void);

/* Poll interval in microseconds for kthread workers */
extern uint32_t dsa_emu_poll_usecs;

#endif
