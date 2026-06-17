#ifndef DSA_SL_DUMP_H
#define DSA_SL_DUMP_H

#include "dsa.h"
#include <uapi/linux/dsa_sl.h>

void dump_gencap(void *addr);
void dump_wqcap(void *addr);
void dump_grpcap(void *addr);
void dump_tableoffsets(void *addr);
void dump_engcap(void *addr);
void dump_wqcfg(struct dsa_device_info *dsa, int idx);

void dsa_dumpbuf(struct dsa_memcpy_reqs *dsa);

#endif
