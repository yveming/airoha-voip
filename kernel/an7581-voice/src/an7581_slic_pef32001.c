// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lantiq PEF32001 backend placeholder for an7581-slic.
 *
 * PEF32001 uses CSI / Lantiq DXS rather than ZSI.  The backend slot is
 * kept in the common SLIC table, but the transport and init sequence are
 * not implemented yet.
 */

#include "an7581_slic.h"

int an7581_slic_pef32001_detect(struct an7581_slic *slic)
{
	return -EOPNOTSUPP;
}
