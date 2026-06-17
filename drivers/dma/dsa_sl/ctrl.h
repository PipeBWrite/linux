#ifndef DSA_CTRL_H
#define DSA_CTRL_H

#include "dsa.h"

int dsa_device_enable(struct dsa_device_info *dsa, bool enable);
void dsa_write_wqcfg(union WQCFG *in, struct dsa_device_info *dsa, int idx);
void dsa_read_wqcfg(union WQCFG *out, struct dsa_device_info *dsa, int idx);
int dsa_wq_enable(struct dsa_device_info *dsa, int idx, bool enable);
void dsa_dump_swerr(struct dsa_device_info *dsa);
void dsa_configure_wqs(struct dsa_device_info *dsa);
void dsa_configure_wqs_user(struct dsa_device_info *dsa, unsigned int pasid);

#endif
