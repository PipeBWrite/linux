#ifndef DSA_IRQ_H
#define DSA_IRQ_H

#include <linux/irq.h>

#include "dsa.h"

int dsa_setup_interrupt(struct dsa_device_info *dsa);
int dsa_release_interrupt(struct dsa_device_info *dsa);
int dsa_wq_setup_interrupt(struct dsa_device_info *dsa,
			   struct dsa_workqueue *wq);
int dsa_wq_free_interrupt(struct dsa_device_info *dsa,
			  struct dsa_workqueue *wq);

#endif
