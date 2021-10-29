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

#define _DEFAULT_SOURCE
#include "os.h"
#include "vnodeMain.h"
#include "vnodeWrite.h"
#include "vnodeWriteMsg.h"

typedef int32_t (*WriteMsgFp)(SVnode *, void *pCont, SVnRsp *);

typedef struct {
  int32_t  code;
  int8_t   qtype;
  SVnode * pVnode;
  SRpcMsg  rpcMsg;
  SVnRsp  rspRet;
  char     reserveForSync[24];
  SWalHead walHead;
} SVnWriteMsg;

static struct {
  SMWorkerPool pool;
  int64_t      queuedBytes;
  int32_t      queuedMsgs;
} tsVwrite = {0};

void vnodeStartWrite(SVnode *pVnode) {}
void vnodeStopWrite(SVnode *pVnode) {
  while (pVnode->queuedWMsg > 0) {
    vTrace("vgId:%d, queued wmsg num:%d", pVnode->vgId, pVnode->queuedWMsg);
    taosMsleep(10);
  }
}

static int32_t vnodeWriteToWQueue(SVnode *pVnode, SWalHead *pHead, int32_t qtype, SRpcMsg *pRpcMsg) {
  if (!(pVnode->accessState & TSDB_VN_WRITE_ACCCESS)) {
    vWarn("vgId:%d, no write auth", pVnode->vgId);
    return TSDB_CODE_VND_NO_WRITE_AUTH;
  }

  if (tsAvailDataDirGB <= tsMinimalDataDirGB) {
    vWarn("vgId:%d, failed to write into vwqueue since no diskspace, avail:%fGB", pVnode->vgId, tsAvailDataDirGB);
    return TSDB_CODE_VND_NO_DISKSPACE;
  }

  if (pHead->len > TSDB_MAX_WAL_SIZE) {
    vError("vgId:%d, wal len:%d exceeds limit, hver:%" PRIu64, pVnode->vgId, pHead->len, pHead->version);
    return TSDB_CODE_WAL_SIZE_LIMIT;
  }

  if (tsVwrite.queuedBytes > tsMaxVnodeQueuedBytes) {
    vDebug("vgId:%d, too many bytes:%" PRId64 " in vwqueue, flow control", pVnode->vgId, tsVwrite.queuedBytes);
    return TSDB_CODE_VND_IS_FLOWCTRL;
  }

  int32_t      size = sizeof(SVnWriteMsg) + pHead->len;
  SVnWriteMsg *pWrite = taosAllocateQitem(size);
  if (pWrite == NULL) {
    return TSDB_CODE_VND_OUT_OF_MEMORY;
  }

  if (pRpcMsg != NULL) {
    pWrite->rpcMsg = *pRpcMsg;
  }

  memcpy(&pWrite->walHead, pHead, sizeof(SWalHead) + pHead->len);
  pWrite->pVnode = pVnode;
  pWrite->qtype = qtype;

  atomic_add_fetch_64(&tsVwrite.queuedBytes, size);
  atomic_add_fetch_32(&tsVwrite.queuedMsgs, 1);
  atomic_add_fetch_32(&pVnode->refCount, 1);
  atomic_add_fetch_32(&pVnode->queuedWMsg, 1);
  taosWriteQitem(pVnode->pWriteQ, pWrite);

  return TSDB_CODE_SUCCESS;
}

static void vnodeFreeFromWQueue(SVnode *pVnode, SVnWriteMsg *pWrite) {
  int64_t size = sizeof(SVnWriteMsg) + pWrite->walHead.len;
  atomic_sub_fetch_64(&tsVwrite.queuedBytes, size);
  atomic_sub_fetch_32(&tsVwrite.queuedMsgs, 1);
  atomic_sub_fetch_32(&pVnode->queuedWMsg, 1);

  taosFreeQitem(pWrite);
  vnodeRelease(pVnode);
}

void vnodeProcessWriteReq(SRpcMsg *pRpcMsg) {
  int32_t code;

  SMsgHead *pMsg = pRpcMsg->pCont;
  pMsg->vgId = htonl(pMsg->vgId);
  pMsg->contLen = htonl(pMsg->contLen);

  SVnode *pVnode = vnodeAcquire(pMsg->vgId);
  if (pVnode == NULL) {
    code = TSDB_CODE_VND_INVALID_VGROUP_ID;
  } else {
    SWalHead *pHead = (SWalHead *)((char *)pRpcMsg->pCont - sizeof(SWalHead));
    pHead->msgType = pRpcMsg->msgType;
    pHead->version = 0;
    pHead->len = pMsg->contLen;
    code = vnodeWriteToWQueue(pVnode, pHead, TAOS_QTYPE_RPC, pRpcMsg);
  }

  if (code != TSDB_CODE_SUCCESS) {
    SRpcMsg rpcRsp = {.handle = pRpcMsg->handle, .code = code};
    rpcSendResponse(&rpcRsp);
  }

  vnodeRelease(pVnode);
  rpcFreeCont(pRpcMsg->pCont);
}

static void vnodeProcessWrite(SVnWriteMsg *pWrite, SVnode *pVnode) {
}
  // SWalHead *pHead = &pWrite->walHead;
  // SVnRsp * pRet = &pWrite->rspRet;
  // int32_t   msgType = pHead->msgType;

  // vTrace("vgId:%d, msg:%s will be processed, hver:%" PRIu64, pVnode->vgId, taosMsg[pHead->msgType], pHead->version);

  // // write data locally
  // switch (msgType) {
  //   case TSDB_MSG_TYPE_SUBMIT:
  //     pRet->len = sizeof(SSubmitRsp);
  //     pRet->rsp = rpcMallocCont(pRet->len);
  //     pWrite->code = vnodeProcessSubmitReq(pVnode, (void*)pHead->cont, pRet->rsp);
  //     break;
  //   case TSDB_MSG_TYPE_MD_CREATE_TABLE:
  //     pWrite->code = vnodeProcessCreateTableReq(pVnode, (void*)pHead->cont, NULL);
  //     break;
  //   case TSDB_MSG_TYPE_MD_DROP_TABLE:
  //     pWrite->code = vnodeProcessDropTableReq(pVnode, (void*)pHead->cont, NULL);
  //     break;
  //   case TSDB_MSG_TYPE_MD_ALTER_TABLE:
  //     pWrite->code = vnodeProcessAlterTableReq(pVnode, (void*)pHead->cont, NULL);
  //     break;
  //   case TSDB_MSG_TYPE_MD_DROP_STABLE:
  //     pWrite->code = vnodeProcessDropStableReq(pVnode, (void*)pHead->cont, NULL);
  //     break;
  //   case TSDB_MSG_TYPE_UPDATE_TAG_VAL:
  //     pWrite->code = vnodeProcessUpdateTagValReq(pVnode, (void*)pHead->cont, NULL);
  //     break;
  //   //mq related
  //   case TSDB_MSG_TYPE_MQ_CONNECT:
  //     pWrite->code = vnodeProcessMqConnectReq(pVnode, (void*)pHead->cont, NULL);
  //     break;
  //   case TSDB_MSG_TYPE_MQ_DISCONNECT:
  //     pWrite->code = vnodeProcessMqDisconnectReq(pVnode, (void*)pHead->cont, NULL);
  //     break;
  //   case TSDB_MSG_TYPE_MQ_ACK:
  //     pWrite->code = vnodeProcessMqAckReq(pVnode, (void*)pHead->cont, NULL);
  //     break;
  //   case TSDB_MSG_TYPE_MQ_RESET:
  //     pWrite->code = vnodeProcessMqResetReq(pVnode, (void*)pHead->cont, NULL);
  //     break;
  //   //mq related end
  //   default:
  //     pWrite->code = TSDB_CODE_VND_MSG_NOT_PROCESSED;
  //     break;
  // }

  // if (pWrite->code < 0) return false;

  // // update fsync
  // return (pWrite->code == 0 && msgType != TSDB_MSG_TYPE_SUBMIT);


  // // walFsync(pVnode->wal, fsync); 


// static void vnodeProcessWriteEnd(SVnWriteMsg *pWrite, SVnode *pVnode) {
//   if (qtype == TAOS_QTYPE_RPC) {
//     SRpcMsg rpcRsp = {
//         .handle = pWrite->rpcMsg.handle,
//         .pCont = pWrite->rspRet.rsp,
//         .contLen = pWrite->rspRet.len,
//         .code = pWrite->code,
//     };
//     rpcSendResponse(&rpcRsp);
//   } else {
//     if (pWrite->rspRet.rsp) {
//       rpcFreeCont(pWrite->rspRet.rsp);
//     }
//   }
//   vnodeFreeFromWQueue(pVnode, pWrite);
// }

int32_t vnodeInitWrite() {
  SMWorkerPool *pPool = &tsVwrite.pool;
  pPool->name = "vnode-write";
  pPool->max = tsNumOfCores;
  if (tMWorkerInit(pPool) != 0) return -1;

  vInfo("vwrite is initialized, max worker %d", pPool->max);
  return TSDB_CODE_SUCCESS;
}

void vnodeCleanupWrite() {
  tMWorkerCleanup(&tsVwrite.pool);
  vInfo("vwrite is closed");
}

taos_queue vnodeAllocWriteQueue(SVnode *pVnode) {
  return tMWorkerAllocQueue(&tsVwrite.pool, pVnode, NULL);
}

void vnodeFreeWriteQueue(taos_queue pQueue) { tMWorkerFreeQueue(&tsVwrite.pool, pQueue); }

taos_queue vnodeAllocApplyQueue(SVnode *pVnode) {
  return tMWorkerAllocQueue(&tsVwrite.pool, pVnode, NULL);
}

void vnodeFreeApplyQueue(taos_queue pQueue) { tMWorkerFreeQueue(&tsVwrite.pool, pQueue); }
