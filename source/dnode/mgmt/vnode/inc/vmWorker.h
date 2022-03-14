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

#ifndef _TD_DND_VNODE_WORKER_H_
#define _TD_DND_VNODE_WORKER_H_

#include "vmInt.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t vmStartWorker(SDnode *pDnode);
void    vmStopWorker(SDnode *pDnode);
void    vmInitMsgFp(SMnodeMgmt *pMgmt);
void    vmProcessRpcMsg(SDnode *pDnode, SRpcMsg *pMsg, SEpSet *pEpSet);
int32_t vmPutMsgToWriteQueue(SDnode *pDnode, SRpcMsg *pRpcMsg);
int32_t vmPutMsgToReadQueue(SDnode *pDnode, SRpcMsg *pRpcMsg);
void    vmConsumeChildQueue(SDnode *pDnode, SMndMsg *pMsg, int32_t msgLen, void *pCont, int32_t contLen);
void    vmConsumeParentQueue(SDnode *pDnode, SRpcMsg *pMsg, int32_t msgLen, void *pCont, int32_t contLen);

void vmProcessWriteMsg(SDnode *pDnode, SMgmtWrapper *pWrapper, SNodeMsg *pMsg);
void vmProcessSyncMsg(SDnode *pDnode, SMgmtWrapper *pWrapper, SNodeMsg *pMsg);
void vmProcessQueryMsg(SDnode *pDnode, SMgmtWrapper *pWrapper, SNodeMsg *pMsg);
void vmProcessFetchMsg(SDnode *pDnode, SMgmtWrapper *pWrapper, SNodeMsg *pMsg);

#ifdef __cplusplus
}
#endif

#endif /*_TD_DND_VNODE_WORKER_H_*/