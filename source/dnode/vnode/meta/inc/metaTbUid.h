/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _TD_META_UID_H_
#define _TD_META_UID_H_

#include "meta.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------ APIS EXPOSED ------------------------ */
typedef struct STbUidGenerator {
  tb_uid_t nextUid;
} STbUidGenerator;

// tb_uid_t
#define IVLD_TB_UID 0
tb_uid_t generateUid(STbUidGenerator *);

// STableUidGenerator
void tableUidGeneratorInit(STbUidGenerator *, tb_uid_t suid);
#define tableUidGeneratorClear(ug)

#ifdef __cplusplus
}
#endif

#endif /*_TD_META_UID_H_*/