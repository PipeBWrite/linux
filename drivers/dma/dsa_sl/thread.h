#ifndef DSA_THREAD_H
#define DSA_THREAD_H

#include "dsa.h"
#include <uapi/linux/dsa_sl.h>

#ifdef CONFIG_DSA_SL_ENABLE_USER_THREAD

int dsa_start_thread(struct dsa_device_info *dsa, struct dsa_memcpy_reqs *reqs);

int dsa_stop_thread(struct dsa_device_info *dsa);

#else

int dsa_start_thread(struct dsa_device_info *dsa, struct dsa_memcpy_reqs *reqs)
{
	return 0;
}

int dsa_stop_thread(struct dsa_device_info *dsa)
{
	return 0;
}

#endif

#endif
