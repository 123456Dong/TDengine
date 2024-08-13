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
#include "executorInt.h"
#include "filter.h"
#include "function.h"
#include "functionMgt.h"
#include "operator.h"
#include "querytask.h"
#include "storageapi.h"
#include "streamexecutorInt.h"
#include "tchecksum.h"
#include "tcommon.h"
#include "tcompare.h"
#include "tdatablock.h"
#include "tfill.h"
#include "ttime.h"

#define STREAM_TIME_SLICE_OP_STATE_NAME      "StreamTimeSliceHistoryState"
#define STREAM_TIME_SLICE_OP_CHECKPOINT_NAME "StreamTimeSliceOperator_Checkpoint"
#define HAS_NON_ROW_DATA(pRowData)           (pRowData->key == INT64_MIN)
#define HAS_ROW_DATA(pRowData)               (pRowData && pRowData->key != INT64_MIN)
#define IS_INVALID_WIN_KEY(ts)               ((ts) == INT64_MIN)
#define SET_WIN_KEY_INVALID(ts)              ((ts) = INT64_MIN)

typedef struct SSliceRowData {
  TSKEY           key;
  SResultCellData pRowVal[];
} SSliceRowData;

typedef struct SSlicePoint {
  SWinKey        key;
  SSliceRowData* pLeftRow;
  SSliceRowData* pRightRow;
  SRowBuffPos*   pResPos;
} SSlicePoint;

int32_t saveTimeSliceWinResult(SWinKey* pKey, SSHashObj* pUpdatedMap) {
  return tSimpleHashPut(pUpdatedMap, pKey, sizeof(SWinKey), NULL, 0);
}

void streamTimeSliceReleaseState(SOperatorInfo* pOperator) {
  int32_t                       code = TSDB_CODE_SUCCESS;
  int32_t                       lino = 0;
  SStreamTimeSliceOperatorInfo* pInfo = pOperator->info;
  int32_t                       winSize = taosArrayGetSize(pInfo->historyWins) * sizeof(SWinKey);
  int32_t                       resSize = winSize + sizeof(TSKEY);
  char*                         pBuff = taosMemoryCalloc(1, resSize);
  QUERY_CHECK_NULL(pBuff, code, lino, _end, terrno);

  memcpy(pBuff, pInfo->historyWins->pData, winSize);
  memcpy(pBuff + winSize, &pInfo->twAggSup.maxTs, sizeof(TSKEY));
  qDebug("===stream=== time slice operator relase state. save result count:%d",
         (int32_t)taosArrayGetSize(pInfo->historyWins));
  pInfo->streamAggSup.stateStore.streamStateSaveInfo(pInfo->streamAggSup.pState, STREAM_TIME_SLICE_OP_STATE_NAME,
                                                     strlen(STREAM_TIME_SLICE_OP_STATE_NAME), pBuff, resSize);
  pInfo->streamAggSup.stateStore.streamStateCommit(pInfo->streamAggSup.pState);
  taosMemoryFreeClear(pBuff);

  SOperatorInfo* downstream = pOperator->pDownstream[0];
  if (downstream->fpSet.releaseStreamStateFn) {
    downstream->fpSet.releaseStreamStateFn(downstream);
  }

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
}

void streamTimeSliceReloadState(SOperatorInfo* pOperator) {
  int32_t                       code = TSDB_CODE_SUCCESS;
  int32_t                       lino = 0;
  SStreamTimeSliceOperatorInfo* pInfo = pOperator->info;
  SStreamAggSupporter*          pAggSup = &pInfo->streamAggSup;
  SExecTaskInfo*                pTaskInfo = pOperator->pTaskInfo;
  resetWinRange(&pAggSup->winRange);

  int32_t size = 0;
  void*   pBuf = NULL;
  code = pAggSup->stateStore.streamStateGetInfo(pAggSup->pState, STREAM_TIME_SLICE_OP_STATE_NAME,
                                                strlen(STREAM_TIME_SLICE_OP_STATE_NAME), &pBuf, &size);
  QUERY_CHECK_CODE(code, lino, _end);

  int32_t num = (size - sizeof(TSKEY)) / sizeof(SWinKey);
  qDebug("===stream=== time slice operator reload state. get result count:%d", num);
  SWinKey* pKeyBuf = (SWinKey*)pBuf;
  ASSERT(size == num * sizeof(SWinKey) + sizeof(TSKEY));

  TSKEY ts = *(TSKEY*)((char*)pBuf + size - sizeof(TSKEY));
  pInfo->twAggSup.maxTs = TMAX(pInfo->twAggSup.maxTs, ts);
  pAggSup->stateStore.streamStateReloadInfo(pAggSup->pState, ts);

  if (!pInfo->pUpdatedMap && num > 0) {
    _hash_fn_t hashFn = taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY);
    pInfo->pUpdatedMap = tSimpleHashInit(64, hashFn);
    QUERY_CHECK_NULL(pInfo->pUpdatedMap, code, lino, _end, terrno);
  }

  for (int32_t i = 0; i < num; i++) {
    SWinKey* pKey = pKeyBuf + i;
    qDebug("===stream=== reload state. try process result %" PRId64 ", %" PRIu64 ", index:%d", pKey->ts, pKey->groupId,
           i);
    code = saveTimeSliceWinResult(pKey, pInfo->pUpdatedMap);
    QUERY_CHECK_CODE(code, lino, _end);
  }
  taosMemoryFree(pBuf);

  SOperatorInfo* downstream = pOperator->pDownstream[0];
  if (downstream->fpSet.reloadStreamStateFn) {
    downstream->fpSet.reloadStreamStateFn(downstream);
  }
  reloadAggSupFromDownStream(downstream, &pInfo->streamAggSup);

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s. task:%s", __func__, lino, tstrerror(code), GET_TASKID(pTaskInfo));
  }
}

static void resetFillWindow(SResultRowData* pRowData) {
  pRowData->key = INT64_MIN;
  pRowData->pRowVal = NULL;
}

static void resetPrevAndNextWindow(SStreamFillSupporter* pFillSup) {
  resetFillWindow(&pFillSup->cur);
  resetFillWindow(&pFillSup->prev);
  resetFillWindow(&pFillSup->next);
  resetFillWindow(&pFillSup->nextNext);
}

void destroyStreamTimeSliceOperatorInfo(void* param) {
  SStreamTimeSliceOperatorInfo* pInfo = (SStreamTimeSliceOperatorInfo*)param;
  colDataDestroy(&pInfo->twAggSup.timeWindowData);
  destroyStreamAggSupporter(&pInfo->streamAggSup);
  resetPrevAndNextWindow(pInfo->pFillSup);
  destroyStreamFillSupporter(pInfo->pFillSup);
  destroyStreamFillInfo(pInfo->pFillInfo);
  blockDataDestroy(pInfo->pRes);
  blockDataDestroy(pInfo->pDelRes);
  blockDataDestroy(pInfo->pCheckpointRes);

  taosMemoryFreeClear(pInfo->leftRow.pRowVal);
  taosMemoryFreeClear(pInfo->valueRow.pRowVal);
  taosMemoryFreeClear(pInfo->rightRow.pRowVal);

  cleanupExprSupp(&pInfo->scalarSup);
  taosArrayDestroy(pInfo->historyPoints);

  taosArrayDestroyP(pInfo->pUpdated, destroyFlusedPos);
  pInfo->pUpdated = NULL;

  tSimpleHashCleanup(pInfo->pUpdatedMap);
  pInfo->pUpdatedMap = NULL;

  taosArrayDestroy(pInfo->pDelWins);
  tSimpleHashCleanup(pInfo->pDeletedMap);
  clearGroupResInfo(&pInfo->groupResInfo);

  taosMemoryFreeClear(param);
}

int32_t doStreamTimeSliceEncodeOpState(void** buf, int32_t len, SOperatorInfo* pOperator, int32_t* pLen) {
  int32_t                       code = TSDB_CODE_SUCCESS;
  SStreamTimeSliceOperatorInfo* pInfo = pOperator->info;
  if (!pInfo) {
    return TSDB_CODE_FAILED;
  }

  void* pData = (buf == NULL) ? NULL : *buf;

  // 1.streamAggSup.pResultRows
  int32_t tlen = 0;
  int32_t mapSize = tSimpleHashGetSize(pInfo->streamAggSup.pResultRows);
  tlen += taosEncodeFixedI32(buf, mapSize);
  void*   pIte = NULL;
  size_t  keyLen = 0;
  int32_t iter = 0;
  while ((pIte = tSimpleHashIterate(pInfo->streamAggSup.pResultRows, pIte, &iter)) != NULL) {
    void* pKey = tSimpleHashGetKey(pIte, &keyLen);
    tlen += encodeSSessionKey(buf, pKey);
    tlen += encodeSResultWindowInfo(buf, pIte, pInfo->streamAggSup.resultRowSize);
  }

  // 2.twAggSup
  tlen += encodeSTimeWindowAggSupp(buf, &pInfo->twAggSup);

  // 3.checksum
  if (buf) {
    uint32_t cksum = taosCalcChecksum(0, pData, len - sizeof(uint32_t));
    tlen += taosEncodeFixedU32(buf, cksum);
  } else {
    tlen += sizeof(uint32_t);
  }

  (*pLen) = tlen;
  return code;
}

int32_t doStreamTimeSliceDecodeOpState(void* buf, int32_t len, SOperatorInfo* pOperator) {
  int32_t                       code = TSDB_CODE_SUCCESS;
  int32_t                       lino = 0;
  SStreamTimeSliceOperatorInfo* pInfo = pOperator->info;
  SExecTaskInfo*                pTaskInfo = pOperator->pTaskInfo;
  if (!pInfo) {
    code = TSDB_CODE_FAILED;
    QUERY_CHECK_CODE(code, lino, _end);
  }
  SStreamAggSupporter* pAggSup = &pInfo->streamAggSup;

  // 3.checksum
  int32_t dataLen = len - sizeof(uint32_t);
  void*   pCksum = POINTER_SHIFT(buf, dataLen);
  if (taosCheckChecksum(buf, dataLen, *(uint32_t*)pCksum) != TSDB_CODE_SUCCESS) {
    qError("stream event state is invalid");
    code = TSDB_CODE_FAILED;
    QUERY_CHECK_CODE(code, lino, _end);
  }

  // 1.streamAggSup.pResultRows
  int32_t mapSize = 0;
  buf = taosDecodeFixedI32(buf, &mapSize);
  for (int32_t i = 0; i < mapSize; i++) {
    SResultWindowInfo winfo = {0};
    buf = decodeSSessionKey(buf, &winfo.sessionWin);
    int32_t winCode = TSDB_CODE_SUCCESS;
    code = pAggSup->stateStore.streamStateSessionAddIfNotExist(
        pAggSup->pState, &winfo.sessionWin, pAggSup->gap, (void**)&winfo.pStatePos, &pAggSup->resultRowSize, &winCode);
    QUERY_CHECK_CODE(code, lino, _end);

    buf = decodeSResultWindowInfo(buf, &winfo, pInfo->streamAggSup.resultRowSize);
    code = tSimpleHashPut(pInfo->streamAggSup.pResultRows, &winfo.sessionWin, sizeof(SSessionKey), &winfo,
                          sizeof(SResultWindowInfo));
    QUERY_CHECK_CODE(code, lino, _end);
  }

  // 2.twAggSup
  buf = decodeSTimeWindowAggSupp(buf, &pInfo->twAggSup);

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s. task:%s", __func__, lino, tstrerror(code), GET_TASKID(pTaskInfo));
  }
  return code;
}

static int32_t initTimeSliceResultBuf(SStreamFillSupporter* pFillSup, SExprSupp* pExpSup) {
  pFillSup->rowSize = sizeof(TSKEY) + getResultRowSize(pExpSup->pCtx, pFillSup->numOfAllCols);
  pFillSup->next.key = INT64_MIN;
  pFillSup->nextNext.key = INT64_MIN;
  pFillSup->prev.key = INT64_MIN;
  pFillSup->cur.key = INT64_MIN;
  pFillSup->next.pRowVal = NULL;
  pFillSup->nextNext.pRowVal = NULL;
  pFillSup->prev.pRowVal = NULL;
  pFillSup->cur.pRowVal = NULL;

  return TSDB_CODE_SUCCESS;
}

static int32_t initTimeSliceFillSup(SStreamInterpFuncPhysiNode* pPhyFillNode, SExprSupp* pExprSup, int32_t numOfExprs,
                                    SStreamFillSupporter** ppResFillSup) {
  int32_t               code = TSDB_CODE_SUCCESS;
  int32_t               lino = 0;
  SStreamFillSupporter* pFillSup = taosMemoryCalloc(1, sizeof(SStreamFillSupporter));
  QUERY_CHECK_NULL(pFillSup, code, lino, _end, terrno);

  pFillSup->numOfFillCols = numOfExprs;
  int32_t numOfNotFillCols = 0;
  pFillSup->pAllColInfo = createFillColInfo(pExprSup->pExprInfo, pFillSup->numOfFillCols, NULL, numOfNotFillCols,
                                            (const SNodeListNode*)(pPhyFillNode->pFillValues));
  QUERY_CHECK_NULL(pFillSup->pAllColInfo, code, lino, _end, terrno);

  pFillSup->type = convertFillType(pPhyFillNode->fillMode);
  pFillSup->numOfAllCols = pFillSup->numOfFillCols + numOfNotFillCols;
  pFillSup->interval.interval = pPhyFillNode->interval;
  pFillSup->interval.intervalUnit = pPhyFillNode->intervalUnit;
  pFillSup->interval.offset = 0;
  pFillSup->interval.offsetUnit = pPhyFillNode->intervalUnit;
  pFillSup->interval.precision = pPhyFillNode->precision;
  pFillSup->interval.sliding = pPhyFillNode->interval;
  pFillSup->interval.slidingUnit = pPhyFillNode->intervalUnit;
  pFillSup->pAPI = NULL;
  _hash_fn_t hashFn = taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY);
  pFillSup->pResMap = tSimpleHashInit(16, hashFn);
  QUERY_CHECK_NULL(pFillSup->pResMap, code, lino, _end, terrno);

  code = initTimeSliceResultBuf(pFillSup, pExprSup);
  QUERY_CHECK_CODE(code, lino, _end);

  pFillSup->hasDelete = false;
  (*ppResFillSup) = pFillSup;

_end:
  if (code != TSDB_CODE_SUCCESS) {
    destroyStreamFillSupporter(pFillSup);
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
  return code;
}

static void doStreamTimeSliceSaveCheckpoint(SOperatorInfo* pOperator) {
  SStreamTimeSliceOperatorInfo* pInfo = pOperator->info;
  int32_t                       code = TSDB_CODE_SUCCESS;
  int32_t                       lino = 0;
  void*                         buf = NULL;
  if (needSaveStreamOperatorInfo(&pInfo->basic)) {
    int32_t len = 0;
    code = doStreamTimeSliceEncodeOpState(NULL, 0, pOperator, &len);
    QUERY_CHECK_CODE(code, lino, _end);

    buf = taosMemoryCalloc(1, len);
    QUERY_CHECK_NULL(buf, code, lino, _end, terrno);

    void* pBuf = buf;
    code = doStreamTimeSliceEncodeOpState(&pBuf, len, pOperator, &len);
    QUERY_CHECK_CODE(code, lino, _end);

    pInfo->streamAggSup.stateStore.streamStateSaveInfo(pInfo->streamAggSup.pState, STREAM_TIME_SLICE_OP_CHECKPOINT_NAME,
                                                       strlen(STREAM_TIME_SLICE_OP_CHECKPOINT_NAME), buf, len);
    saveStreamOperatorStateComplete(&pInfo->basic);
  }

_end:
  taosMemoryFreeClear(buf);
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
}

static SResultCellData* getSliceResultCell(SResultCellData* pRowVal, int32_t index) {
  if (!pRowVal) {
    return NULL;
  }
  char*            pData = (char*)pRowVal;
  SResultCellData* pCell = pRowVal;
  for (int32_t i = 0; i < index; i++) {
    pData += (pCell->bytes + sizeof(SResultCellData));
    pCell = (SResultCellData*)pData;
  }
  return pCell;
}

static int32_t fillPointResult(SStreamFillSupporter* pFillSup, SResultRowData* pResRow, TSKEY ts, SSDataBlock* pBlock,
                               bool* pRes, bool isFilled) {
  int32_t code = TSDB_CODE_SUCCESS;
  int32_t lino = 0;
  if (pBlock->info.rows >= pBlock->info.capacity) {
    (*pRes) = false;
    goto _end;
  }

  bool ckRes = true;
  code = checkResult(pFillSup, ts, pBlock->info.id.groupId, &ckRes);
  QUERY_CHECK_CODE(code, lino, _end);
  if (!ckRes) {
    (*pRes) = true;
    goto _end;
  }

  for (int32_t i = 0; i < pFillSup->numOfAllCols; i++) {
    SFillColInfo*    pFillCol = pFillSup->pAllColInfo + i;
    int32_t          dstSlotId = GET_DEST_SLOT_ID(pFillCol);
    SColumnInfoData* pDstCol = taosArrayGet(pBlock->pDataBlock, dstSlotId);

    if (isIrowtsPseudoColumn(pFillCol->pExpr)) {
      code = colDataSetVal(pDstCol, pBlock->info.rows, (char*)&ts, false);
      QUERY_CHECK_CODE(code, lino, _end);
    } else if (isIsfilledPseudoColumn(pFillCol->pExpr)) {
      code = colDataSetVal(pDstCol, pBlock->info.rows, (char*)&isFilled, false);
      QUERY_CHECK_CODE(code, lino, _end);
    } else {
      int32_t          srcSlot = pFillCol->pExpr->base.pParam[0].pCol->slotId;
      SResultCellData* pCell = getSliceResultCell(pResRow->pRowVal, srcSlot);
      code = setRowCell(pDstCol, pBlock->info.rows, pCell);
      QUERY_CHECK_CODE(code, lino, _end);
    }
  }

  pBlock->info.rows++;
  (*pRes) = true;

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
  return code;
}

static void fillNormalRange(SStreamFillSupporter* pFillSup, SStreamFillInfo* pFillInfo, SSDataBlock* pBlock) {
  int32_t code = TSDB_CODE_SUCCESS;
  int32_t lino = 0;
  while (hasRemainCalc(pFillInfo) && pBlock->info.rows < pBlock->info.capacity) {
    STimeWindow st = {.skey = pFillInfo->current, .ekey = pFillInfo->current};
    if (inWinRange(&pFillSup->winRange, &st)) {
      bool res = true;
      code = fillPointResult(pFillSup, pFillInfo->pResRow, pFillInfo->current, pBlock, &res, true);
      QUERY_CHECK_CODE(code, lino, _end);
    }
    pFillInfo->current = taosTimeAdd(pFillInfo->current, pFillSup->interval.sliding, pFillSup->interval.slidingUnit,
                                     pFillSup->interval.precision);
  }

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
}

static void fillLinearRange(SStreamFillSupporter* pFillSup, SStreamFillInfo* pFillInfo, SSDataBlock* pBlock) {
  int32_t code = TSDB_CODE_SUCCESS;
  int32_t lino = 0;
  while (hasRemainCalc(pFillInfo) && pBlock->info.rows < pBlock->info.capacity) {
    bool ckRes = true;
    code = checkResult(pFillSup, pFillInfo->current, pBlock->info.id.groupId, &ckRes);
    QUERY_CHECK_CODE(code, lino, _end);
    for (int32_t i = 0; i < pFillSup->numOfAllCols && ckRes; ++i) {
      SFillColInfo*    pFillCol = pFillSup->pAllColInfo + i;
      int32_t          dstSlotId = GET_DEST_SLOT_ID(pFillCol);
      SColumnInfoData* pDstCol = taosArrayGet(pBlock->pDataBlock, dstSlotId);
      int16_t          type = pDstCol->info.type;
      int32_t          index = pBlock->info.rows;
      if (isIrowtsPseudoColumn(pFillCol->pExpr)) {
        code = colDataSetVal(pDstCol, pBlock->info.rows, (char*)&pFillInfo->current, false);
        QUERY_CHECK_CODE(code, lino, _end);
      } else if (isIsfilledPseudoColumn(pFillCol->pExpr)) {
        bool isFilled = true;
        code = colDataSetVal(pDstCol, pBlock->info.rows, (char*)&isFilled, false);
        QUERY_CHECK_CODE(code, lino, _end);
      } else if (isInterpFunc(pFillCol->pExpr)) {
        int32_t          srcSlot = pFillCol->pExpr->base.pParam[0].pCol->slotId;
        SResultCellData* pCell = getSliceResultCell(pFillInfo->pResRow->pRowVal, srcSlot);
        if (IS_VAR_DATA_TYPE(type) || type == TSDB_DATA_TYPE_BOOL || pCell->isNull) {
          colDataSetNULL(pDstCol, index);
          continue;
        }
        SPoint* pEnd = taosArrayGet(pFillInfo->pLinearInfo->pEndPoints, srcSlot);
        double  vCell = 0;
        SPoint  start = {0};
        start.key = pFillInfo->pResRow->key;
        start.val = pCell->pData;

        SPoint cur = {0};
        cur.key = pFillInfo->current;
        cur.val = taosMemoryCalloc(1, pCell->bytes);
        QUERY_CHECK_NULL(cur.val, code, lino, _end, terrno);

        taosGetLinearInterpolationVal(&cur, pCell->type, &start, pEnd, pCell->type);
        code = colDataSetVal(pDstCol, index, (const char*)cur.val, false);
        QUERY_CHECK_CODE(code, lino, _end);

        destroySPoint(&cur);
      } else {
        int32_t          srcSlot = pFillCol->pExpr->base.pParam[0].pCol->slotId;
        SResultCellData* pCell = getSliceResultCell(pFillInfo->pResRow->pRowVal, srcSlot);
        code = setRowCell(pDstCol, pBlock->info.rows, pCell);
        QUERY_CHECK_CODE(code, lino, _end);
      }
    }
    pFillInfo->current = taosTimeAdd(pFillInfo->current, pFillSup->interval.sliding, pFillSup->interval.slidingUnit,
                                     pFillSup->interval.precision);
    if (ckRes) {
      pBlock->info.rows++;
    }
  }

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
}

static void setFillKeyInfo(TSKEY start, TSKEY end, SInterval* pInterval, SStreamFillInfo* pFillInfo) {
  pFillInfo->start = start;
  pFillInfo->current = pFillInfo->start;
  pFillInfo->end = end;
}

static void doStreamFillRange(SStreamFillSupporter* pFillSup, SStreamFillInfo* pFillInfo, SSDataBlock* pRes) {
  int32_t code = TSDB_CODE_SUCCESS;
  int32_t lino = 0;
  bool    res = true;
  if (pFillInfo->needFill == false && pFillInfo->pos != FILL_POS_INVALID) {
    code = fillPointResult(pFillSup, &pFillSup->cur, pFillSup->cur.key, pRes, &res, false);
    QUERY_CHECK_CODE(code, lino, _end);
    return;
  }

  if (pFillInfo->pos == FILL_POS_START) {
    code = fillPointResult(pFillSup, &pFillSup->cur, pFillSup->cur.key, pRes, &res, false);
    QUERY_CHECK_CODE(code, lino, _end);
    if (res) {
      pFillInfo->pos = FILL_POS_INVALID;
    }
  }
  if (pFillInfo->type != TSDB_FILL_LINEAR) {
    fillNormalRange(pFillSup, pFillInfo, pRes);
  } else {
    fillLinearRange(pFillSup, pFillInfo, pRes);

    if (pFillInfo->pos == FILL_POS_MID) {
      code = fillPointResult(pFillSup, &pFillSup->cur, pFillSup->cur.key, pRes, &res, false);
      QUERY_CHECK_CODE(code, lino, _end);
      if (res) {
        pFillInfo->pos = FILL_POS_INVALID;
      }
    }

    if (pFillInfo->current > pFillInfo->end && pFillInfo->pLinearInfo->hasNext) {
      pFillInfo->pLinearInfo->hasNext = false;
      taosArraySwap(pFillInfo->pLinearInfo->pEndPoints, pFillInfo->pLinearInfo->pNextEndPoints);
      pFillInfo->pResRow = &pFillSup->cur;
      setFillKeyInfo(pFillSup->cur.key, pFillInfo->pLinearInfo->nextEnd, &pFillSup->interval, pFillInfo);
      fillLinearRange(pFillSup, pFillInfo, pRes);
    }
  }
  if (pFillInfo->pos == FILL_POS_END) {
    code = fillPointResult(pFillSup, &pFillSup->cur, pFillSup->cur.key, pRes, &res, false);
    QUERY_CHECK_CODE(code, lino, _end);
    if (res) {
      pFillInfo->pos = FILL_POS_INVALID;
    }
  }

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
}

static int32_t getQualifiedRowNumAsc(SExprSupp* pExprSup, SSDataBlock* pBlock, int32_t rowId, bool ignoreNull) {
  if (rowId >= pBlock->info.rows) {
    return -1;
  }

  if (!ignoreNull) {
    return rowId;
  }

  for (int32_t i = rowId; rowId < pBlock->info.rows; i++) {
    if (!checkNullRow(pExprSup, pBlock, rowId, ignoreNull)) {
      return i;
    }
  }
  return -1;
}

static int32_t getQualifiedRowNumDesc(SExprSupp* pExprSup, SSDataBlock* pBlock, TSKEY* tsCols, int32_t rowId,
                                      bool ignoreNull) {
  TSKEY   ts = tsCols[rowId];
  int32_t resRow = -1;
  for (; rowId >= 0; rowId--) {
    if (checkNullRow(pExprSup, pBlock, rowId, ignoreNull)) {
      continue;
    }

    if (ts != tsCols[rowId]) {
      if (resRow >= 0) {
        break;
      } else {
        ts = tsCols[rowId];
      }
    }
    resRow = rowId;
  }
  return resRow;
}

static void setResultRowData(SSliceRowData** ppRowData, void* pBuff) { (*ppRowData) = (SSliceRowData*)pBuff; }

static void setPointBuff(SSlicePoint* pPoint, SStreamFillSupporter* pFillSup) {
  if (pFillSup->type != TSDB_FILL_LINEAR) {
    setResultRowData(&pPoint->pRightRow, pPoint->pResPos->pRowBuff);
    pPoint->pLeftRow = pPoint->pRightRow;
  } else {
    setResultRowData(&pPoint->pLeftRow, pPoint->pResPos->pRowBuff);
    void* pBuff = POINTER_SHIFT(pPoint->pResPos->pRowBuff, pFillSup->rowSize);
    setResultRowData(&pPoint->pRightRow, pBuff);
  }
}

static TSKEY adustPrevTsKey(TSKEY pointTs, TSKEY rowTs, SInterval* pInterval) {
  if (rowTs >= pointTs) {
    pointTs = taosTimeAdd(pointTs, pInterval->sliding, pInterval->slidingUnit, pInterval->precision);
  }
  return pointTs;
}

static TSKEY adustEndTsKey(TSKEY pointTs, TSKEY rowTs, SInterval* pInterval) {
  if (rowTs <= pointTs) {
    pointTs = taosTimeAdd(pointTs, pInterval->sliding * -1, pInterval->slidingUnit, pInterval->precision);
  }
  return pointTs;
}
static int32_t getLinearResultInfoFromState(SStreamAggSupporter* pAggSup, SStreamFillSupporter* pFillSup, TSKEY ts,
                                            int64_t groupId, SSlicePoint* pCurPoint, SSlicePoint* pPrevPoint,
                                            SSlicePoint* pNextPoint) {
  int32_t code = TSDB_CODE_SUCCESS;
  int32_t lino = 0;
  int32_t tmpRes = TSDB_CODE_SUCCESS;
  void*   pState = pAggSup->pState;
  resetPrevAndNextWindow(pFillSup);
  pCurPoint->pResPos = NULL;
  pPrevPoint->pResPos = NULL;
  pNextPoint->pResPos = NULL;

  pCurPoint->key.groupId = groupId;
  pCurPoint->key.ts = ts;
  int32_t curVLen = 0;
  code =
      pAggSup->stateStore.streamStateFillGet(pState, &pCurPoint->key, (void**)&pCurPoint->pResPos, &curVLen, &tmpRes);
  QUERY_CHECK_CODE(code, lino, _end);

  setPointBuff(pCurPoint, pFillSup);

  if (HAS_ROW_DATA(pCurPoint->pRightRow)) {
    pFillSup->cur.key = pCurPoint->pRightRow->key;
    pFillSup->cur.pRowVal = pCurPoint->pRightRow->pRowVal;
    if (HAS_NON_ROW_DATA(pCurPoint->pLeftRow)) {
      pPrevPoint->key.groupId = groupId;
      int32_t preVLen = 0;
      code = pAggSup->stateStore.streamStateFillGetPrev(pState, &pCurPoint->key, &pPrevPoint->key,
                                                        (void**)&pPrevPoint->pResPos, &preVLen, &tmpRes);
      QUERY_CHECK_CODE(code, lino, _end);
      if (tmpRes == TSDB_CODE_SUCCESS) {
        ASSERT(!IS_INVALID_WIN_KEY(pPrevPoint->key.ts));
        setPointBuff(pPrevPoint, pFillSup);
        if (HAS_ROW_DATA(pPrevPoint->pRightRow)) {
          pFillSup->prev.key = pPrevPoint->pRightRow->key;
          pFillSup->prev.pRowVal = pPrevPoint->pRightRow->pRowVal;
        } else {
          pFillSup->prev.key = pPrevPoint->pLeftRow->key;
          pFillSup->prev.pRowVal = pPrevPoint->pLeftRow->pRowVal;
        }
        pFillSup->prevOriginKey = pFillSup->prev.key;
        pFillSup->prev.key = adustPrevTsKey(pPrevPoint->key.ts, pFillSup->prev.key, &pFillSup->interval);
      }
      goto _end;
    }
  }

  if (HAS_ROW_DATA(pCurPoint->pLeftRow)) {
    pFillSup->prev.key = pCurPoint->pLeftRow->key;
    pFillSup->prev.pRowVal = pCurPoint->pLeftRow->pRowVal;
    pFillSup->prevOriginKey = pFillSup->prev.key;
    pFillSup->prev.key = adustPrevTsKey(pCurPoint->key.ts, pFillSup->prev.key, &pFillSup->interval);
    if (HAS_NON_ROW_DATA(pCurPoint->pRightRow)) {
      pNextPoint->key.groupId = groupId;
      int32_t nextVLen = 0;
      code = pAggSup->stateStore.streamStateFillGetNext(pState, &pCurPoint->key, &pNextPoint->key,
                                                        (void**)&pNextPoint->pResPos, &nextVLen, &tmpRes);
      QUERY_CHECK_CODE(code, lino, _end);
      if (tmpRes == TSDB_CODE_SUCCESS) {
        ASSERT(!IS_INVALID_WIN_KEY(pNextPoint->key.ts));
        setPointBuff(pNextPoint, pFillSup);
        if (HAS_ROW_DATA(pNextPoint->pLeftRow)) {
          pFillSup->next.key = pNextPoint->pLeftRow->key;
          pFillSup->next.pRowVal = pNextPoint->pLeftRow->pRowVal;
        } else {
          pFillSup->next.key = pNextPoint->pRightRow->key;
          pFillSup->next.pRowVal = pNextPoint->pRightRow->pRowVal;
        }
        pFillSup->nextOriginKey = pFillSup->next.key;
        pFillSup->next.key = adustEndTsKey(pNextPoint->key.ts, pFillSup->next.key, &pFillSup->interval);
      } else {
        resetFillWindow(&pFillSup->prev);
      }
    }
  }

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
  return code;
}

static int32_t getResultInfoFromState(SStreamAggSupporter* pAggSup, SStreamFillSupporter* pFillSup, TSKEY ts,
                                      int64_t groupId, SSlicePoint* pCurPoint, SSlicePoint* pPrevPoint,
                                      SSlicePoint* pNextPoint) {
  int32_t code = TSDB_CODE_SUCCESS;
  int32_t lino = 0;
  int32_t tmpRes = TSDB_CODE_SUCCESS;
  void*   pState = pAggSup->pState;
  resetPrevAndNextWindow(pFillSup);
  pCurPoint->pResPos = NULL;
  pPrevPoint->pResPos = NULL;
  pNextPoint->pResPos = NULL;

  pCurPoint->key.groupId = groupId;
  pCurPoint->key.ts = ts;
  int32_t curVLen = 0;
  code =
      pAggSup->stateStore.streamStateFillGet(pState, &pCurPoint->key, (void**)&pCurPoint->pResPos, &curVLen, &tmpRes);
  QUERY_CHECK_CODE(code, lino, _end);

  if (tmpRes == TSDB_CODE_SUCCESS) {
    setPointBuff(pCurPoint, pFillSup);
    pFillSup->cur.key = pCurPoint->pRightRow->key;
    pFillSup->cur.pRowVal = pCurPoint->pRightRow->pRowVal;
  } else {
    pFillSup->cur.key = pCurPoint->key.ts + 1;
  }

  pPrevPoint->key.groupId = groupId;
  int32_t preVLen = 0;
  code = pAggSup->stateStore.streamStateFillGetPrev(pState, &pCurPoint->key, &pPrevPoint->key,
                                                    (void**)&pPrevPoint->pResPos, &preVLen, &tmpRes);
  QUERY_CHECK_CODE(code, lino, _end);
  if (tmpRes == TSDB_CODE_SUCCESS) {
    ASSERT(!IS_INVALID_WIN_KEY(pPrevPoint->key.ts));
    setPointBuff(pPrevPoint, pFillSup);
    if (HAS_ROW_DATA(pPrevPoint->pRightRow)) {
      pFillSup->prev.key = pPrevPoint->pRightRow->key;
      pFillSup->prev.pRowVal = pPrevPoint->pRightRow->pRowVal;
    } else {
      pFillSup->prev.key = pPrevPoint->pLeftRow->key;
      pFillSup->prev.pRowVal = pPrevPoint->pLeftRow->pRowVal;
    }
    pFillSup->prev.key = adustPrevTsKey(pPrevPoint->key.ts, pFillSup->prev.key, &pFillSup->interval);
  }

  pNextPoint->key.groupId = groupId;
  int32_t nextVLen = 0;
  code = pAggSup->stateStore.streamStateFillGetNext(pState, &pCurPoint->key, &pNextPoint->key,
                                                    (void**)&pNextPoint->pResPos, &nextVLen, &tmpRes);
  QUERY_CHECK_CODE(code, lino, _end);
  if (tmpRes == TSDB_CODE_SUCCESS) {
    ASSERT(!IS_INVALID_WIN_KEY(pNextPoint->key.ts));
    setPointBuff(pNextPoint, pFillSup);
    if (HAS_ROW_DATA(pNextPoint->pLeftRow)) {
      pFillSup->next.key = pNextPoint->pLeftRow->key;
      pFillSup->next.pRowVal = pNextPoint->pLeftRow->pRowVal;
    } else {
      pFillSup->next.key = pNextPoint->pRightRow->key;
      pFillSup->next.pRowVal = pNextPoint->pRightRow->pRowVal;
    }
    pFillSup->next.key = adustEndTsKey(pNextPoint->key.ts, pFillSup->next.key, &pFillSup->interval);
  }

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
  return code;
}

static int32_t getPointInfoFromStateRight(SStreamAggSupporter* pAggSup, SStreamFillSupporter* pFillSup, TSKEY ts,
                                          int64_t groupId, SSlicePoint* pCurPoint, SSlicePoint* pNextPoint,
                                          int32_t* pWinCode) {
  int32_t code = TSDB_CODE_SUCCESS;
  int32_t lino = 0;
  int32_t tmpRes = TSDB_CODE_SUCCESS;
  void*   pState = pAggSup->pState;
  pCurPoint->pResPos = NULL;
  pNextPoint->pResPos = NULL;

  pNextPoint->key.groupId = groupId;
  STimeWindow stw = {.skey = ts, .ekey = ts};
  getNextTimeWindow(&pFillSup->interval, &stw, TSDB_ORDER_ASC);
  pNextPoint->key.ts = stw.skey;

  int32_t curVLen = 0;
  code = pAggSup->stateStore.streamStateFillAddIfNotExist(pState, &pNextPoint->key, (void**)&pNextPoint->pResPos,
                                                          &curVLen, pWinCode);
  QUERY_CHECK_CODE(code, lino, _end);

  setPointBuff(pNextPoint, pFillSup);

  if (*pWinCode != TSDB_CODE_SUCCESS) {
    if (pNextPoint->pLeftRow) {
      SET_WIN_KEY_INVALID(pNextPoint->pLeftRow->key);
    }
    if (pNextPoint->pRightRow) {
      SET_WIN_KEY_INVALID(pNextPoint->pRightRow->key);
    }
  }

  SET_WIN_KEY_INVALID(pCurPoint->key.ts);
  pCurPoint->key.groupId = groupId;
  int32_t nextVLen = 0;
  code = pAggSup->stateStore.streamStateFillGetPrev(pState, &pNextPoint->key, &pCurPoint->key,
                                                    (void**)&pCurPoint->pResPos, &nextVLen, &tmpRes);
  QUERY_CHECK_CODE(code, lino, _end);
  if (tmpRes == TSDB_CODE_SUCCESS) {
    setPointBuff(pCurPoint, pFillSup);
  }

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
  return code;
}

static int32_t getPointInfoFromState(SStreamAggSupporter* pAggSup, SStreamFillSupporter* pFillSup, TSKEY ts,
                                     int64_t groupId, SSlicePoint* pCurPoint, SSlicePoint* pNextPoint,
                                     int32_t* pWinCode) {
  int32_t code = TSDB_CODE_SUCCESS;
  int32_t lino = 0;
  int32_t tmpRes = TSDB_CODE_SUCCESS;
  void*   pState = pAggSup->pState;
  pCurPoint->pResPos = NULL;
  pNextPoint->pResPos = NULL;
  pCurPoint->key.groupId = groupId;
  pCurPoint->key.ts = ts;

  int32_t curVLen = 0;
  code = pAggSup->stateStore.streamStateFillAddIfNotExist(pState, &pCurPoint->key, (void**)&pCurPoint->pResPos,
                                                          &curVLen, pWinCode);
  QUERY_CHECK_CODE(code, lino, _end);

  setPointBuff(pCurPoint, pFillSup);

  if (*pWinCode != TSDB_CODE_SUCCESS) {
    if (pCurPoint->pLeftRow) {
      SET_WIN_KEY_INVALID(pCurPoint->pLeftRow->key);
    }
    if (pCurPoint->pRightRow) {
      SET_WIN_KEY_INVALID(pCurPoint->pRightRow->key);
    }
  }

  int32_t nextVLen = 0;
  pNextPoint->key.groupId = groupId;
  if (pFillSup->type != TSDB_FILL_LINEAR && pFillSup->type != TSDB_FILL_PREV) {
    SET_WIN_KEY_INVALID(pNextPoint->key.ts);
    code = pAggSup->stateStore.streamStateFillGetNext(pState, &pCurPoint->key, &pNextPoint->key,
                                                      (void**)&pNextPoint->pResPos, &nextVLen, &tmpRes);
    QUERY_CHECK_CODE(code, lino, _end);
    if (tmpRes == TSDB_CODE_SUCCESS) {
      setPointBuff(pNextPoint, pFillSup);
    }
  } else {
    pNextPoint->key.ts = taosTimeAdd(pCurPoint->key.ts, pFillSup->interval.sliding, pFillSup->interval.slidingUnit,
                                     pFillSup->interval.precision);
    code = pAggSup->stateStore.streamStateFillAddIfNotExist(pState, &pNextPoint->key, (void**)&pNextPoint->pResPos,
                                                            &nextVLen, &tmpRes);
    QUERY_CHECK_CODE(code, lino, _end);
    setPointBuff(pNextPoint, pFillSup);
    if (tmpRes != TSDB_CODE_SUCCESS) {
      SET_WIN_KEY_INVALID(pNextPoint->pLeftRow->key);
      SET_WIN_KEY_INVALID(pNextPoint->pRightRow->key);
    }
  }

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
  return code;
}

static void copyNonFillValueInfo(SStreamFillSupporter* pFillSup, SStreamFillInfo* pFillInfo) {
  for (int32_t i = 0; i < pFillSup->numOfAllCols; ++i) {
    SFillColInfo* pFillCol = pFillSup->pAllColInfo + i;
    if (!isInterpFunc(pFillCol->pExpr) && !isIrowtsPseudoColumn(pFillCol->pExpr) &&
        !isIsfilledPseudoColumn(pFillCol->pExpr)) {
      int32_t          srcSlot = pFillCol->pExpr->base.pParam[0].pCol->slotId;
      SResultCellData* pSrcCell = getResultCell(&pFillSup->cur, srcSlot);
      SResultCellData* pDestCell = getResultCell(pFillInfo->pResRow, srcSlot);
      pDestCell->isNull = pSrcCell->isNull;
      if (!pDestCell->isNull) {
        memcpy(pDestCell->pData, pSrcCell->pData, pSrcCell->bytes);
      }
    }
  }
}

static void copyCalcRowDeltaData(SResultRowData* pEndRow, SArray* pEndPoins, SFillColInfo* pFillCol, int32_t numOfCol) {
  for (int32_t i = 0; i < numOfCol; i++) {
    if (isInterpFunc(pFillCol[i].pExpr)) {
      int32_t          slotId = pFillCol[i].pExpr->base.pParam[0].pCol->slotId;
      SResultCellData* pECell = getResultCell(pEndRow, slotId);
      SPoint*          pPoint = taosArrayGet(pEndPoins, slotId);
      pPoint->key = pEndRow->key;
      memcpy(pPoint->val, pECell->pData, pECell->bytes);
    }
  }
}

static void setTimeSliceFillRule(SStreamFillSupporter* pFillSup, SStreamFillInfo* pFillInfo, TSKEY ts) {
  if (!hasPrevWindow(pFillSup) && !hasNextWindow(pFillSup)) {
    pFillInfo->needFill = false;
    pFillInfo->pos = FILL_POS_START;
    goto _end;
  }
  TSKEY prevWKey = INT64_MIN;
  TSKEY nextWKey = INT64_MIN;
  if (hasPrevWindow(pFillSup)) {
    prevWKey = pFillSup->prev.key;
  }
  if (hasNextWindow(pFillSup)) {
    nextWKey = pFillSup->next.key;
  }

  pFillInfo->needFill = true;
  pFillInfo->pos = FILL_POS_INVALID;
  switch (pFillInfo->type) {
    case TSDB_FILL_NULL:
    case TSDB_FILL_NULL_F:
    case TSDB_FILL_SET_VALUE:
    case TSDB_FILL_SET_VALUE_F: {
      if (hasPrevWindow(pFillSup)) {
        TSKEY endTs = adustEndTsKey(ts, pFillSup->cur.key, &pFillSup->interval);
        setFillKeyInfo(prevWKey, endTs, &pFillSup->interval, pFillInfo);
        pFillInfo->pos = FILL_POS_END;
      } else {
        TSKEY startTs = adustPrevTsKey(ts, pFillSup->cur.key, &pFillSup->interval);
        setFillKeyInfo(startTs, nextWKey, &pFillSup->interval, pFillInfo);
        pFillInfo->pos = FILL_POS_START;
      }
      copyNonFillValueInfo(pFillSup, pFillInfo);
      pFillInfo->pResRow->key = ts;
    } break;
    case TSDB_FILL_PREV: {
      if (hasNextWindow(pFillSup)) {
        TSKEY startTs = adustPrevTsKey(ts, pFillSup->cur.key, &pFillSup->interval);
        setFillKeyInfo(startTs, nextWKey, &pFillSup->interval, pFillInfo);
        pFillInfo->pos = FILL_POS_START;
        resetFillWindow(&pFillSup->prev);
        pFillSup->prev.key = ts;
        pFillSup->prev.pRowVal = pFillSup->cur.pRowVal;
      } else {
        ASSERT(hasPrevWindow(pFillSup));
        TSKEY endTs = adustEndTsKey(ts, pFillSup->cur.key, &pFillSup->interval);
        setFillKeyInfo(prevWKey, endTs, &pFillSup->interval, pFillInfo);
        pFillInfo->pos = FILL_POS_END;
      }
      pFillInfo->pResRow = &pFillSup->prev;
    } break;
    case TSDB_FILL_NEXT: {
      if (hasPrevWindow(pFillSup)) {
        TSKEY endTs = adustEndTsKey(ts, pFillSup->cur.key, &pFillSup->interval);
        setFillKeyInfo(prevWKey, endTs, &pFillSup->interval, pFillInfo);
        pFillInfo->pos = FILL_POS_END;
        resetFillWindow(&pFillSup->next);
        pFillSup->next.key = ts;
        pFillSup->next.pRowVal = pFillSup->cur.pRowVal;
      } else {
        ASSERT(hasNextWindow(pFillSup));
        TSKEY startTs = adustPrevTsKey(ts, pFillSup->cur.key, &pFillSup->interval);
        setFillKeyInfo(startTs, nextWKey, &pFillSup->interval, pFillInfo);
        pFillInfo->pos = FILL_POS_START;
        resetFillWindow(&pFillSup->prev);
      }
      pFillInfo->pResRow = &pFillSup->next;
    } break;
    case TSDB_FILL_LINEAR: {
      if (hasPrevWindow(pFillSup) && hasNextWindow(pFillSup)) {
        setFillKeyInfo(prevWKey, nextWKey, &pFillSup->interval, pFillInfo);
        pFillInfo->pos = FILL_POS_INVALID;
        SET_WIN_KEY_INVALID(pFillInfo->pLinearInfo->nextEnd);
        pFillSup->next.key = pFillSup->nextOriginKey;
        copyCalcRowDeltaData(&pFillSup->next, pFillInfo->pLinearInfo->pEndPoints, pFillSup->pAllColInfo,
                         pFillSup->numOfAllCols);
        pFillSup->prev.key = pFillSup->prevOriginKey;
        pFillInfo->pResRow = &pFillSup->prev;
        pFillInfo->pLinearInfo->hasNext = false;
      } else if (hasPrevWindow(pFillSup)) {
        TSKEY endTs = adustEndTsKey(ts, pFillSup->cur.key, &pFillSup->interval);
        setFillKeyInfo(prevWKey, endTs, &pFillSup->interval, pFillInfo);
        pFillInfo->pos = FILL_POS_END;
        SET_WIN_KEY_INVALID(pFillInfo->pLinearInfo->nextEnd);
        copyCalcRowDeltaData(&pFillSup->cur, pFillInfo->pLinearInfo->pEndPoints, pFillSup->pAllColInfo,
                         pFillSup->numOfAllCols);
        pFillSup->prev.key = pFillSup->prevOriginKey;
        pFillInfo->pResRow = &pFillSup->prev;
        pFillInfo->pLinearInfo->hasNext = false;
      } else {
        ASSERT(hasNextWindow(pFillSup));
        TSKEY startTs = adustPrevTsKey(ts, pFillSup->cur.key, &pFillSup->interval);
        setFillKeyInfo(startTs, nextWKey, &pFillSup->interval, pFillInfo);
        pFillInfo->pos = FILL_POS_START;
        SET_WIN_KEY_INVALID(pFillInfo->pLinearInfo->nextEnd);
        pFillSup->next.key = pFillSup->nextOriginKey;
        copyCalcRowDeltaData(&pFillSup->next, pFillInfo->pLinearInfo->pEndPoints, pFillSup->pAllColInfo,
                         pFillSup->numOfAllCols);
        pFillInfo->pResRow = &pFillSup->cur;
        pFillInfo->pLinearInfo->hasNext = false;
      }
    } break;
    default:
      ASSERT(0);
      break;
  }

_end:
  if (ts != pFillSup->cur.key) {
    pFillInfo->pos = FILL_POS_INVALID;
  }
}

static bool needAdjustValue(SSlicePoint* pPoint, TSKEY ts, bool isLeft, int32_t fillType) {
  if (IS_INVALID_WIN_KEY(pPoint->key.ts)) {
    return false;
  }

  switch (fillType) {
    case TSDB_FILL_NULL:
    case TSDB_FILL_NULL_F:
    case TSDB_FILL_SET_VALUE:
    case TSDB_FILL_SET_VALUE_F: {
      if (!isLeft && HAS_NON_ROW_DATA(pPoint->pRightRow)) {
        return true;
      }
    } break;
    case TSDB_FILL_PREV: {
      if (isLeft && (HAS_NON_ROW_DATA(pPoint->pLeftRow) || pPoint->pLeftRow->key <= ts)) {
        return true;
      }

      if (!isLeft && pPoint->key.ts == ts) {
        return true;
      }
    } break;
    case TSDB_FILL_NEXT: {
      if (!isLeft && (HAS_NON_ROW_DATA(pPoint->pRightRow) || pPoint->pRightRow->key >= ts)) {
        return true;
      }
    } break;
    case TSDB_FILL_LINEAR: {
      if (isLeft && (HAS_NON_ROW_DATA(pPoint->pLeftRow) || pPoint->pLeftRow->key <= ts)) {
        return true;
      } else if (!isLeft && (HAS_NON_ROW_DATA(pPoint->pRightRow) || pPoint->pRightRow->key >= ts)) {
        return true;
      }
    } break;
    default:
      ASSERT(0);
  }
  return false;
}

static void transBlockToResultRow(const SSDataBlock* pBlock, int32_t rowId, TSKEY ts, SSliceRowData* pRowVal) {
  int32_t numOfCols = taosArrayGetSize(pBlock->pDataBlock);
  for (int32_t i = 0; i < numOfCols; ++i) {
    SColumnInfoData* pColData = taosArrayGet(pBlock->pDataBlock, i);
    SResultCellData* pCell = getSliceResultCell(pRowVal->pRowVal, i);
    if (!colDataIsNull_s(pColData, rowId)) {
      pCell->isNull = false;
      pCell->type = pColData->info.type;
      pCell->bytes = pColData->info.bytes;
      char* val = colDataGetData(pColData, rowId);
      if (IS_VAR_DATA_TYPE(pCell->type)) {
        memcpy(pCell->pData, val, varDataTLen(val));
      } else {
        memcpy(pCell->pData, val, pCell->bytes);
      }
    } else {
      pCell->isNull = true;
    }
  }
  pRowVal->key = ts;
}

static int32_t saveTimeSliceWinResultInfo(SStreamAggSupporter* pAggSup, int8_t trigger, SWinKey* pKey,
                                          SSHashObj* pUpdatedMap, bool needDel, SSHashObj* pDeletedMap) {
  int32_t code = TSDB_CODE_SUCCESS;
  int32_t lino = 0;

  if (trigger == STREAM_TRIGGER_AT_ONCE) {
    code = saveTimeSliceWinResult(pKey, pUpdatedMap);
    QUERY_CHECK_CODE(code, lino, _end);
    if (needDel) {
      code = saveTimeSliceWinResult(pKey, pDeletedMap);
      QUERY_CHECK_CODE(code, lino, _end);
    }
  } else if (trigger == STREAM_TRIGGER_FORCE_WINDOW_CLOSE) {
    code = pAggSup->stateStore.streamStateGroupPut(pAggSup->pState, pKey->groupId, NULL, 0);
    QUERY_CHECK_CODE(code, lino, _end);
  }

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
  return code;
}

static void doStreamTimeSliceImpl(SOperatorInfo* pOperator, SSDataBlock* pBlock) {
  int32_t                       code = TSDB_CODE_SUCCESS;
  int32_t                       lino = 0;
  int32_t                       winCode = TSDB_CODE_SUCCESS;
  SStreamTimeSliceOperatorInfo* pInfo = (SStreamTimeSliceOperatorInfo*)pOperator->info;
  SExecTaskInfo*                pTaskInfo = pOperator->pTaskInfo;
  SStreamAggSupporter*          pAggSup = &pInfo->streamAggSup;
  SExprSupp*                    pExprSup = &pOperator->exprSupp;
  int32_t                       numOfOutput = pExprSup->numOfExprs;
  SColumnInfoData*              pColDataInfo = taosArrayGet(pBlock->pDataBlock, pInfo->primaryTsIndex);
  TSKEY*                        tsCols = (int64_t*)pColDataInfo->pData;
  void*                         pPkVal = NULL;
  int32_t                       pkLen = 0;
  int64_t                       groupId = pBlock->info.id.groupId;
  SColumnInfoData*              pPkColDataInfo = NULL;
  SStreamFillSupporter*         pFillSup = pInfo->pFillSup;
  SStreamFillInfo*              pFillInfo = pInfo->pFillInfo;
  if (hasSrcPrimaryKeyCol(&pInfo->basic)) {
    pPkColDataInfo = taosArrayGet(pBlock->pDataBlock, pInfo->basic.primaryPkIndex);
  }

  pFillSup->winRange = pTaskInfo->streamInfo.fillHistoryWindow;
  if (pFillSup->winRange.ekey <= 0) {
    pFillSup->winRange.ekey = INT64_MAX;
  }

  int32_t startPos = 0;
  for (; startPos < pBlock->info.rows; startPos++) {
    if (hasSrcPrimaryKeyCol(&pInfo->basic) && pInfo->ignoreExpiredData) {
      pPkVal = colDataGetData(pPkColDataInfo, startPos);
      pkLen = colDataGetRowLength(pPkColDataInfo, startPos);
    }

    if (pInfo->ignoreExpiredData && checkExpiredData(&pAggSup->stateStore, pAggSup->pUpdateInfo, &pInfo->twAggSup,
                                                     pBlock->info.id.uid, tsCols[startPos], pPkVal, pkLen)) {
      qDebug("===stream===ignore expired data, window end ts:%" PRId64 ", maxts - wartermak:%" PRId64, tsCols[startPos],
             pInfo->twAggSup.maxTs - pInfo->twAggSup.waterMark);
      continue;
    }

    if (checkNullRow(pExprSup, pBlock, startPos, pInfo->ignoreNull)) {
      continue;
    }
    break;
  }

  if (startPos >= pBlock->info.rows) {
    return;
  }

  SResultRowInfo dumyInfo = {0};
  dumyInfo.cur.pageId = -1;
  STimeWindow curWin = getActiveTimeWindow(NULL, &dumyInfo, tsCols[startPos], &pFillSup->interval, TSDB_ORDER_ASC);
  SSlicePoint curPoint = {0};
  SSlicePoint nextPoint = {0};
  bool        left = false;
  bool        right = false;
  if (pFillSup->type != TSDB_FILL_PREV || curWin.skey == tsCols[startPos]) {
    code = getPointInfoFromState(pAggSup, pFillSup, curWin.skey, groupId, &curPoint, &nextPoint, &winCode);
  } else {
    code = getPointInfoFromStateRight(pAggSup, pFillSup, curWin.skey, groupId, &curPoint, &nextPoint, &winCode);
  }
  QUERY_CHECK_CODE(code, lino, _end);

  right = needAdjustValue(&curPoint, tsCols[startPos], false, pFillSup->type);
  if (right) {
    transBlockToResultRow(pBlock, startPos, tsCols[startPos], curPoint.pRightRow);
    bool needDel = pInfo->destHasPrimaryKey && winCode == TSDB_CODE_SUCCESS;
    saveTimeSliceWinResultInfo(pAggSup, pInfo->twAggSup.calTrigger, &curPoint.key, pInfo->pUpdatedMap, needDel,
                               pInfo->pDeletedMap);
  }
  releaseOutputBuf(pAggSup->pState, curPoint.pResPos, &pAggSup->stateStore);

  while (startPos < pBlock->info.rows) {
    int32_t numOfWin = getNumOfRowsInTimeWindow(&pBlock->info, tsCols, startPos, curWin.ekey, binarySearchForKey, NULL,
                                                TSDB_ORDER_ASC);
    startPos += numOfWin;
    int32_t leftRowId = getQualifiedRowNumDesc(pExprSup, pBlock, tsCols, startPos - 1, pInfo->ignoreNull);
    left = needAdjustValue(&nextPoint, tsCols[leftRowId], true, pFillSup->type);
    if (left) {
      transBlockToResultRow(pBlock, leftRowId, tsCols[leftRowId], nextPoint.pLeftRow);
      bool needDel = pInfo->destHasPrimaryKey && winCode == TSDB_CODE_SUCCESS;
      saveTimeSliceWinResultInfo(pAggSup, pInfo->twAggSup.calTrigger, &nextPoint.key, pInfo->pUpdatedMap, needDel,
                                 pInfo->pDeletedMap);
    }
    releaseOutputBuf(pAggSup->pState, nextPoint.pResPos, &pAggSup->stateStore);

    startPos = getQualifiedRowNumAsc(pExprSup, pBlock, startPos, pInfo->ignoreNull);
    if (startPos < 0) {
      break;
    }
    curWin = getActiveTimeWindow(NULL, &dumyInfo, tsCols[startPos], &pFillSup->interval, TSDB_ORDER_ASC);
    if (pFillSup->type != TSDB_FILL_PREV || curWin.skey == tsCols[startPos]) {
      code = getPointInfoFromState(pAggSup, pFillSup, curWin.skey, groupId, &curPoint, &nextPoint, &winCode);
    } else {
      code = getPointInfoFromStateRight(pAggSup, pFillSup, curWin.skey, groupId, &curPoint, &nextPoint, &winCode);
    }
    QUERY_CHECK_CODE(code, lino, _end);

    right = needAdjustValue(&curPoint, tsCols[startPos], false, pFillSup->type);
    if (right) {
      transBlockToResultRow(pBlock, startPos, tsCols[startPos], curPoint.pRightRow);
      bool needDel = pInfo->destHasPrimaryKey && winCode == TSDB_CODE_SUCCESS;
      saveTimeSliceWinResultInfo(pAggSup, pInfo->twAggSup.calTrigger, &curPoint.key, pInfo->pUpdatedMap, needDel,
                                 pInfo->pDeletedMap);
    }
    releaseOutputBuf(pAggSup->pState, curPoint.pResPos, &pAggSup->stateStore);
  }

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
}

void doBuildTimeSlicePointResult(SStreamAggSupporter* pAggSup, SStreamFillSupporter* pFillSup,
                                 SStreamFillInfo* pFillInfo, SSDataBlock* pBlock, SGroupResInfo* pGroupResInfo) {
  int32_t code = TSDB_CODE_SUCCESS;
  int32_t lino = 0;
  blockDataCleanup(pBlock);
  if (!hasRemainResults(pGroupResInfo)) {
    return;
  }

  // clear the existed group id
  pBlock->info.id.groupId = 0;
  int32_t numOfRows = getNumOfTotalRes(pGroupResInfo);
  for (; pGroupResInfo->index < numOfRows; pGroupResInfo->index++) {
    SWinKey* pKey = (SWinKey*)taosArrayGet(pGroupResInfo->pRows, pGroupResInfo->index);
    if (pBlock->info.id.groupId == 0) {
      pBlock->info.id.groupId = pKey->groupId;
    } else if (pBlock->info.id.groupId != pKey->groupId) {
      break;
    }
    SSlicePoint curPoint = {.key.ts = pKey->ts, .key.groupId = pKey->groupId};
    SSlicePoint prevPoint = {0};
    SSlicePoint nextPoint = {0};
    if (pFillSup->type != TSDB_FILL_LINEAR) {
      code = getResultInfoFromState(pAggSup, pFillSup, pKey->ts, pKey->groupId, &curPoint, &prevPoint, &nextPoint);
    } else {
      code =
          getLinearResultInfoFromState(pAggSup, pFillSup, pKey->ts, pKey->groupId, &curPoint, &prevPoint, &nextPoint);
    }
    QUERY_CHECK_CODE(code, lino, _end);

    setTimeSliceFillRule(pFillSup, pFillInfo, pKey->ts);
    doStreamFillRange(pFillSup, pFillInfo, pBlock);
    releaseOutputBuf(pAggSup->pState, curPoint.pResPos, &pAggSup->stateStore);
    releaseOutputBuf(pAggSup->pState, prevPoint.pResPos, &pAggSup->stateStore);
    releaseOutputBuf(pAggSup->pState, nextPoint.pResPos, &pAggSup->stateStore);
  }

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
}

static int32_t buildTimeSliceResult(SOperatorInfo* pOperator, SSDataBlock** ppRes) {
  int32_t                       code = TSDB_CODE_SUCCESS;
  int32_t                       lino = 0;
  SStreamTimeSliceOperatorInfo* pInfo = pOperator->info;
  SExecTaskInfo*                pTaskInfo = pOperator->pTaskInfo;
  uint16_t                      opType = pOperator->operatorType;
  SStreamAggSupporter*          pAggSup = &pInfo->streamAggSup;

  doBuildDeleteResultImpl(&pAggSup->stateStore, pAggSup->pState, pInfo->pDelWins, &pInfo->delIndex, pInfo->pDelRes);
  if (pInfo->pDelRes->info.rows != 0) {
    // process the rest of the data
    printDataBlock(pInfo->pDelRes, getStreamOpName(opType), GET_TASKID(pTaskInfo));
    (*ppRes) = pInfo->pDelRes;
    goto _end;
  }

  doBuildTimeSlicePointResult(pAggSup, pInfo->pFillSup, pInfo->pFillInfo, pInfo->pRes, &pInfo->groupResInfo);
  if (pInfo->pRes->info.rows != 0) {
    printDataBlock(pInfo->pRes, getStreamOpName(opType), GET_TASKID(pTaskInfo));
    (*ppRes) = pInfo->pRes;
    goto _end;
  }

  (*ppRes) = NULL;

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
  return code;
}

int32_t getSliceMaxTsWins(const SArray* pAllWins, SArray* pMaxWins) {
  int32_t code = TSDB_CODE_SUCCESS;
  int32_t lino = 0;
  int32_t size = taosArrayGetSize(pAllWins);
  if (size == 0) {
    goto _end;
  }
  SWinKey* pKey = taosArrayGet(pAllWins, size - 1);
  void*    tmp = taosArrayPush(pMaxWins, pKey);
  QUERY_CHECK_NULL(tmp, code, lino, _end, terrno);

  if (pKey->groupId == 0) {
    goto _end;
  }
  uint64_t preGpId = pKey->groupId;
  for (int32_t i = size - 2; i >= 0; i--) {
    pKey = taosArrayGet(pAllWins, i);
    if (preGpId != pKey->groupId) {
      void* tmp = taosArrayPush(pMaxWins, pKey);
      QUERY_CHECK_NULL(tmp, code, lino, _end, terrno);
      preGpId = pKey->groupId;
    }
  }

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
  return code;
}

static int32_t doDeleteTimeSliceResult(SStreamAggSupporter* pAggSup, SSDataBlock* pBlock, SSHashObj* pUpdatedMap,
                                       SArray* pDelWins) {
  int32_t code = TSDB_CODE_SUCCESS;
  int32_t lino = 0;
  int32_t winCode = TSDB_CODE_SUCCESS;

  SColumnInfoData* pGroupCol = taosArrayGet(pBlock->pDataBlock, GROUPID_COLUMN_INDEX);
  uint64_t*        groupIds = (uint64_t*)pGroupCol->pData;
  SColumnInfoData* pCalStartCol = taosArrayGet(pBlock->pDataBlock, CALCULATE_START_TS_COLUMN_INDEX);
  TSKEY*           tsCalStarts = (TSKEY*)pCalStartCol->pData;
  SColumnInfoData* pCalEndCol = taosArrayGet(pBlock->pDataBlock, CALCULATE_END_TS_COLUMN_INDEX);
  TSKEY*           tsCalEnds = (TSKEY*)pCalEndCol->pData;
  for (int32_t i = 0; i < pBlock->info.rows; i++) {
    while (1) {
      TSKEY    ts = tsCalStarts[i];
      TSKEY    endTs = tsCalEnds[i];
      uint64_t groupId = groupIds[i];
      SWinKey  key = {.ts = ts, .groupId = groupId};
      SWinKey  nextKey = {.groupId = groupId};
      code = pAggSup->stateStore.streamStateFillGetNext(pAggSup->pState, &key, &nextKey, NULL, NULL, &winCode);
      QUERY_CHECK_CODE(code, lino, _end);
      if (key.ts > endTs) {
        break;
      }
      (void)tSimpleHashRemove(pUpdatedMap, &key, sizeof(SWinKey));
      void* tmp = taosArrayPush(pDelWins, &key);
      QUERY_CHECK_NULL(tmp, code, lino, _end, terrno);

      pAggSup->stateStore.streamStateDel(pAggSup->pState, &key);
      if (winCode != TSDB_CODE_SUCCESS) {
        break;
      }
      key = nextKey;
    }
  }

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
  return code;
}

static int32_t setAllResultKey(SStreamAggSupporter* pAggSup, TSKEY ts, SSHashObj* pUpdatedMap) {
  int32_t          code = TSDB_CODE_SUCCESS;
  int32_t          lino = 0;
  int64_t          groupId = 0;
  SStreamStateCur* pCur = pAggSup->stateStore.streamStateGroupGetCur(pAggSup->pState);
  int32_t          winCode = pAggSup->stateStore.streamStateGroupGetKVByCur(pCur, &groupId, NULL, NULL);
  if (winCode != TSDB_CODE_SUCCESS) {
    goto _end;
  }
  SWinKey key = {.ts = ts, .groupId = groupId};
  code = saveTimeSliceWinResult(&key, pUpdatedMap);
  QUERY_CHECK_CODE(code, lino, _end);

  pAggSup->stateStore.streamStateGroupCurNext(pCur);

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
  return code;
}

static int32_t doStreamTimeSliceNext(SOperatorInfo* pOperator, SSDataBlock** ppRes) {
  int32_t                       code = TSDB_CODE_SUCCESS;
  int32_t                       lino = 0;
  SStreamTimeSliceOperatorInfo* pInfo = pOperator->info;
  SExecTaskInfo*                pTaskInfo = pOperator->pTaskInfo;
  SStreamAggSupporter*          pAggSup = &pInfo->streamAggSup;

  if (pOperator->status == OP_EXEC_DONE) {
    (*ppRes) = NULL;
    goto _end;
  }

  if (pOperator->status == OP_RES_TO_RETURN) {
    if (hasRemainCalc(pInfo->pFillInfo) ||
        (pInfo->pFillInfo->pos != FILL_POS_INVALID && pInfo->pFillInfo->needFill == true)) {
      blockDataCleanup(pInfo->pRes);
      doStreamFillRange(pInfo->pFillSup, pInfo->pFillInfo, pInfo->pRes);
      if (pInfo->pRes->info.rows > 0) {
        printDataBlock(pInfo->pRes, getStreamOpName(pOperator->operatorType), GET_TASKID(pTaskInfo));
        (*ppRes) = pInfo->pRes;
        goto _end;
      }
    }

    SSDataBlock* resBlock = NULL;
    code = buildTimeSliceResult(pOperator, &resBlock);
    QUERY_CHECK_CODE(code, lino, _end);

    if (resBlock != NULL) {
      (*ppRes) = resBlock;
      goto _end;
    }

    if (pInfo->recvCkBlock) {
      pInfo->recvCkBlock = false;
      printDataBlock(pInfo->pCheckpointRes, getStreamOpName(pOperator->operatorType), GET_TASKID(pTaskInfo));
      (*ppRes) = pInfo->pCheckpointRes;
      goto _end;
    }

    setStreamOperatorCompleted(pOperator);
    resetStreamFillSup(pInfo->pFillSup);
    (*ppRes) = NULL;
    goto _end;
  }

  SSDataBlock*   fillResult = NULL;
  SOperatorInfo* downstream = pOperator->pDownstream[0];
  while (1) {
    SSDataBlock* pBlock = getNextBlockFromDownstream(pOperator, 0);
    if (pBlock == NULL) {
      pOperator->status = OP_RES_TO_RETURN;
      qDebug("===stream===return data:%s. recv datablock num:%" PRIu64, getStreamOpName(pOperator->operatorType),
             pInfo->numOfDatapack);
      pInfo->numOfDatapack = 0;
      break;
    }
    pInfo->numOfDatapack++;
    printSpecDataBlock(pBlock, getStreamOpName(pOperator->operatorType), "recv", GET_TASKID(pTaskInfo));
    setStreamOperatorState(&pInfo->basic, pBlock->info.type);

    switch (pBlock->info.type) {
      case STREAM_DELETE_RESULT: {
        code = doDeleteTimeSliceResult(pAggSup, pBlock, pInfo->pUpdatedMap, pInfo->pDelWins);
        QUERY_CHECK_CODE(code, lino, _end);
      } break;
      case STREAM_NORMAL:
      case STREAM_INVALID: {
        SExprSupp* pExprSup = &pInfo->scalarSup;
        if (pExprSup->pExprInfo != NULL) {
          projectApplyFunctions(pExprSup->pExprInfo, pBlock, pBlock, pExprSup->pCtx, pExprSup->numOfExprs, NULL);
        }
      } break;
      case STREAM_CHECKPOINT: {
        pInfo->recvCkBlock = true;
        pAggSup->stateStore.streamStateCommit(pAggSup->pState);
        doStreamTimeSliceSaveCheckpoint(pOperator);
        pInfo->recvCkBlock = true;
        copyDataBlock(pInfo->pCheckpointRes, pBlock);
        continue;
      } break;
      case STREAM_CREATE_CHILD_TABLE: {
        (*ppRes) = pBlock;
        goto _end;
      } break;
      case STREAM_GET_RESULT: {
        setAllResultKey(pAggSup, pBlock->info.window.skey, pInfo->pUpdatedMap);
        continue;
      }
      default:
        ASSERTS(false, "invalid SSDataBlock type");
    }

    doStreamTimeSliceImpl(pOperator, pBlock);
  }

  if (!pInfo->destHasPrimaryKey) {
    removeDeleteResults(pInfo->pUpdatedMap, pInfo->pDelWins);
  } else {
    copyIntervalDeleteKey(pInfo->pDeletedMap, pInfo->pDelWins);
  }

  void*   pIte = NULL;
  int32_t iter = 0;
  while ((pIte = tSimpleHashIterate(pInfo->pUpdatedMap, pIte, &iter)) != NULL) {
    SWinKey* pKey = (SWinKey*)tSimpleHashGetKey(pIte, NULL);
    void*    tmp = taosArrayPush(pInfo->pUpdated, pKey);
    QUERY_CHECK_NULL(tmp, code, lino, _end, terrno);
  }
  taosArraySort(pInfo->pUpdated, winKeyCmprImpl);

  if (pInfo->isHistoryOp) {
    code = getSliceMaxTsWins(pInfo->pUpdated, pInfo->historyWins);
    QUERY_CHECK_CODE(code, lino, _end);
  }

  initMultiResInfoFromArrayList(&pInfo->groupResInfo, pInfo->pUpdated);
  pInfo->pUpdated = taosArrayInit(16, sizeof(SWinKey));
  QUERY_CHECK_NULL(pInfo->pUpdated, code, lino, _end, terrno);

  code = blockDataEnsureCapacity(pInfo->pRes, pOperator->resultInfo.capacity);
  QUERY_CHECK_CODE(code, lino, _end);

  tSimpleHashCleanup(pInfo->pUpdatedMap);
  _hash_fn_t hashFn = taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY);
  pInfo->pUpdatedMap = tSimpleHashInit(1024, hashFn);

  code = buildTimeSliceResult(pOperator, ppRes);
  QUERY_CHECK_CODE(code, lino, _end);

  if (!(*ppRes)) {
    setStreamOperatorCompleted(pOperator);
    resetStreamFillSup(pInfo->pFillSup);
  }

_end:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
  return code;
}

static SSDataBlock* doStreamTimeSlice(SOperatorInfo* pOperator) {
  SSDataBlock* pRes = NULL;
  (void)doStreamTimeSliceNext(pOperator, &pRes);
  return pRes;
}

static void copyFillValueInfo(SStreamFillSupporter* pFillSup, SStreamFillInfo* pFillInfo) {
  if (pFillInfo->type == TSDB_FILL_SET_VALUE || pFillInfo->type == TSDB_FILL_SET_VALUE_F) {
    int32_t valueIndex = 0;
    for (int32_t i = 0; i < pFillSup->numOfAllCols; ++i) {
      SFillColInfo* pFillCol = pFillSup->pAllColInfo + i;
      if (!isInterpFunc(pFillCol->pExpr)) {
        continue;
      }
      int32_t          srcSlot = pFillCol->pExpr->base.pParam[0].pCol->slotId;
      SResultCellData* pCell = getResultCell(pFillInfo->pResRow, srcSlot);
      SFillColInfo*    pValueCol = pFillSup->pAllColInfo + valueIndex;
      SVariant*        pVar = &(pValueCol->fillVal);
      if (pCell->type == TSDB_DATA_TYPE_FLOAT) {
        float v = 0;
        GET_TYPED_DATA(v, float, pVar->nType, &pVar->i);
        SET_TYPED_DATA(pCell->pData, pCell->type, v);
      } else if (IS_FLOAT_TYPE(pCell->type)) {
        double v = 0;
        GET_TYPED_DATA(v, double, pVar->nType, &pVar->i);
        SET_TYPED_DATA(pCell->pData, pCell->type, v);
      } else if (IS_INTEGER_TYPE(pCell->type)) {
        int64_t v = 0;
        GET_TYPED_DATA(v, int64_t, pVar->nType, &pVar->i);
        SET_TYPED_DATA(pCell->pData, pCell->type, v);
      } else {
        pCell->isNull = true;
      }
      valueIndex++;
    }
  } else if (pFillInfo->type == TSDB_FILL_NULL || pFillInfo->type == TSDB_FILL_NULL_F) {
    for (int32_t i = 0; i < pFillSup->numOfAllCols; ++i) {
      SFillColInfo*    pFillCol = pFillSup->pAllColInfo + i;
      int32_t          slotId = GET_DEST_SLOT_ID(pFillCol);
      SResultCellData* pCell = getResultCell(pFillInfo->pResRow, slotId);
      pCell->isNull = true;
    }
  }
}

int32_t getDownstreamRes(SOperatorInfo* downstream, SSDataBlock** ppRes) {
  if (downstream->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_SCAN) {
    SStreamScanInfo* pInfo = (SStreamScanInfo*)downstream->info;
    *ppRes = pInfo->pRes;
    return TSDB_CODE_SUCCESS;
  } else if (downstream->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_PARTITION) {
    SStreamPartitionOperatorInfo* pInfo = (SStreamPartitionOperatorInfo*)downstream->info;
    *ppRes = pInfo->binfo.pRes;
    return TSDB_CODE_SUCCESS;
  }
  qError("%s failed at line %d since %s", __func__, __LINE__, tstrerror(TSDB_CODE_FAILED));
  return TSDB_CODE_FAILED;
}

int32_t createStreamTimeSliceOperatorInfo(SOperatorInfo* downstream, SPhysiNode* pPhyNode, SExecTaskInfo* pTaskInfo,
                                          SReadHandle* pHandle, SOperatorInfo** ppOptInfo) {
  int32_t                       code = TSDB_CODE_SUCCESS;
  int32_t                       lino = 0;
  SStreamTimeSliceOperatorInfo* pInfo = taosMemoryCalloc(1, sizeof(SStreamTimeSliceOperatorInfo));
  QUERY_CHECK_NULL(pInfo, code, lino, _error, terrno);

  SOperatorInfo* pOperator = taosMemoryCalloc(1, sizeof(SOperatorInfo));
  QUERY_CHECK_NULL(pOperator, code, lino, _error, terrno);

  SStreamInterpFuncPhysiNode* pInterpPhyNode = (SStreamInterpFuncPhysiNode*)pPhyNode;
  pOperator->pTaskInfo = pTaskInfo;
  initResultSizeInfo(&pOperator->resultInfo, 4096);
  SExprSupp* pExpSup = &pOperator->exprSupp;
  int32_t    numOfExprs = 0;
  SExprInfo* pExprInfo = NULL;
  code = createExprInfo(pInterpPhyNode->pFuncs, NULL, &pExprInfo, &numOfExprs);
  QUERY_CHECK_CODE(code, lino, _error);

  code = initExprSupp(pExpSup, pExprInfo, numOfExprs, &pTaskInfo->storageAPI.functionStore);
  QUERY_CHECK_CODE(code, lino, _error);

  if (pInterpPhyNode->pExprs != NULL) {
    int32_t    num = 0;
    SExprInfo* pScalarExprInfo = NULL;
    code = createExprInfo(pInterpPhyNode->pExprs, NULL, &pScalarExprInfo, &num);
    QUERY_CHECK_CODE(code, lino, _error);

    code = initExprSupp(&pInfo->scalarSup, pScalarExprInfo, num, &pTaskInfo->storageAPI.functionStore);
    QUERY_CHECK_CODE(code, lino, _error);
  }

  code = filterInitFromNode((SNode*)pInterpPhyNode->node.pConditions, &pOperator->exprSupp.pFilterInfo, 0);
  QUERY_CHECK_CODE(code, lino, _error);

  pInfo->twAggSup = (STimeWindowAggSupp){
      .waterMark = pInterpPhyNode->streamNodeOption.watermark,
      .calTrigger = pInterpPhyNode->streamNodeOption.triggerType,
      .maxTs = INT64_MIN,
      .minTs = INT64_MAX,
      .deleteMark = getDeleteMarkFromOption(&pInterpPhyNode->streamNodeOption),
  };

  pInfo->primaryTsIndex = ((SColumnNode*)pInterpPhyNode->pTimeSeries)->slotId;

  pInfo->pFillSup = NULL;
  code = initTimeSliceFillSup(pInterpPhyNode, pExpSup, numOfExprs, &pInfo->pFillSup);
  QUERY_CHECK_CODE(code, lino, _error);

  int32_t ratio = 1;
  if (pInfo->pFillSup->type == TSDB_FILL_LINEAR) {
    ratio = 2;
  }

  code =
      initStreamAggSupporter(&pInfo->streamAggSup, pExpSup, numOfExprs, 0, pTaskInfo->streamInfo.pState, sizeof(TSKEY),
                             0, &pTaskInfo->storageAPI.stateStore, pHandle, &pInfo->twAggSup, GET_TASKID(pTaskInfo),
                             &pTaskInfo->storageAPI, pInfo->primaryTsIndex, STREAM_STATE_BUFF_HASH_SORT, ratio);
  QUERY_CHECK_CODE(code, lino, _error);

  initExecTimeWindowInfo(&pInfo->twAggSup.timeWindowData, &pTaskInfo->window);

  pInfo->pRes = createDataBlockFromDescNode(pPhyNode->pOutputDataBlockDesc);
  pInfo->delIndex = 0;
  pInfo->pDelWins = taosArrayInit(4, sizeof(SWinKey));
  QUERY_CHECK_NULL(pInfo->pDelWins, code, lino, _error, terrno);

  pInfo->pDelRes = NULL;
  code = createSpecialDataBlock(STREAM_DELETE_RESULT, &pInfo->pDelRes);
  QUERY_CHECK_CODE(code, lino, _error);

  _hash_fn_t hashFn = taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY);
  pInfo->pDeletedMap = tSimpleHashInit(1024, hashFn);
  QUERY_CHECK_NULL(pInfo->pDeletedMap, code, lino, _error, terrno);

  pInfo->ignoreExpiredData = pInterpPhyNode->streamNodeOption.igExpired;
  pInfo->ignoreExpiredDataSaved = false;
  pInfo->pUpdated = taosArrayInit(64, sizeof(SWinKey));
  pInfo->pUpdatedMap = tSimpleHashInit(1024, hashFn);
  pInfo->historyPoints = taosArrayInit(4, sizeof(SWinKey));
  QUERY_CHECK_NULL(pInfo->historyPoints, code, lino, _error, terrno);

  pInfo->recvCkBlock = false;
  pInfo->pCheckpointRes = NULL;
  code = createSpecialDataBlock(STREAM_CHECKPOINT, &pInfo->pCheckpointRes);
  QUERY_CHECK_CODE(code, lino, _error);

  pInfo->destHasPrimaryKey = pInterpPhyNode->streamNodeOption.destHasPrimaryKey;
  pInfo->numOfDatapack = 0;

  SSDataBlock* pDownRes = NULL;
  code = getDownstreamRes(downstream, &pDownRes);
  QUERY_CHECK_CODE(code, lino, _error);

  pInfo->pFillInfo = initStreamFillInfo(pInfo->pFillSup, pDownRes);
  copyFillValueInfo(pInfo->pFillSup, pInfo->pFillInfo);
  pInfo->ignoreNull = getIgoreNullRes(pExpSup);

  if (pHandle) {
    pInfo->isHistoryOp = pHandle->fillHistory;
  }

  pOperator->operatorType = QUERY_NODE_PHYSICAL_PLAN_STREAM_INTERP_FUNC;
  setOperatorInfo(pOperator, getStreamOpName(pOperator->operatorType), QUERY_NODE_PHYSICAL_PLAN_STREAM_INTERP_FUNC,
                  true, OP_NOT_OPENED, pInfo, pTaskInfo);
  // for stream
  void*   buff = NULL;
  int32_t len = 0;
  int32_t res = pTaskInfo->storageAPI.stateStore.streamStateGetInfo(
      pTaskInfo->streamInfo.pState, STREAM_TIME_SLICE_OP_CHECKPOINT_NAME, strlen(STREAM_TIME_SLICE_OP_CHECKPOINT_NAME),
      &buff, &len);
  if (res == TSDB_CODE_SUCCESS) {
    code = doStreamTimeSliceDecodeOpState(buff, len, pOperator);
    taosMemoryFree(buff);
    QUERY_CHECK_CODE(code, lino, _error);
  }
  pOperator->fpSet = createOperatorFpSet(optrDummyOpenFn, doStreamTimeSlice, NULL, destroyStreamTimeSliceOperatorInfo,
                                         optrDefaultBufFn, NULL, optrDefaultGetNextExtFn, NULL);
  setOperatorStreamStateFn(pOperator, streamTimeSliceReleaseState, streamTimeSliceReloadState);

  if (downstream) {
    if (downstream->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_SCAN) {
      SStreamScanInfo* pScanInfo = downstream->info;
      pScanInfo->igCheckUpdate = true;
    }
    initDownStream(downstream, &pInfo->streamAggSup, pOperator->operatorType, pInfo->primaryTsIndex, &pInfo->twAggSup,
                   &pInfo->basic);
    code = appendDownstream(pOperator, &downstream, 1);
  }
  (*ppOptInfo) = pOperator;
  return code;

_error:
  if (code != TSDB_CODE_SUCCESS) {
    qError("%s failed at line %d since %s", __func__, lino, tstrerror(code));
  }
  if (pInfo != NULL) {
    destroyStreamTimeSliceOperatorInfo(pInfo);
  }
  if (pOperator != NULL) {
    pOperator->info = NULL;
    destroyOperator(pOperator);
  }
  pTaskInfo->code = code;
  (*ppOptInfo) = NULL;
  return code;
}
