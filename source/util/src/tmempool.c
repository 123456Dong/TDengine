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
#include "osMemPool.h"
#include "tmempoolInt.h"
#include "tlog.h"
#include "tutil.h"

static TdThreadOnce  gMPoolInit = PTHREAD_ONCE_INIT;
threadlocal void* threadPoolHandle = NULL;
threadlocal void* threadPoolSession = NULL;
SMemPoolMgmt gMPMgmt = {0};
SMPStrategyFp gMPFps[] = {
  {NULL}, 
  {NULL,        mpDirectAlloc, mpDirectFree, mpDirectGetMemSize, mpDirectRealloc, NULL,               NULL},
  {mpChunkInit, mpChunkAlloc,  mpChunkFree,  mpChunkGetMemSize,  mpChunkRealloc,  mpChunkInitSession, mpChunkUpdateCfg}
};


int32_t mpCheckCfg(SMemPoolCfg* cfg) {
  if (cfg->chunkSize < MEMPOOL_MIN_CHUNK_SIZE || cfg->chunkSize > MEMPOOL_MAX_CHUNK_SIZE) {
    uError("invalid memory pool chunkSize:%d", cfg->chunkSize);
    return TSDB_CODE_INVALID_MEM_POOL_PARAM;
  }

  if (cfg->evicPolicy <= 0 || cfg->evicPolicy >= E_EVICT_MAX_VALUE) {
    uError("invalid memory pool evicPolicy:%d", cfg->evicPolicy);
    return TSDB_CODE_INVALID_MEM_POOL_PARAM;
  }

  if (cfg->threadNum <= 0) {
    uError("invalid memory pool threadNum:%d", cfg->threadNum);
    return TSDB_CODE_INVALID_MEM_POOL_PARAM;
  }

  return TSDB_CODE_SUCCESS;
}


void mpFreeCacheGroup(SMPCacheGroup* pGrp) {
  if (NULL == pGrp) {
    return;
  }

  taosMemoryFree(pGrp->pNodes);
  taosMemoryFree(pGrp);
}


int32_t mpAddCacheGroup(SMemPool* pPool, SMPCacheGroupInfo* pInfo, SMPCacheGroup* pHead) {
  SMPCacheGroup* pGrp = NULL;
  if (NULL == pInfo->pGrpHead) {
    pInfo->pGrpHead = taosMemCalloc(1, sizeof(*pInfo->pGrpHead));
    if (NULL == pInfo->pGrpHead) {
      uError("malloc chunkCache failed");
      MP_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
    }

    pGrp = pInfo->pGrpHead;
  } else {
    pGrp = (SMPCacheGroup*)taosMemCalloc(1, sizeof(SMPCacheGroup));
    pGrp->pNext = pHead;
  }

  pGrp->nodesNum = pInfo->groupNum;
  pGrp->pNodes = taosMemCalloc(pGrp->nodesNum, pInfo->nodeSize);
  if (NULL == pGrp->pNodes) {
    uError("calloc %d %d nodes in cache group failed", pGrp->nodesNum, pInfo->nodeSize);
    MP_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
  }

  if (pHead && atomic_val_compare_exchange_ptr(&pInfo->pGrpHead, pHead, pGrp) != pHead) {
    mpFreeCacheGroup(pGrp);
    return TSDB_CODE_SUCCESS;
  }

  atomic_add_fetch_64(&pInfo->allocNum, pGrp->nodesNum);

  return TSDB_CODE_SUCCESS;
}

void mpDestroyCacheGroup(SMPCacheGroupInfo* pInfo) {
  SMPCacheGroup* pGrp = pInfo->pGrpHead;
  SMPCacheGroup* pNext = NULL;
  while (NULL != pGrp) {
    pNext = pGrp->pNext;

    mpFreeCacheGroup(pGrp);

    pGrp = pNext;
  }
}


int32_t mpPopIdleNode(SMemPool* pPool, SMPCacheGroupInfo* pInfo, void** ppRes) {
  SMPCacheGroup* pGrp = NULL;
  SMPListNode* pNode = NULL;
  
  while (true) {
    pNode = (SMPListNode*)atomic_load_ptr(&pInfo->pIdleList);
    if (NULL == pNode) {
      break;
    }

    if (atomic_val_compare_exchange_ptr(&pInfo->pIdleList, pNode, pNode->pNext) != pNode) {
      continue;
    }

    pNode->pNext = NULL;
    goto _return;
  }

  while (true) {
    pGrp = atomic_load_ptr(&pInfo->pGrpHead);
    int32_t offset = atomic_fetch_add_32(&pGrp->idleOffset, 1);
    if (offset < pGrp->nodesNum) {
      pNode = (SMPListNode*)((char*)pGrp->pNodes + offset * pInfo->nodeSize);
      break;
    } else {
      atomic_sub_fetch_32(&pGrp->idleOffset, 1);
    }
    
    MP_ERR_RET(mpAddCacheGroup(pPool, pInfo, pGrp));
  }

_return:

  *ppRes = pNode;

  return TSDB_CODE_SUCCESS;
}

void mpPushIdleNode(SMemPool* pPool, SMPCacheGroupInfo* pInfo, SMPListNode* pNode) {
  SMPCacheGroup* pGrp = NULL;
  SMPListNode* pOrig = NULL;
  
  while (true) {
    pOrig = (SMPListNode*)atomic_load_ptr(&pInfo->pIdleList);
    pNode->pNext = pOrig;
    
    if (atomic_val_compare_exchange_ptr(&pInfo->pIdleList, pOrig, pNode) != pOrig) {
      continue;
    }

    break;
  }
}


int32_t mpUpdateCfg(SMemPool* pPool) {
  atomic_store_64(&pPool->retireThreshold[0], pPool->cfg.maxSize * MP_RETIRE_LOW_THRESHOLD_PERCENT);
  atomic_store_64(&pPool->retireThreshold[1], pPool->cfg.maxSize * MP_RETIRE_MID_THRESHOLD_PERCENT);
  atomic_store_64(&pPool->retireThreshold[2], pPool->cfg.maxSize * MP_RETIRE_HIGH_THRESHOLD_PERCENT);
  
  atomic_store_64(&pPool->retireUnit, TMAX(pPool->cfg.maxSize * MP_RETIRE_UNIT_PERCENT, MP_RETIRE_UNIT_MIN_SIZE));

  if (gMPFps[gMPMgmt.strategy].updateCfgFp) {
    MP_ERR_RET((*gMPFps[gMPMgmt.strategy].updateCfgFp)(pPool));
  }

  return TSDB_CODE_SUCCESS;
}

int32_t mpInit(SMemPool* pPool, char* poolName, SMemPoolCfg* cfg) {
  MP_ERR_RET(mpCheckCfg(cfg));
  
  TAOS_MEMCPY(&pPool->cfg, cfg, sizeof(*cfg));
  
  pPool->name = taosStrdup(poolName);
  if (NULL == pPool->name) {
    uError("calloc memory pool name %s failed", poolName);
    MP_ERR_RET(terrno);
  }

  MP_ERR_RET(mpUpdateCfg(pPool));

  pPool->ctrlInfo.statFlags = MP_STAT_FLAG_LOG_ALL;
  pPool->ctrlInfo.funcFlags = MP_CTRL_FLAG_PRINT_STAT;

  pPool->sessionCache.groupNum = MP_SESSION_CACHE_ALLOC_BATCH_SIZE;
  pPool->sessionCache.nodeSize = sizeof(SMPSession);

  MP_ERR_RET(mpAddCacheGroup(pPool, &pPool->sessionCache, NULL));

  if (gMPFps[gMPMgmt.strategy].initFp) {
    MP_ERR_RET((*gMPFps[gMPMgmt.strategy].initFp)(pPool, poolName, cfg));
  }
  
  return TSDB_CODE_SUCCESS;
}

FORCE_INLINE void mpUpdateMaxAllocSize(int64_t* pMaxAllocMemSize, int64_t newSize) {
  int64_t maxAllocMemSize = atomic_load_64(pMaxAllocMemSize);
  while (true) {
    if (newSize <= maxAllocMemSize) {
      break;
    }
    
    if (maxAllocMemSize == atomic_val_compare_exchange_64(pMaxAllocMemSize, maxAllocMemSize, newSize)) {
      break;
    }

    maxAllocMemSize = atomic_load_64(pMaxAllocMemSize);
  }
}

void mpUpdateAllocSize(SMemPool* pPool, SMPSession* pSession, int64_t size) {
  int64_t allocMemSize = atomic_add_fetch_64(&pSession->allocMemSize, size);
  mpUpdateMaxAllocSize(&pSession->maxAllocMemSize, allocMemSize);

  allocMemSize = atomic_load_64(&pSession->pJob->job.allocMemSize);
  mpUpdateMaxAllocSize(&pSession->pJob->job.maxAllocMemSize, allocMemSize);

  allocMemSize = atomic_load_64(&pPool->allocMemSize);
  mpUpdateMaxAllocSize(&pPool->maxAllocMemSize, allocMemSize);
}

int32_t mpPutRetireMsgToQueue(SMemPool* pPool, bool retireLowLevel) {
  if (retireLowLevel) {
    if (0 == atomic_val_compare_exchange_8(&gMPMgmt.msgQueue.lowLevelRetire, 0, 1)) {
      atomic_store_ptr(&gMPMgmt.msgQueue.pPool, pPool);
      MP_ERR_RET(tsem2_post(&gMPMgmt.threadSem));
    }
    
    return TSDB_CODE_SUCCESS;
  }

  if (0 == atomic_val_compare_exchange_8(&gMPMgmt.msgQueue.midLevelRetire, 0, 1)) {
    atomic_store_ptr(&gMPMgmt.msgQueue.pPool, pPool);
    MP_ERR_RET(tsem2_post(&gMPMgmt.threadSem));
  }
  
  return TSDB_CODE_SUCCESS;
}


int32_t mpChkQuotaOverflow(SMemPool* pPool, SMPSession* pSession, int64_t size) {
  SMPJob* pJob = pSession->pJob;
  int32_t code = TSDB_CODE_SUCCESS;
  int64_t cAllocSize = atomic_add_fetch_64(&pJob->job.allocMemSize, size);
  int64_t quota = atomic_load_64(&pPool->cfg.jobQuota);
  if (quota > 0 && cAllocSize > quota) {
    code = TSDB_CODE_QRY_REACH_QMEM_THRESHOLD;
    uWarn("job 0x%" PRIx64 " allocSize %" PRId64 " is over than quota %" PRId64, pJob->job.jobId, cAllocSize, quota);
    pPool->cfg.cb.retireJobFp(&pJob->job, code);
    (void)atomic_sub_fetch_64(&pJob->job.allocMemSize, size);
    MP_RET(code);
  }

  int64_t pAllocSize = atomic_add_fetch_64(&pPool->allocMemSize, size);
  quota = atomic_load_64(&pPool->retireThreshold[2]);
  if (pAllocSize >= quota) {
    code = TSDB_CODE_QRY_QUERY_MEM_EXHAUSTED;
    uWarn("%s pool allocSize %" PRId64 " reaches the high quota %" PRId64, pPool->name, pAllocSize, quota);
    pPool->cfg.cb.retireJobFp(&pJob->job, code);
    (void)atomic_sub_fetch_64(&pJob->job.allocMemSize, size);
    (void)atomic_sub_fetch_64(&pPool->allocMemSize, size);
    MP_RET(code);
  }

  quota = atomic_load_64(&pPool->retireThreshold[1]);  
  if (pAllocSize >= quota) {
    uInfo("%s pool allocSize %" PRId64 " reaches the middle quota %" PRId64, pPool->name, pAllocSize, quota);
    if (cAllocSize >= atomic_load_64(&pPool->retireUnit) / 2) {
      code = TSDB_CODE_QRY_QUERY_MEM_EXHAUSTED;
      pPool->cfg.cb.retireJobFp(&pJob->job, code);
      (void)atomic_sub_fetch_64(&pJob->job.allocMemSize, size);
      (void)atomic_sub_fetch_64(&pPool->allocMemSize, size);

      MP_ERR_RET(mpPutRetireMsgToQueue(pPool, false));
      MP_RET(code);
    }
    
    return TSDB_CODE_SUCCESS;
  }

  quota = atomic_load_64(&pPool->retireThreshold[0]);    
  if (pAllocSize >= quota) {
    uInfo("%s pool allocSize %" PRId64 " reaches the low quota %" PRId64, pPool->name, pAllocSize, quota);
    if (cAllocSize >= atomic_load_64(&pPool->retireUnit)) {
      code = TSDB_CODE_QRY_QUERY_MEM_EXHAUSTED;
      pPool->cfg.cb.retireJobFp(&pJob->job, code);
      
      (void)atomic_sub_fetch_64(&pJob->job.allocMemSize, size);
      (void)atomic_sub_fetch_64(&pPool->allocMemSize, size);

      MP_ERR_RET(mpPutRetireMsgToQueue(pPool, true));
      MP_RET(code);
    }
  }

  return TSDB_CODE_SUCCESS;
}

int64_t mpGetMemorySizeImpl(SMemPool* pPool, SMPSession* pSession, void *ptr) {
  return (*gMPFps[gMPMgmt.strategy].getSizeFp)(pPool, pSession, ptr);
}

int32_t mpMalloc(SMemPool* pPool, SMPSession* pSession, int64_t size, uint32_t alignment, void** ppRes) {
  MP_RET((*gMPFps[gMPMgmt.strategy].allocFp)(pPool, pSession, size, alignment, ppRes));
}

int32_t mpCalloc(SMemPool* pPool, SMPSession* pSession, int64_t num, int64_t size, void** ppRes) {
  int32_t code = TSDB_CODE_SUCCESS;
  int64_t totalSize = num * size;
  void *res = NULL;

  MP_ERR_RET(mpMalloc(pPool, pSession, totalSize, 0, &res));

  if (NULL != res) {
    TAOS_MEMSET(res, 0, totalSize);
  }

_return:

  *ppRes = res;

  return code;
}


void mpFree(SMemPool* pPool, SMPSession* pSession, void *ptr, int64_t* origSize) {
  if (NULL == ptr) {
    if (origSize) {
      *origSize = 0;
    }
    
    return;
  }

  (*gMPFps[gMPMgmt.strategy].freeFp)(pPool, pSession, ptr, origSize);
}

int32_t mpRealloc(SMemPool* pPool, SMPSession* pSession, void **pPtr, int64_t size, int64_t* origSize) {
  int32_t code = TSDB_CODE_SUCCESS;

  if (NULL == *pPtr) {
    *origSize = 0;
    MP_RET(mpMalloc(pPool, pSession, size, 0, pPtr));
  }

  if (0 == size) {
    mpFree(pPool, pSession, *pPtr, origSize);
    return TSDB_CODE_SUCCESS;
  }

  *origSize = mpGetMemorySizeImpl(pPool, pSession, *pPtr);

  MP_RET((*gMPFps[gMPMgmt.strategy].reallocFp)(pPool, pSession, pPtr, size, origSize));
}

void mpPrintStatDetail(SMPCtrlInfo* pCtrl, SMPStatDetail* pDetail, char* detailName, int64_t maxAllocSize) {
  if (!MP_GET_FLAG(pCtrl->funcFlags, MP_CTRL_FLAG_PRINT_STAT)) {
    return;
  }

  uInfo("MemPool [%s] stat detail:", detailName);

  uInfo("Max Used Memory Size: %" PRId64, maxAllocSize);
  
  uInfo("[times]:");
  switch (gMPMgmt.strategy) {
    case E_MP_STRATEGY_DIRECT:
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("Malloc", pDetail->times.memMalloc));
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("Calloc", pDetail->times.memCalloc));
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("Realloc", pDetail->times.memRealloc));
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("Strdup", pDetail->times.strdup));
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("Free", pDetail->times.memFree));
      break;
    case E_MP_STRATEGY_CHUNK:
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("chunkMalloc", pDetail->times.chunkMalloc));
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("chunkRecycle", pDetail->times.chunkRecycle));
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("chunkReUse", pDetail->times.chunkReUse));
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("chunkFree", pDetail->times.chunkFree));
      break;
    default:
      break;
  }
  
  uInfo("[bytes]:");
  switch (gMPMgmt.strategy) {
    case E_MP_STRATEGY_DIRECT:  
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("Malloc", pDetail->bytes.memMalloc));
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("Calloc", pDetail->bytes.memCalloc));
      uInfo(MP_STAT_ORIG_FORMAT, MP_STAT_ORIG_VALUE("Realloc", pDetail->bytes.memRealloc));
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("Strdup", pDetail->bytes.strdup));
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("Free", pDetail->bytes.memFree));
      break;
  case E_MP_STRATEGY_CHUNK:
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("chunkMalloc", pDetail->bytes.chunkMalloc));
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("chunkRecycle", pDetail->bytes.chunkRecycle));
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("chunkReUse", pDetail->bytes.chunkReUse));
      uInfo(MP_STAT_FORMAT, MP_STAT_VALUE("chunkFree", pDetail->bytes.chunkFree));
      break;
    default:
      break;
  }
}

void mpPrintFileLineStat(SMPCtrlInfo* pCtrl, SHashObj* pHash, char* detailName) {
  //TODO
}

void mpPrintNodeStat(SMPCtrlInfo* pCtrl, SHashObj* pHash, char* detailName) {
  //TODO
}

void mpPrintSessionStat(SMPCtrlInfo* pCtrl, SMPStatSession* pSessStat, char* detailName) {
  if (!MP_GET_FLAG(pCtrl->funcFlags, MP_CTRL_FLAG_PRINT_STAT)) {
    return;
  }

  uInfo("MemPool [%s] session stat:", detailName);
  uInfo("init session succeed num: %" PRId64, pSessStat->initSucc);
  uInfo("init session failed num: %" PRId64, pSessStat->initFail);
  uInfo("session destroyed num: %" PRId64, pSessStat->destroyNum);
}

void mpPrintStat(SMemPool* pPool, SMPSession* pSession, char* procName) {
  char detailName[128];

  if (NULL != pSession) {
    snprintf(detailName, sizeof(detailName) - 1, "%s - %s", procName, "Session");
    detailName[sizeof(detailName) - 1] = 0;
    mpPrintStatDetail(&pSession->ctrlInfo, &pSession->stat.statDetail, detailName, pSession->maxAllocMemSize);

    snprintf(detailName, sizeof(detailName) - 1, "%s - %s", procName, "SessionFile");
    detailName[sizeof(detailName) - 1] = 0;
    mpPrintFileLineStat(&pSession->ctrlInfo, pSession->stat.fileStat, detailName);

    snprintf(detailName, sizeof(detailName) - 1, "%s - %s", procName, "SessionFileLine");
    detailName[sizeof(detailName) - 1] = 0;
    mpPrintFileLineStat(&pSession->ctrlInfo, pSession->stat.lineStat, detailName);
  }

  snprintf(detailName, sizeof(detailName) - 1, "%s - %s", procName, pPool->name);
  detailName[sizeof(detailName) - 1] = 0;
  mpPrintSessionStat(&pPool->ctrlInfo, &pPool->stat.statSession, detailName);
  mpPrintStatDetail(&pPool->ctrlInfo, &pPool->stat.statDetail, detailName, pPool->maxAllocMemSize);

  snprintf(detailName, sizeof(detailName) - 1, "%s - %s", procName, "MemPoolNode");
  detailName[sizeof(detailName) - 1] = 0;
  mpPrintNodeStat(&pSession->ctrlInfo, pSession->stat.nodeStat, detailName);
  
  snprintf(detailName, sizeof(detailName) - 1, "%s - %s", procName, "MemPoolFile");
  detailName[sizeof(detailName) - 1] = 0;
  mpPrintFileLineStat(&pSession->ctrlInfo, pSession->stat.fileStat, detailName);
  
  snprintf(detailName, sizeof(detailName) - 1, "%s - %s", procName, "MemPoolFileLine");
  detailName[sizeof(detailName) - 1] = 0;
  mpPrintFileLineStat(&pSession->ctrlInfo, pSession->stat.lineStat, detailName);
}

void mpLogStatDetail(SMPStatDetail* pDetail, EMPStatLogItem item, SMPStatInput* pInput) {
  switch (item) {
    case E_MP_STAT_LOG_MEM_MALLOC: {
      if (MP_GET_FLAG(pInput->procFlags, MP_STAT_PROC_FLAG_EXEC)) {
        atomic_add_fetch_64(&pDetail->times.memMalloc.exec, 1);
        atomic_add_fetch_64(&pDetail->bytes.memMalloc.exec, pInput->size);
      }
      if (MP_GET_FLAG(pInput->procFlags, MP_STAT_PROC_FLAG_RES_SUCC)) {
        atomic_add_fetch_64(&pDetail->times.memMalloc.succ, 1);
        atomic_add_fetch_64(&pDetail->bytes.memMalloc.succ, pInput->size);
      } 
      if (MP_GET_FLAG(pInput->procFlags, MP_STAT_PROC_FLAG_RES_FAIL)) {
        atomic_add_fetch_64(&pDetail->times.memMalloc.fail, 1);
        atomic_add_fetch_64(&pDetail->bytes.memMalloc.fail, pInput->size);
      } 
      break;
    }
    case E_MP_STAT_LOG_MEM_CALLOC:{
      if (MP_GET_FLAG(pInput->procFlags, MP_STAT_PROC_FLAG_EXEC)) {
        atomic_add_fetch_64(&pDetail->times.memCalloc.exec, 1);
        atomic_add_fetch_64(&pDetail->bytes.memCalloc.exec, pInput->size);
      }
      if (MP_GET_FLAG(pInput->procFlags, MP_STAT_PROC_FLAG_RES_SUCC)) {
        atomic_add_fetch_64(&pDetail->times.memCalloc.succ, 1);
        atomic_add_fetch_64(&pDetail->bytes.memCalloc.succ, pInput->size);
      } 
      if (MP_GET_FLAG(pInput->procFlags, MP_STAT_PROC_FLAG_RES_FAIL)) {
        atomic_add_fetch_64(&pDetail->times.memCalloc.fail, 1);
        atomic_add_fetch_64(&pDetail->bytes.memCalloc.fail, pInput->size);
      } 
      break;
    }
    case E_MP_STAT_LOG_MEM_REALLOC:{
      if (MP_GET_FLAG(pInput->procFlags, MP_STAT_PROC_FLAG_EXEC)) {
        atomic_add_fetch_64(&pDetail->times.memRealloc.exec, 1);
        atomic_add_fetch_64(&pDetail->bytes.memRealloc.exec, pInput->size);
        atomic_add_fetch_64(&pDetail->bytes.memRealloc.origExec, pInput->origSize);
      }
      if (MP_GET_FLAG(pInput->procFlags, MP_STAT_PROC_FLAG_RES_SUCC)) {
        atomic_add_fetch_64(&pDetail->times.memRealloc.succ, 1);
        atomic_add_fetch_64(&pDetail->bytes.memRealloc.succ, pInput->size);
        atomic_add_fetch_64(&pDetail->bytes.memRealloc.origSucc, pInput->origSize);
      } 
      if (MP_GET_FLAG(pInput->procFlags, MP_STAT_PROC_FLAG_RES_FAIL)) {
        atomic_add_fetch_64(&pDetail->times.memRealloc.fail, 1);
        atomic_add_fetch_64(&pDetail->bytes.memRealloc.fail, pInput->size);
        atomic_add_fetch_64(&pDetail->bytes.memRealloc.origFail, pInput->origSize);
      } 
      break;
    }
    case E_MP_STAT_LOG_MEM_FREE:{
      if (MP_GET_FLAG(pInput->procFlags, MP_STAT_PROC_FLAG_EXEC)) {
        atomic_add_fetch_64(&pDetail->times.memFree.exec, 1);
        atomic_add_fetch_64(&pDetail->bytes.memFree.exec, pInput->size);
      }
      if (MP_GET_FLAG(pInput->procFlags, MP_STAT_PROC_FLAG_RES_SUCC)) {
        atomic_add_fetch_64(&pDetail->times.memFree.succ, 1);
        atomic_add_fetch_64(&pDetail->bytes.memFree.succ, pInput->size);
      } 
      if (MP_GET_FLAG(pInput->procFlags, MP_STAT_PROC_FLAG_RES_FAIL)) {
        atomic_add_fetch_64(&pDetail->times.memFree.fail, 1);
        atomic_add_fetch_64(&pDetail->bytes.memFree.fail, pInput->size);
      } 
      break;
    }
    case E_MP_STAT_LOG_MEM_STRDUP: {
      if (MP_GET_FLAG(pInput->procFlags, MP_STAT_PROC_FLAG_EXEC)) {
        atomic_add_fetch_64(&pDetail->times.strdup.exec, 1);
        atomic_add_fetch_64(&pDetail->bytes.strdup.exec, pInput->size);
      }
      if (MP_GET_FLAG(pInput->procFlags, MP_STAT_PROC_FLAG_RES_SUCC)) {
        atomic_add_fetch_64(&pDetail->times.strdup.succ, 1);
        atomic_add_fetch_64(&pDetail->bytes.strdup.succ, pInput->size);
      } 
      if (MP_GET_FLAG(pInput->procFlags, MP_STAT_PROC_FLAG_RES_FAIL)) {
        atomic_add_fetch_64(&pDetail->times.strdup.fail, 1);
        atomic_add_fetch_64(&pDetail->bytes.strdup.fail, pInput->size);
      } 
      break;
    }
    case E_MP_STAT_LOG_CHUNK_MALLOC:  
    case E_MP_STAT_LOG_CHUNK_RECYCLE:  
    case E_MP_STAT_LOG_CHUNK_REUSE:
    case E_MP_STAT_LOG_CHUNK_FREE: {

    }
    default:
      uError("Invalid stat item: %d", item);
      break;
  }
}

void mpLogStat(SMemPool* pPool, SMPSession* pSession, EMPStatLogItem item, SMPStatInput* pInput) {
  switch (item) {
    case E_MP_STAT_LOG_MEM_MALLOC:
    case E_MP_STAT_LOG_MEM_CALLOC:
    case E_MP_STAT_LOG_MEM_REALLOC:
    case E_MP_STAT_LOG_MEM_FREE:
    case E_MP_STAT_LOG_MEM_STRDUP: {
      if (MP_GET_FLAG(pSession->ctrlInfo.statFlags, MP_STAT_FLAG_LOG_ALL_MEM_STAT)) {
        mpLogStatDetail(&pSession->stat.statDetail, item, pInput);
      }
      if (MP_GET_FLAG(pPool->ctrlInfo.statFlags, MP_STAT_FLAG_LOG_ALL_MEM_STAT)) {
        mpLogStatDetail(&pPool->stat.statDetail, item, pInput);
      }
      break;
    }
    case E_MP_STAT_LOG_CHUNK_MALLOC:  
    case E_MP_STAT_LOG_CHUNK_RECYCLE:  
    case E_MP_STAT_LOG_CHUNK_REUSE:
    case E_MP_STAT_LOG_CHUNK_FREE: {

    }
    default:
      uError("Invalid stat item: %d", item);
      break;
  }
}

void mpCheckUpateCfg(void) {
  taosRLockLatch(&gMPMgmt.poolLock);
  int32_t poolNum = taosArrayGetSize(gMPMgmt.poolList);
  for (int32_t i = 0; i < poolNum; ++i) {
    SMemPool* pPool = (SMemPool*)taosArrayGetP(gMPMgmt.poolList, i);
    if (pPool->cfg.cb.cfgUpdateFp) {
      (*pPool->cfg.cb.cfgUpdateFp)((void*)pPool, &pPool->cfg);
    }
  }
  taosRUnLockLatch(&gMPMgmt.poolLock);
}

void* mpMgmtThreadFunc(void* param) {
  int32_t timeout = 0;
  while (0 == atomic_load_8(&gMPMgmt.modExit)) {
    timeout = tsem2_timewait(&gMPMgmt.threadSem, gMPMgmt.waitMs);
    if (0 != timeout) {
      mpCheckUpateCfg();
      continue;
    }

    if (atomic_load_8(&gMPMgmt.msgQueue.midLevelRetire)) {
      (*gMPMgmt.msgQueue.pPool->cfg.cb.retireJobsFp)(atomic_load_64(&gMPMgmt.msgQueue.pPool->retireUnit), false, TSDB_CODE_QRY_QUERY_MEM_EXHAUSTED);
    } else if (atomic_load_8(&gMPMgmt.msgQueue.lowLevelRetire)) {
      (*gMPMgmt.msgQueue.pPool->cfg.cb.retireJobsFp)(atomic_load_64(&gMPMgmt.msgQueue.pPool->retireUnit), true, TSDB_CODE_QRY_QUERY_MEM_EXHAUSTED);
    }
    
    mpCheckUpateCfg();
  }
  
  return NULL;
}

void mpModInit(void) {
  int32_t code = TSDB_CODE_SUCCESS;
  
  taosInitRWLatch(&gMPMgmt.poolLock);
  
  gMPMgmt.poolList = taosArrayInit(10, POINTER_BYTES);
  if (NULL == gMPMgmt.poolList) {
    MP_ERR_JRET(terrno);
  }

  gMPMgmt.strategy = E_MP_STRATEGY_DIRECT;

  gMPMgmt.code = tsem2_init(&gMPMgmt.threadSem, 0, 0);
  if (TSDB_CODE_SUCCESS != gMPMgmt.code) {
    uError("failed to init sem2, error: 0x%x", gMPMgmt.code);
    return;
  }

  gMPMgmt.waitMs = MP_DEFAULT_MEM_CHK_INTERVAL_MS;
  
  TdThreadAttr thAttr;
  MP_ERR_JRET(taosThreadAttrInit(&thAttr));
  MP_ERR_JRET(taosThreadAttrSetDetachState(&thAttr, PTHREAD_CREATE_JOINABLE));
  code = taosThreadCreate(&gMPMgmt.poolMgmtThread, &thAttr, mpMgmtThreadFunc, NULL);
  if (code != 0) {
    uError("failed to create memPool mgmt thread, error: 0x%x", code);
    (void)taosThreadAttrDestroy(&thAttr);
    MP_ERR_JRET(code);
  }

  MP_ERR_JRET(taosThreadAttrDestroy(&thAttr));

_return:

  gMPMgmt.code = code;
}

int32_t taosMemPoolOpen(char* poolName, SMemPoolCfg* cfg, void** poolHandle) {
  int32_t code = TSDB_CODE_SUCCESS;
  SMemPool* pPool = NULL;
  
  MP_ERR_JRET(taosThreadOnce(&gMPoolInit, mpModInit));
  if (TSDB_CODE_SUCCESS != gMPMgmt.code) {
    uError("init memory pool failed, code: 0x%x", gMPMgmt.code);
    MP_ERR_JRET(gMPMgmt.code);
  }

  pPool = (SMemPool*)taosMemoryCalloc(1, sizeof(SMemPool));
  if (NULL == pPool) {
    uError("calloc memory pool failed, code: 0x%x", terrno);
    MP_ERR_JRET(terrno);
  }

  MP_ERR_JRET(mpInit(pPool, poolName, cfg));

  taosWLockLatch(&gMPMgmt.poolLock);
  
  if (NULL == taosArrayPush(gMPMgmt.poolList, &pPool)) {
    taosWUnLockLatch(&gMPMgmt.poolLock);
    MP_ERR_JRET(terrno);
  }
  
  pPool->slotId = taosArrayGetSize(gMPMgmt.poolList) - 1;
  
  taosWUnLockLatch(&gMPMgmt.poolLock);

_return:

  if (TSDB_CODE_SUCCESS != code) {
    taosMemPoolClose(pPool);
    pPool = NULL;
  }

  *poolHandle = pPool;

  return code;
}

void taosMemPoolCfgUpdate(void* poolHandle, SMemPoolCfg* pCfg) {
  SMemPool* pPool = (SMemPool*)poolHandle;

  (void)mpUpdateCfg(pPool);
}

void taosMemPoolDestroySession(void* poolHandle, void* session) {
  SMemPool* pPool = (SMemPool*)poolHandle;
  SMPSession* pSession = (SMPSession*)session;
  if (NULL == pSession) {
    uWarn("null pointer of session");
    return;
  }

  (void)atomic_sub_fetch_32(&pSession->pJob->remainSession, 1);
  
  //TODO;

  (void)atomic_add_fetch_64(&pPool->stat.statSession.destroyNum, 1);

  mpPrintStat(pPool, pSession, "DestroySession");

  TAOS_MEMSET(pSession, 0, sizeof(*pSession));

  mpPushIdleNode(pPool, &pPool->sessionCache, (SMPListNode*)pSession);
}

int32_t taosMemPoolInitSession(void* poolHandle, void** ppSession, void* pJob) {
  int32_t code = TSDB_CODE_SUCCESS;
  SMemPool* pPool = (SMemPool*)poolHandle;
  SMPSession* pSession = NULL;

  MP_ERR_JRET(mpPopIdleNode(pPool, &pPool->sessionCache, (void**)&pSession));

  TAOS_MEMCPY(&pSession->ctrlInfo, &pPool->ctrlInfo, sizeof(pSession->ctrlInfo));

  if (gMPFps[gMPMgmt.strategy].initSessionFp) {
    MP_ERR_JRET((*gMPFps[gMPMgmt.strategy].initSessionFp)(pPool, pSession));
  }
  
  pSession->pJob = (SMPJob*)pJob;
  (void)atomic_add_fetch_32(&pSession->pJob->remainSession, 1);

_return:

  if (TSDB_CODE_SUCCESS != code) {
    taosMemPoolDestroySession(poolHandle, pSession);
    pSession = NULL;
    (void)atomic_add_fetch_64(&pPool->stat.statSession.initFail, 1);
  } else {
    (void)atomic_add_fetch_64(&pPool->stat.statSession.initSucc, 1);
  }

  *ppSession = pSession;

  return code;
}


void *taosMemPoolMalloc(void* poolHandle, void* session, int64_t size, char* fileName, int32_t lineNo) {
  int32_t code = TSDB_CODE_SUCCESS;
  void *res = NULL;
  
  if (NULL == poolHandle || NULL == session || NULL == fileName || size < 0) {
    uError("%s invalid input param, handle:%p, session:%p, fileName:%p, size:%" PRId64, __FUNCTION__, poolHandle, session, fileName, size);
    MP_ERR_JRET(TSDB_CODE_INVALID_MEM_POOL_PARAM);
  }

  SMemPool* pPool = (SMemPool*)poolHandle;
  SMPSession* pSession = (SMPSession*)session;
  SMPStatInput input = {.size = size, .file = fileName, .line = lineNo, .procFlags = MP_STAT_PROC_FLAG_EXEC};

  terrno = mpMalloc(pPool, pSession, size, 0, &res);

  MP_SET_FLAG(input.procFlags, (res ? MP_STAT_PROC_FLAG_RES_SUCC : MP_STAT_PROC_FLAG_RES_FAIL));
  mpLogStat(pPool, pSession, E_MP_STAT_LOG_MEM_MALLOC, &input);

_return:

  return res;
}

void   *taosMemPoolCalloc(void* poolHandle, void* session, int64_t num, int64_t size, char* fileName, int32_t lineNo) {
  int32_t code = TSDB_CODE_SUCCESS;
  void *res = NULL;
  
  if (NULL == poolHandle || NULL == session || NULL == fileName || num < 0 || size < 0) {
    uError("%s invalid input param, handle:%p, session:%p, fileName:%p, num:%" PRId64 ", size:%" PRId64, 
      __FUNCTION__, poolHandle, session, fileName, num, size);
    MP_ERR_JRET(TSDB_CODE_INVALID_MEM_POOL_PARAM);
  }

  SMemPool* pPool = (SMemPool*)poolHandle;
  SMPSession* pSession = (SMPSession*)session;
  int64_t totalSize = num * size;
  SMPStatInput input = {.size = totalSize, .file = fileName, .line = lineNo, .procFlags = MP_STAT_PROC_FLAG_EXEC};

  terrno = mpCalloc(pPool, pSession, num, size, &res);

  MP_SET_FLAG(input.procFlags, (res ? MP_STAT_PROC_FLAG_RES_SUCC : MP_STAT_PROC_FLAG_RES_FAIL));
  mpLogStat(pPool, pSession, E_MP_STAT_LOG_MEM_CALLOC, &input);

_return:

  return res;
}

void *taosMemPoolRealloc(void* poolHandle, void* session, void *ptr, int64_t size, char* fileName, int32_t lineNo) {
  int32_t code = TSDB_CODE_SUCCESS;
  
  if (NULL == poolHandle || NULL == session || NULL == fileName || size < 0) {
    uError("%s invalid input param, handle:%p, session:%p, fileName:%p, size:%" PRId64, 
      __FUNCTION__, poolHandle, session, fileName, size);
    MP_ERR_JRET(TSDB_CODE_INVALID_MEM_POOL_PARAM);
  }

  SMemPool* pPool = (SMemPool*)poolHandle;
  SMPSession* pSession = (SMPSession*)session;
  SMPStatInput input = {.size = size, .file = fileName, .line = lineNo, .procFlags = MP_STAT_PROC_FLAG_EXEC};

  terrno = mpRealloc(pPool, pSession, &ptr, size, &input.origSize);

  MP_SET_FLAG(input.procFlags, ((ptr || 0 == size) ? MP_STAT_PROC_FLAG_RES_SUCC : MP_STAT_PROC_FLAG_RES_FAIL));
  mpLogStat(pPool, pSession, E_MP_STAT_LOG_MEM_REALLOC, &input);

_return:

  return ptr;
}

char   *taosMemPoolStrdup(void* poolHandle, void* session, const char *ptr, char* fileName, int32_t lineNo) {
  int32_t code = TSDB_CODE_SUCCESS;
  void *res = NULL;
  
  if (NULL == poolHandle || NULL == session || NULL == fileName || NULL == ptr) {
    uError("%s invalid input param, handle:%p, session:%p, fileName:%p, ptr:%p", 
      __FUNCTION__, poolHandle, session, fileName, ptr);
    MP_ERR_JRET(TSDB_CODE_INVALID_MEM_POOL_PARAM);
  }

  SMemPool* pPool = (SMemPool*)poolHandle;
  SMPSession* pSession = (SMPSession*)session;
  int64_t size = strlen(ptr) + 1;
  SMPStatInput input = {.size = size, .file = fileName, .line = lineNo, .procFlags = MP_STAT_PROC_FLAG_EXEC};

  terrno = mpMalloc(pPool, pSession, size, 0, &res);
  if (NULL != res) {
    TAOS_STRCPY(res, ptr);
    *((char*)res + size - 1) = 0;
  }

  MP_SET_FLAG(input.procFlags, (res ? MP_STAT_PROC_FLAG_RES_SUCC : MP_STAT_PROC_FLAG_RES_FAIL));
  mpLogStat(pPool, pSession, E_MP_STAT_LOG_MEM_STRDUP, &input);

_return:

  return res;
}

void taosMemPoolFree(void* poolHandle, void* session, void *ptr, char* fileName, int32_t lineNo) {
  int32_t code = TSDB_CODE_SUCCESS;
  if (NULL == poolHandle || NULL == session || NULL == fileName) {
    uError("%s invalid input param, handle:%p, session:%p, fileName:%p", 
      __FUNCTION__, poolHandle, session, fileName);
    MP_ERR_JRET(TSDB_CODE_INVALID_MEM_POOL_PARAM);
  }

  SMemPool* pPool = (SMemPool*)poolHandle;
  SMPSession* pSession = (SMPSession*)session;
  SMPStatInput input = {.file = fileName, .line = lineNo, .procFlags = MP_STAT_PROC_FLAG_EXEC};

  mpFree(pPool, pSession, ptr, &input.size);

  MP_SET_FLAG(input.procFlags, MP_STAT_PROC_FLAG_RES_SUCC);
  mpLogStat(pPool, pSession, E_MP_STAT_LOG_MEM_FREE, &input);

_return:

  return;
}

int64_t taosMemPoolGetMemorySize(void* poolHandle, void* session, void *ptr, char* fileName, int32_t lineNo) {
  int32_t code = TSDB_CODE_SUCCESS;
  if (NULL == poolHandle || NULL == session || NULL == fileName) {
    uError("%s invalid input param, handle:%p, session:%p, fileName:%p", 
      __FUNCTION__, poolHandle, session, fileName);
    MP_ERR_JRET(TSDB_CODE_INVALID_MEM_POOL_PARAM);
  }

  if (NULL == ptr) {
    return 0;
  }

  SMemPool* pPool = (SMemPool*)poolHandle;
  SMPSession* pSession = (SMPSession*)session;
  return mpGetMemorySizeImpl(pPool, pSession, ptr);

_return:

  return code;
}

void* taosMemPoolMallocAlign(void* poolHandle, void* session, uint32_t alignment, int64_t size, char* fileName, int32_t lineNo) {
  int32_t code = TSDB_CODE_SUCCESS;
  void *res = NULL;
  
  if (NULL == poolHandle || NULL == session || NULL == fileName || size < 0 || alignment < POINTER_BYTES || alignment % POINTER_BYTES) {
    uError("%s invalid input param, handle:%p, session:%p, fileName:%p, alignment:%u, size:%" PRId64, 
      __FUNCTION__, poolHandle, session, fileName, alignment, size);
    MP_ERR_JRET(TSDB_CODE_INVALID_MEM_POOL_PARAM);
  }

  SMemPool* pPool = (SMemPool*)poolHandle;
  SMPSession* pSession = (SMPSession*)session;
  SMPStatInput input = {.size = size, .file = fileName, .line = lineNo, .procFlags = MP_STAT_PROC_FLAG_EXEC};

  terrno = mpMalloc(pPool, pSession, size, alignment, &res);

  MP_SET_FLAG(input.procFlags, (res ? MP_STAT_PROC_FLAG_RES_SUCC : MP_STAT_PROC_FLAG_RES_FAIL));
  mpLogStat(pPool, pSession, E_MP_STAT_LOG_MEM_MALLOC, &input);

_return:

  return res;
}

void taosMemPoolClose(void* poolHandle) {
  SMemPool* pPool = (SMemPool*)poolHandle;

  taosMemoryFree(pPool->name);
  mpDestroyCacheGroup(&pPool->sessionCache);
}

void taosMemPoolModDestroy(void) {

}


void taosMemPoolTrim(void* poolHandle, void* session, int32_t size, char* fileName, int32_t lineNo) {

}

int32_t taosMemPoolCallocJob(uint64_t jobId, void** ppJob) {
  *ppJob = taosMemoryCalloc(1, sizeof(SMPJob));
  if (NULL == *ppJob) {
    uError("calloc mp job failed, code: 0x%x", terrno);
    return terrno;
  }

  SMPJob* pJob = (SMPJob*)*ppJob;
  pJob->job.jobId = jobId;
  
  return TSDB_CODE_SUCCESS;
}


void taosAutoMemoryFree(void *ptr) {
  if (NULL != threadPoolHandle) {
    taosMemPoolFree(threadPoolHandle, threadPoolSession, ptr, __FILE__, __LINE__);
  } else {
    taosMemFree(ptr);
  }
}


