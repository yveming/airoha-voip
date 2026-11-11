/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AN7581_ZSI_H
#define _AN7581_ZSI_H

#include <linux/device.h>
#include <linux/types.h>

struct an7581_zsi;

int an7581_zsi_write(struct an7581_zsi *zsi, const u8 *buf, size_t len);
int an7581_zsi_read_reg(struct an7581_zsi *zsi, u8 ec, u8 reg,
			u8 *buf, size_t len);
int an7581_zsi_write_reg(struct an7581_zsi *zsi, u8 ec, u8 reg,
			 const u8 *buf, size_t len);
struct an7581_zsi *an7581_zsi_get(struct device *dev, const char *phandle);
void an7581_zsi_put(struct an7581_zsi *zsi);

#endif
