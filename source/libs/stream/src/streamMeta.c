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

#include "executor.h"
#include "streamBackendRocksdb.h"
#include "streamInt.h"
#include "tref.h"
#include "ttimer.h"

static TdThreadOnce streamMetaModuleInit = PTHREAD_ONCE_INIT;
int32_t             streamBackendId = 0;
int32_t             streamBackendCfWrapperId = 0;

int64_t streamGetLatestCheckpointId(SStreamMeta* pMeta);

static void streamMetaEnvInit() {
  streamBackendId = taosOpenRef(64, streamBackendCleanup);
  streamBackendCfWrapperId = taosOpenRef(64, streamBackendHandleCleanup);
}

void streamMetaInit() { taosThreadOnce(&streamMetaModuleInit, streamMetaEnvInit); }
void streamMetaCleanup() {
  taosCloseRef(streamBackendId);
  taosCloseRef(streamBackendCfWrapperId);
}

// int32_t streamStateRebuild(SStreamMeta* pMeta, char* path, int64_t chkpId) {
//   int32_t code = 0;

//   int32_t nTask = taosHashGetSize(pMeta->pTasks);
//   assert(nTask == 0);

//   return code;
// }
SStreamMeta* streamMetaOpen(const char* path, void* ahandle, FTaskExpand expandFunc, int32_t vgId) {
  int32_t      code = -1;
  SStreamMeta* pMeta = taosMemoryCalloc(1, sizeof(SStreamMeta));
  if (pMeta == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }

  char* tpath = taosMemoryCalloc(1, strlen(path) + 64);
  sprintf(tpath, "%s%s%s", path, TD_DIRSEP, "stream");
  pMeta->path = tpath;

  if (tdbOpen(pMeta->path, 16 * 1024, 1, &pMeta->db, 0) < 0) {
    goto _err;
  }
  if (tdbTbOpen("task.db", sizeof(int32_t), -1, NULL, pMeta->db, &pMeta->pTaskDb, 0) < 0) {
    goto _err;
  }

  if (tdbTbOpen("checkpoint.db", sizeof(int32_t), -1, NULL, pMeta->db, &pMeta->pCheckpointDb, 0) < 0) {
    goto _err;
  }

  _hash_fn_t fp = taosGetDefaultHashFunction(TSDB_DATA_TYPE_INT);
  pMeta->pTasks = taosHashInit(64, fp, true, HASH_NO_LOCK);
  if (pMeta->pTasks == NULL) {
    goto _err;
  }

  // task list
  pMeta->pTaskList = taosArrayInit(4, sizeof(int32_t));
  if (pMeta->pTaskList == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }

  if (streamMetaBegin(pMeta) < 0) {
    goto _err;
  }

  pMeta->walScanCounter = 0;
  pMeta->vgId = vgId;
  pMeta->ahandle = ahandle;
  pMeta->expandFunc = expandFunc;

  pMeta->pTaskBackendUnique =
      taosHashInit(64, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY), false, HASH_ENTRY_LOCK);
  pMeta->chkpSaved = taosArrayInit(4, sizeof(int64_t));
  pMeta->chkpInUse = taosArrayInit(4, sizeof(int64_t));
  pMeta->chkpCap = 8;
  taosInitRWLatch(&pMeta->chkpDirLock);

  int64_t chkpId = streamGetLatestCheckpointId(pMeta);
  pMeta->chkpId = chkpId;

  pMeta->streamBackend = streamBackendInit(pMeta->path, chkpId);
  if (pMeta->streamBackend == NULL) {
    goto _err;
  }
  pMeta->streamBackendRid = taosAddRef(streamBackendId, pMeta->streamBackend);

  code = streamBackendLoadCheckpointInfo(pMeta);
  if (code != 0) {
    terrno = TAOS_SYSTEM_ERROR(code);
    goto _err;
  }
  taosInitRWLatch(&pMeta->lock);
  taosThreadMutexInit(&pMeta->backendMutex, NULL);

  return pMeta;

_err:
  taosMemoryFree(pMeta->path);
  if (pMeta->pTasks) taosHashCleanup(pMeta->pTasks);
  if (pMeta->pTaskList) taosArrayDestroy(pMeta->pTaskList);
  if (pMeta->pTaskDb) tdbTbClose(pMeta->pTaskDb);
  if (pMeta->pCheckpointDb) tdbTbClose(pMeta->pCheckpointDb);
  if (pMeta->db) tdbClose(pMeta->db);
  taosMemoryFree(pMeta);

  qError("failed to open stream meta");
  return NULL;
}

int32_t streamMetaReopen(SStreamMeta* pMeta, int64_t chkpId) {
  // stop all running tasking and reopen later
  void* pIter = NULL;
  while (1) {
    pIter = taosHashIterate(pMeta->pTasks, pIter);
    if (pIter == NULL) {
      break;
    }

    SStreamTask* pTask = *(SStreamTask**)pIter;
    if (pTask->schedTimer) {
      taosTmrStop(pTask->schedTimer);
      pTask->schedTimer = NULL;
    }

    if (pTask->launchTaskTimer) {
      taosTmrStop(pTask->launchTaskTimer);
      pTask->launchTaskTimer = NULL;
    }

    tFreeStreamTask(pTask);
  }

  // close stream backend
  streamBackendCleanup(pMeta->streamBackend);
  taosRemoveRef(streamBackendId, pMeta->streamBackendRid);
  pMeta->streamBackendRid = -1;
  pMeta->streamBackend = NULL;

  char* defaultPath = taosMemoryCalloc(1, strlen(pMeta->path) + 64);
  sprintf(defaultPath, "%s%s%s", pMeta->path, TD_DIRSEP, "state");
  taosRemoveDir(defaultPath);

  char* newPath = taosMemoryCalloc(1, strlen(pMeta->path) + 64);
  sprintf(newPath, "%s%s%s", pMeta->path, TD_DIRSEP, "received");

  if (taosRenameFile(newPath, defaultPath) < 0) {
    taosMemoryFree(defaultPath);
    taosMemoryFree(newPath);
    return -1;
  }

  pMeta->streamBackend = streamBackendInit(pMeta->path, 0);
  if (pMeta->streamBackend == NULL) {
    return -1;
  }
  pMeta->streamBackendRid = taosAddRef(streamBackendId, pMeta->streamBackend);

  taosHashClear(pMeta->pTasks);

  taosArrayClear(pMeta->pTaskList);

  taosHashClear(pMeta->pTaskBackendUnique);

  taosArrayClear(pMeta->chkpSaved);

  taosArrayClear(pMeta->chkpInUse);

  return 0;
}
void streamMetaClose(SStreamMeta* pMeta) {
  tdbAbort(pMeta->db, pMeta->txn);
  tdbTbClose(pMeta->pTaskDb);
  tdbTbClose(pMeta->pCheckpointDb);
  tdbClose(pMeta->db);

  void* pIter = NULL;
  while (1) {
    pIter = taosHashIterate(pMeta->pTasks, pIter);
    if (pIter == NULL) {
      break;
    }

    SStreamTask* pTask = *(SStreamTask**)pIter;
    if (pTask->schedTimer) {
      taosTmrStop(pTask->schedTimer);
      pTask->schedTimer = NULL;
    }

    if (pTask->launchTaskTimer) {
      taosTmrStop(pTask->launchTaskTimer);
      pTask->launchTaskTimer = NULL;
    }

    tFreeStreamTask(pTask);
  }

  taosHashCleanup(pMeta->pTasks);
  taosRemoveRef(streamBackendId, pMeta->streamBackendRid);
  pMeta->pTaskList = taosArrayDestroy(pMeta->pTaskList);
  taosMemoryFree(pMeta->path);
  taosThreadMutexDestroy(&pMeta->backendMutex);
  taosHashCleanup(pMeta->pTaskBackendUnique);

  taosArrayDestroy(pMeta->chkpSaved);
  taosArrayDestroy(pMeta->chkpInUse);

  taosMemoryFree(pMeta);
}

#if 0
int32_t streamMetaAddSerializedTask(SStreamMeta* pMeta, int64_t ver, char* msg, int32_t msgLen) {
  SStreamTask* pTask = taosMemoryCalloc(1, sizeof(SStreamTask));
  if (pTask == NULL) {
    return -1;
  }
  SDecoder decoder;
  tDecoderInit(&decoder, (uint8_t*)msg, msgLen);
  if (tDecodeStreamTask(&decoder, pTask) < 0) {
    tDecoderClear(&decoder);
    goto FAIL;
  }
  tDecoderClear(&decoder);

  if (pMeta->expandFunc(pMeta->ahandle, pTask, ver) < 0) {
    ASSERT(0);
    goto FAIL;
  }

  if (taosHashPut(pMeta->pTasks, &pTask->id.taskId, sizeof(int32_t), &pTask, sizeof(void*)) < 0) {
    goto FAIL;
  }

  if (tdbTbUpsert(pMeta->pTaskDb, &pTask->id.taskId, sizeof(int32_t), msg, msgLen, pMeta->txn) < 0) {
    taosHashRemove(pMeta->pTasks, &pTask->id.taskId, sizeof(int32_t));
    ASSERT(0);
    goto FAIL;
  }

  return 0;

FAIL:
  if (pTask) tFreeStreamTask(pTask);
  return -1;
}
#endif

int32_t streamMetaSaveTask(SStreamMeta* pMeta, SStreamTask* pTask) {
  void*   buf = NULL;
  int32_t len;
  int32_t code;
  tEncodeSize(tEncodeStreamTask, pTask, len, code);
  if (code < 0) {
    return -1;
  }
  buf = taosMemoryCalloc(1, len);
  if (buf == NULL) {
    return -1;
  }

  SEncoder encoder = {0};
  tEncoderInit(&encoder, buf, len);
  tEncodeStreamTask(&encoder, pTask);
  tEncoderClear(&encoder);

  if (tdbTbUpsert(pMeta->pTaskDb, &pTask->id.taskId, sizeof(int32_t), buf, len, pMeta->txn) < 0) {
    return -1;
  }

  taosMemoryFree(buf);
  return 0;
}

// add to the ready tasks hash map, not the restored tasks hash map
int32_t streamMetaAddDeployedTask(SStreamMeta* pMeta, int64_t ver, SStreamTask* pTask) {
  int64_t checkpointId = 0;

  void* p = taosHashGet(pMeta->pTasks, &pTask->id.taskId, sizeof(pTask->id.taskId));
  if (p == NULL) {
    if (pMeta->expandFunc(pMeta->ahandle, pTask, ver) < 0) {
      tFreeStreamTask(pTask);
      return -1;
    }

    if (streamMetaSaveTask(pMeta, pTask) < 0) {
      tFreeStreamTask(pTask);
      return -1;
    }

    if (streamMetaCommit(pMeta) < 0) {
      tFreeStreamTask(pTask);
      return -1;
    }
    taosArrayPush(pMeta->pTaskList, &pTask->id.taskId);
  } else {
    return 0;
  }

  taosHashPut(pMeta->pTasks, &pTask->id.taskId, sizeof(pTask->id.taskId), &pTask, POINTER_BYTES);
  return 0;
}

int32_t streamMetaGetNumOfTasks(const SStreamMeta* pMeta) {
  size_t size = taosHashGetSize(pMeta->pTasks);
  ASSERT(taosArrayGetSize(pMeta->pTaskList) == taosHashGetSize(pMeta->pTasks));

  return (int32_t)size;
}

SStreamTask* streamMetaAcquireTask(SStreamMeta* pMeta, int32_t taskId) {
  taosRLockLatch(&pMeta->lock);

  SStreamTask** ppTask = (SStreamTask**)taosHashGet(pMeta->pTasks, &taskId, sizeof(int32_t));
  if (ppTask != NULL) {
    if (!streamTaskShouldStop(&(*ppTask)->status)) {
      int32_t ref = atomic_add_fetch_32(&(*ppTask)->refCnt, 1);
      taosRUnLockLatch(&pMeta->lock);
      qTrace("s-task:%s acquire task, ref:%d", (*ppTask)->id.idStr, ref);
      return *ppTask;
    }
  }

  taosRUnLockLatch(&pMeta->lock);
  return NULL;
}

void streamMetaReleaseTask(SStreamMeta* pMeta, SStreamTask* pTask) {
  int32_t ref = atomic_sub_fetch_32(&pTask->refCnt, 1);
  if (ref > 0) {
    qTrace("s-task:%s release task, ref:%d", pTask->id.idStr, ref);
  } else if (ref == 0) {
    ASSERT(streamTaskShouldStop(&pTask->status));
    tFreeStreamTask(pTask);
  } else if (ref < 0) {
    qError("task ref is invalid, ref:%d, %s", ref, pTask->id.idStr);
  }
}

static void doRemoveIdFromList(SStreamMeta* pMeta, int32_t num, int32_t taskId) {
  for (int32_t i = 0; i < num; ++i) {
    int32_t* pTaskId = taosArrayGet(pMeta->pTaskList, i);
    if (*pTaskId == taskId) {
      taosArrayRemove(pMeta->pTaskList, i);
      break;
    }
  }
}

void streamMetaRemoveTask(SStreamMeta* pMeta, int32_t taskId) {
  SStreamTask* pTask = NULL;

  // pre-delete operation
  taosWLockLatch(&pMeta->lock);
  SStreamTask** ppTask = (SStreamTask**)taosHashGet(pMeta->pTasks, &taskId, sizeof(int32_t));
  if (ppTask) {
    pTask = *ppTask;
    atomic_store_8(&pTask->status.taskStatus, TASK_STATUS__DROPPING);
  } else {
    qDebug("vgId:%d failed to find the task:0x%x, it may be dropped already", pMeta->vgId, taskId);
    taosWUnLockLatch(&pMeta->lock);
    return;
  }
  taosWUnLockLatch(&pMeta->lock);

  qDebug("s-task:0x%x set task status:%s", taskId, streamGetTaskStatusStr(TASK_STATUS__DROPPING));

  while (1) {
    taosRLockLatch(&pMeta->lock);
    ppTask = (SStreamTask**)taosHashGet(pMeta->pTasks, &taskId, sizeof(int32_t));

    if (ppTask) {
      if ((*ppTask)->status.timerActive == 0) {
        taosRUnLockLatch(&pMeta->lock);
        break;
      }

      taosMsleep(10);
      qDebug("s-task:%s wait for quit from timer", (*ppTask)->id.idStr);
      taosRUnLockLatch(&pMeta->lock);
    } else {
      taosRUnLockLatch(&pMeta->lock);
      break;
    }
  }

  // let's do delete of stream task
  taosWLockLatch(&pMeta->lock);
  ppTask = (SStreamTask**)taosHashGet(pMeta->pTasks, &taskId, sizeof(int32_t));
  if (ppTask) {
    taosHashRemove(pMeta->pTasks, &taskId, sizeof(int32_t));
    tdbTbDelete(pMeta->pTaskDb, &taskId, sizeof(int32_t), pMeta->txn);

    atomic_store_8(&pTask->status.taskStatus, TASK_STATUS__DROPPING);
    ASSERT(pTask->status.timerActive == 0);

    int32_t num = taosArrayGetSize(pMeta->pTaskList);
    qDebug("s-task:%s set the drop task flag, remain running s-task:%d", pTask->id.idStr, num - 1);
    doRemoveIdFromList(pMeta, num, pTask->id.taskId);

    // remove the ref by timer
    if (pTask->triggerParam != 0) {
      taosTmrStop(pTask->schedTimer);
      streamMetaReleaseTask(pMeta, pTask);
    }

    streamMetaReleaseTask(pMeta, pTask);
  } else {
    qDebug("vgId:%d failed to find the task:0x%x, it may have been dropped already", pMeta->vgId, taskId);
  }

  taosWUnLockLatch(&pMeta->lock);
}

int32_t streamMetaBegin(SStreamMeta* pMeta) {
  if (tdbBegin(pMeta->db, &pMeta->txn, tdbDefaultMalloc, tdbDefaultFree, NULL,
               TDB_TXN_WRITE | TDB_TXN_READ_UNCOMMITTED) < 0) {
    return -1;
  }
  return 0;
}

// todo add error log
int32_t streamMetaCommit(SStreamMeta* pMeta) {
  if (tdbCommit(pMeta->db, pMeta->txn) < 0) {
    qError("failed to commit stream meta");
    return -1;
  }

  if (tdbPostCommit(pMeta->db, pMeta->txn) < 0) {
    qError("failed to commit stream meta");
    return -1;
  }

  if (tdbBegin(pMeta->db, &pMeta->txn, tdbDefaultMalloc, tdbDefaultFree, NULL,
               TDB_TXN_WRITE | TDB_TXN_READ_UNCOMMITTED) < 0) {
    return -1;
  }

  return 0;
}

int32_t streamMetaAbort(SStreamMeta* pMeta) {
  if (tdbAbort(pMeta->db, pMeta->txn) < 0) {
    return -1;
  }

  if (tdbBegin(pMeta->db, &pMeta->txn, tdbDefaultMalloc, tdbDefaultFree, NULL,
               TDB_TXN_WRITE | TDB_TXN_READ_UNCOMMITTED) < 0) {
    return -1;
  }
  return 0;
}

int64_t streamGetLatestCheckpointId(SStreamMeta* pMeta) {
  int64_t chkpId = 0;

  TBC* pCur = NULL;
  if (tdbTbcOpen(pMeta->pTaskDb, &pCur, NULL) < 0) {
    return chkpId;
  }
  void*    pKey = NULL;
  int32_t  kLen = 0;
  void*    pVal = NULL;
  int32_t  vLen = 0;
  SDecoder decoder;

  tdbTbcMoveToFirst(pCur);
  while (tdbTbcNext(pCur, &pKey, &kLen, &pVal, &vLen) == 0) {
    SCheckpointInfo info;
    tDecoderInit(&decoder, (uint8_t*)pVal, vLen);
    if (tDecodeStreamTaskChkInfo(&decoder, &info) < 0) {
      continue;
    }
    tDecoderClear(&decoder);

    chkpId = TMAX(chkpId, info.checkpointId);
  }

_err:
  tdbFree(pKey);
  tdbFree(pVal);
  tdbTbcClose(pCur);

  return chkpId;
}
int32_t streamLoadTasks(SStreamMeta* pMeta, int64_t ver) {
  TBC* pCur = NULL;
  if (tdbTbcOpen(pMeta->pTaskDb, &pCur, NULL) < 0) {
    return -1;
  }

  void*    pKey = NULL;
  int32_t  kLen = 0;
  void*    pVal = NULL;
  int32_t  vLen = 0;
  SDecoder decoder;

  tdbTbcMoveToFirst(pCur);
  while (tdbTbcNext(pCur, &pKey, &kLen, &pVal, &vLen) == 0) {
    SStreamTask* pTask = taosMemoryCalloc(1, sizeof(SStreamTask));
    if (pTask == NULL) {
      tdbFree(pKey);
      tdbFree(pVal);
      tdbTbcClose(pCur);
      return -1;
    }

    tDecoderInit(&decoder, (uint8_t*)pVal, vLen);
    tDecodeStreamTask(&decoder, pTask);
    tDecoderClear(&decoder);

    // remove duplicate
    void* p = taosHashGet(pMeta->pTasks, &pTask->id.taskId, sizeof(pTask->id.taskId));
    if (p == NULL) {
      if (pMeta->expandFunc(pMeta->ahandle, pTask, pTask->chkInfo.checkpointVer) < 0) {
        tdbFree(pKey);
        tdbFree(pVal);
        tdbTbcClose(pCur);
        taosMemoryFree(pTask);
        return -1;
      }
      taosArrayPush(pMeta->pTaskList, &pTask->id.taskId);
    } else {
      tdbFree(pKey);
      tdbFree(pVal);
      tdbTbcClose(pCur);
      taosMemoryFree(pTask);
      continue;
    }

    if (taosHashPut(pMeta->pTasks, &pTask->id.taskId, sizeof(pTask->id.taskId), &pTask, sizeof(void*)) < 0) {
      tdbFree(pKey);
      tdbFree(pVal);
      tdbTbcClose(pCur);
      taosMemoryFree(pTask);
      return -1;
    }

    ASSERT(pTask->status.downstreamReady == 0);
  }

  tdbFree(pKey);
  tdbFree(pVal);
  if (tdbTbcClose(pCur) < 0) {
    return -1;
  }

  return 0;
}
