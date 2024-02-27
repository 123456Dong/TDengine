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
#include "operator.h"
#include "os.h"
#include "querynodes.h"
#include "querytask.h"
#include "tcompare.h"
#include "tdatablock.h"
#include "thash.h"
#include "tmsg.h"
#include "ttypes.h"
#include "mergejoin.h"



int32_t mWinJoinDumpGrpCache(SMJoinWindowCtx* pCtx) {
  int64_t rowsLeft = pCtx->finBlk->info.capacity - pCtx->finBlk->info.rows;
  SMJoinWinCache* cache = &pCtx->cache;
  int32_t buildGrpNum = taosArrayGetSize(cache->grps);
  int64_t buildTotalRows = TMIN(cache->rowNum, pCtx->jLimit);

  pCtx->finBlk->info.id.groupId = pCtx->seqWinGrp ? pCtx->pJoin->outGrpId : 0;

  if (buildGrpNum <= 0 || buildTotalRows <= 0) {
    MJ_ERR_RET(mJoinNonEqCart((SMJoinCommonCtx*)pCtx, &pCtx->probeGrp, true, pCtx->seqWinGrp));   
    if (pCtx->seqWinGrp) {
      pCtx->pJoin->outGrpId++;
    }
    return TSDB_CODE_SUCCESS;
  }
  
  SMJoinGrpRows* probeGrp = &pCtx->probeGrp;
  int32_t probeRows = GRP_REMAIN_ROWS(probeGrp);
  int32_t probeEndIdx = probeGrp->endIdx;

  if ((!pCtx->seqWinGrp) && 0 == cache->grpIdx && probeRows * buildTotalRows <= rowsLeft) {
    SMJoinGrpRows* pFirstBuild = taosArrayGet(cache->grps, 0);
    if (pFirstBuild->readIdx == pFirstBuild->beginIdx) {
      for (; cache->grpIdx < buildGrpNum; ++cache->grpIdx) {
        SMJoinGrpRows* buildGrp = taosArrayGet(cache->grps, cache->grpIdx);
        MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, buildGrp));
        buildGrp->readIdx = buildGrp->beginIdx;
      }

      cache->grpIdx = 0;
      pCtx->grpRemains = false;
      return TSDB_CODE_SUCCESS;
    }
  }

  for (; !GRP_DONE(probeGrp); ) {
    probeGrp->endIdx = probeGrp->readIdx;
    for (; cache->grpIdx < buildGrpNum && rowsLeft > 0; ++cache->grpIdx) {
      SMJoinGrpRows* buildGrp = taosArrayGet(cache->grps, cache->grpIdx);

      if (rowsLeft >= GRP_REMAIN_ROWS(buildGrp)) {
        MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, buildGrp));
        rowsLeft -= GRP_REMAIN_ROWS(buildGrp);
        buildGrp->readIdx = buildGrp->beginIdx;
        continue;
      }
      
      int32_t buildEndIdx = buildGrp->endIdx;
      buildGrp->endIdx = buildGrp->readIdx + rowsLeft - 1;
      mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, buildGrp);
      buildGrp->readIdx += rowsLeft;
      buildGrp->endIdx = buildEndIdx;
      rowsLeft = 0;
      break;
    }
    probeGrp->endIdx = probeEndIdx;

    if (cache->grpIdx >= buildGrpNum) {
      cache->grpIdx = 0;
      ++probeGrp->readIdx; 
      if (pCtx->seqWinGrp) {
        pCtx->pJoin->outGrpId++;
        break;
      }
    }

    if (rowsLeft <= 0) {
      break;
    }
  }

  probeGrp->endIdx = probeEndIdx;        

  pCtx->grpRemains = probeGrp->readIdx <= probeGrp->endIdx;
  
  return TSDB_CODE_SUCCESS;  
}


static int32_t mOuterJoinHashFullCart(SMJoinMergeCtx* pCtx) {
  SMJoinTableCtx* probe = pCtx->pJoin->probe;
  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinGrpRows* probeGrp = taosArrayGet(probe->eqGrps, probe->grpIdx);

  if (build->grpRowIdx >= 0) {
    bool contLoop = mJoinHashGrpCart(pCtx->finBlk, probeGrp, true, probe, build);
    if (build->grpRowIdx < 0) {
      probeGrp->readIdx++;
    }
    
    if (!contLoop) {
      goto _return;
    }
  }

  size_t bufLen = 0;
  int32_t probeEndIdx = probeGrp->endIdx;
  for (; !GRP_DONE(probeGrp) && !BLK_IS_FULL(pCtx->finBlk); ++probeGrp->readIdx) {
    if (mJoinCopyKeyColsDataToBuf(probe, probeGrp->readIdx, &bufLen)) {
      probeGrp->endIdx = probeGrp->readIdx;
      MJ_ERR_RET(mJoinNonEqGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, true));
      probeGrp->endIdx = probeEndIdx;
      continue;
    }

    void* pGrp = tSimpleHashGet(build->pGrpHash, probe->keyData, bufLen);
    if (NULL == pGrp) {
      probeGrp->endIdx = probeGrp->readIdx;
      MJ_ERR_RET(mJoinNonEqGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, true));
      probeGrp->endIdx = probeEndIdx;
      continue;
    }

    if (build->rowBitmapSize > 0) {
      build->pHashCurGrp = ((SMJoinHashGrpRows*)pGrp)->pRows;
      build->pHashGrpRows = pGrp;
      build->pHashGrpRows->allRowsMatch = true;
    } else {
      build->pHashCurGrp = *(SArray**)pGrp;
    }
    
    build->grpRowIdx = 0;
    bool contLoop = mJoinHashGrpCart(pCtx->finBlk, probeGrp, true, probe, build);
    if (!contLoop) {
      if (build->grpRowIdx < 0) {
        probeGrp->readIdx++;
      }
      goto _return;
    }  
  }

_return:

  pCtx->grpRemains = probeGrp->readIdx <= probeGrp->endIdx;

  return TSDB_CODE_SUCCESS;
}


static int32_t mOuterJoinMergeFullCart(SMJoinMergeCtx* pCtx) {
  int32_t rowsLeft = pCtx->finBlk->info.capacity - pCtx->finBlk->info.rows;
  SMJoinTableCtx* probe = pCtx->pJoin->probe;
  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinGrpRows* probeGrp = taosArrayGet(probe->eqGrps, 0);
  int32_t buildGrpNum = taosArrayGetSize(build->eqGrps);
  int32_t probeRows = GRP_REMAIN_ROWS(probeGrp);
  int32_t probeEndIdx = probeGrp->endIdx;

  if (0 == build->grpIdx && probeRows * build->grpTotalRows <= rowsLeft) {
    SMJoinGrpRows* pFirstBuild = taosArrayGet(build->eqGrps, 0);
    if (pFirstBuild->readIdx == pFirstBuild->beginIdx) {
      for (; build->grpIdx < buildGrpNum; ++build->grpIdx) {
        SMJoinGrpRows* buildGrp = taosArrayGet(build->eqGrps, build->grpIdx);
        MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, buildGrp));
        buildGrp->readIdx = buildGrp->beginIdx;
      }

      pCtx->grpRemains = false;
      return TSDB_CODE_SUCCESS;
    }
  }

  for (; !GRP_DONE(probeGrp); ) {
    probeGrp->endIdx = probeGrp->readIdx;
    for (; build->grpIdx < buildGrpNum && rowsLeft > 0; ++build->grpIdx) {
      SMJoinGrpRows* buildGrp = taosArrayGet(build->eqGrps, build->grpIdx);

      if (rowsLeft >= GRP_REMAIN_ROWS(buildGrp)) {
        MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, buildGrp));
        rowsLeft -= GRP_REMAIN_ROWS(buildGrp);
        buildGrp->readIdx = buildGrp->beginIdx;
        continue;
      }
      
      int32_t buildEndIdx = buildGrp->endIdx;
      buildGrp->endIdx = buildGrp->readIdx + rowsLeft - 1;
      mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, buildGrp);
      buildGrp->readIdx += rowsLeft;
      buildGrp->endIdx = buildEndIdx;
      rowsLeft = 0;
      break;
    }
    probeGrp->endIdx = probeEndIdx;

    if (build->grpIdx >= buildGrpNum) {
      build->grpIdx = 0;
      ++probeGrp->readIdx; 
    }

    if (rowsLeft <= 0) {
      break;
    }
  }

  probeGrp->endIdx = probeEndIdx;        

  pCtx->grpRemains = probeGrp->readIdx <= probeGrp->endIdx;
  
  return TSDB_CODE_SUCCESS;  
}

static int32_t mOuterJoinMergeSeqCart(SMJoinMergeCtx* pCtx) {
  SMJoinTableCtx* probe = pCtx->pJoin->probe;
  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinGrpRows* probeGrp = taosArrayGet(probe->eqGrps, probe->grpIdx);
  int32_t buildGrpNum = taosArrayGetSize(build->eqGrps);
  int32_t probeEndIdx = probeGrp->endIdx;
  int32_t rowsLeft = pCtx->midBlk->info.capacity;  
  bool contLoop = true;
  int32_t startGrpIdx = 0;
  int32_t startRowIdx = -1;

  blockDataCleanup(pCtx->midBlk);

  do {
    for (; !GRP_DONE(probeGrp) && !BLK_IS_FULL(pCtx->finBlk); 
      ++probeGrp->readIdx, probeGrp->readMatch = false, probeGrp->endIdx = probeEndIdx, build->grpIdx = 0) {
      probeGrp->endIdx = probeGrp->readIdx;
      
      rowsLeft = pCtx->midBlk->info.capacity;
      startGrpIdx = build->grpIdx;
      startRowIdx = -1;
      
      for (; build->grpIdx < buildGrpNum && rowsLeft > 0; ++build->grpIdx) {
        SMJoinGrpRows* buildGrp = taosArrayGet(build->eqGrps, build->grpIdx);
        if (startRowIdx < 0) {
          startRowIdx = buildGrp->readIdx;
        }

        if (rowsLeft >= GRP_REMAIN_ROWS(buildGrp)) {
          MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->midBlk, true, probeGrp, buildGrp));
          rowsLeft -= GRP_REMAIN_ROWS(buildGrp);
          buildGrp->readIdx = buildGrp->beginIdx;
          continue;
        }
        
        int32_t buildEndIdx = buildGrp->endIdx;
        buildGrp->endIdx = buildGrp->readIdx + rowsLeft - 1;
        ASSERT(buildGrp->endIdx >= buildGrp->readIdx);
        MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->midBlk, true, probeGrp, buildGrp));
        buildGrp->readIdx += rowsLeft;
        buildGrp->endIdx = buildEndIdx;
        rowsLeft = 0;
        break;
      }

      if (pCtx->midBlk->info.rows > 0) {
        if (build->rowBitmapSize > 0) {
          MJ_ERR_RET(mJoinFilterAndMarkRows(pCtx->midBlk, pCtx->pJoin->pFPreFilter, build, startGrpIdx, startRowIdx));
        } else {
          MJ_ERR_RET(doFilter(pCtx->midBlk, pCtx->pJoin->pFPreFilter, NULL));
        }

        if (pCtx->midBlk->info.rows > 0) {
          probeGrp->readMatch = true;
        }
      } 

      if (0 == pCtx->midBlk->info.rows) {
        if (build->grpIdx == buildGrpNum) {
          if (!probeGrp->readMatch) {
            MJ_ERR_RET(mJoinNonEqGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, true));
          }

          continue;
        }
      } else {
        MJ_ERR_RET(mJoinCopyMergeMidBlk(pCtx, &pCtx->midBlk, &pCtx->finBlk));
        
        if (pCtx->midRemains) {
          contLoop = false;
        } else if (build->grpIdx == buildGrpNum) {
          continue;
        }
      }

      //need break

      probeGrp->endIdx = probeEndIdx;
      
      if (build->grpIdx >= buildGrpNum) {
        build->grpIdx = 0;
        ++probeGrp->readIdx;
        probeGrp->readMatch = false;
      }      

      break;
    }

    if (GRP_DONE(probeGrp) || BLK_IS_FULL(pCtx->finBlk)) {
      break;
    }
  } while (contLoop);

  pCtx->grpRemains = probeGrp->readIdx <= probeGrp->endIdx;

  return TSDB_CODE_SUCCESS;
}

static int32_t mOuterJoinHashGrpCartFilter(SMJoinMergeCtx* pCtx, bool* contLoop) {
  SMJoinTableCtx* probe = pCtx->pJoin->probe;
  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinGrpRows* probeGrp = taosArrayGet(probe->eqGrps, probe->grpIdx);
  int32_t startRowIdx = 0;
  
  blockDataCleanup(pCtx->midBlk);

  do {
    startRowIdx = build->grpRowIdx;
    mJoinHashGrpCart(pCtx->midBlk, probeGrp, true, probe, build);

    if (pCtx->midBlk->info.rows > 0) {
      if (build->rowBitmapSize > 0) {
        MJ_ERR_RET(mJoinFilterAndMarkHashRows(pCtx->midBlk, pCtx->pJoin->pPreFilter, build, startRowIdx));
      } else {
        MJ_ERR_RET(doFilter(pCtx->midBlk, pCtx->pJoin->pPreFilter, NULL));
      }
      if (pCtx->midBlk->info.rows > 0) {
        probeGrp->readMatch = true;
      }
    } 

    if (0 == pCtx->midBlk->info.rows) {
      if (build->grpRowIdx < 0) {
        if (!probeGrp->readMatch) {
          MJ_ERR_RET(mJoinNonEqGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, true));
        }

        break;
      }
      
      continue;
    } else {
      MJ_ERR_RET(mJoinCopyMergeMidBlk(pCtx, &pCtx->midBlk, &pCtx->finBlk));
      
      if (pCtx->midRemains) {
        pCtx->grpRemains = probeGrp->readIdx <= probeGrp->endIdx;
        *contLoop = false;
        return TSDB_CODE_SUCCESS;
      }

      if (build->grpRowIdx < 0) {
        break;
      }

      continue;
    }
  } while (true);

  *contLoop = true;
  return TSDB_CODE_SUCCESS;
}


static int32_t mOuterJoinHashSeqCart(SMJoinMergeCtx* pCtx) {
  SMJoinTableCtx* probe = pCtx->pJoin->probe;
  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinGrpRows* probeGrp = taosArrayGet(probe->eqGrps, 0);
  bool contLoop = false;

  if (build->grpRowIdx >= 0) {
    MJ_ERR_RET(mOuterJoinHashGrpCartFilter(pCtx, &contLoop));
    if (build->grpRowIdx < 0) {
      probeGrp->readIdx++;
      probeGrp->readMatch = false;
    }

    if (!contLoop) {
      goto _return;
    }
  }

  size_t bufLen = 0;
  int32_t probeEndIdx = probeGrp->endIdx;
  for (; !GRP_DONE(probeGrp) && !BLK_IS_FULL(pCtx->finBlk);) {
    if (mJoinCopyKeyColsDataToBuf(probe, probeGrp->readIdx, &bufLen)) {
      probeGrp->endIdx = probeGrp->readIdx;
      MJ_ERR_RET(mJoinNonEqGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, true));
      probeGrp->endIdx = probeEndIdx;
      probeGrp->readIdx++;
      probeGrp->readMatch = false;
      continue;
    }

    void* pGrp = tSimpleHashGet(build->pGrpHash, probe->keyData, bufLen);
    if (NULL == pGrp) {
      probeGrp->endIdx = probeGrp->readIdx;
      MJ_ERR_RET(mJoinNonEqGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, true));
      probeGrp->endIdx = probeEndIdx;
      probeGrp->readIdx++;
      probeGrp->readMatch = false;
      continue;
    }

    if (build->rowBitmapSize > 0) {
      build->pHashCurGrp = ((SMJoinHashGrpRows*)pGrp)->pRows;
      build->pHashGrpRows = pGrp;
      if (0 == build->pHashGrpRows->rowBitmapOffset) {
        MJ_ERR_RET(mJoinGetRowBitmapOffset(build, taosArrayGetSize(build->pHashCurGrp), &build->pHashGrpRows->rowBitmapOffset));
      }
    } else {
      build->pHashCurGrp = *(SArray**)pGrp;
    }
    
    build->grpRowIdx = 0;

    probeGrp->endIdx = probeGrp->readIdx;      
    MJ_ERR_RET(mOuterJoinHashGrpCartFilter(pCtx, &contLoop));
    probeGrp->endIdx = probeEndIdx;
    if (build->grpRowIdx < 0) {
      probeGrp->readIdx++;
      probeGrp->readMatch = false;
    }

    if (!contLoop) {
      break;
    }
  }

_return:

  pCtx->grpRemains = probeGrp->readIdx <= probeGrp->endIdx;

  return TSDB_CODE_SUCCESS;
}


static int32_t mLeftJoinMergeCart(SMJoinMergeCtx* pCtx) {
  return (NULL == pCtx->pJoin->pFPreFilter) ? mOuterJoinMergeFullCart(pCtx) : mOuterJoinMergeSeqCart(pCtx);
}



static bool mLeftJoinRetrieve(SOperatorInfo* pOperator, SMJoinOperatorInfo* pJoin, SMJoinMergeCtx* pCtx) {
  bool probeGot = mJoinRetrieveBlk(pJoin, &pJoin->probe->blkRowIdx, &pJoin->probe->blk, pJoin->probe);
  bool buildGot = false;

  do {
    if (probeGot || MJOIN_DS_NEED_INIT(pOperator, pJoin->build)) {  
      buildGot = mJoinRetrieveBlk(pJoin, &pJoin->build->blkRowIdx, &pJoin->build->blk, pJoin->build);
    }
    
    if (!probeGot) {
      if (!pCtx->groupJoin || NULL == pJoin->probe->remainInBlk) {
        mJoinSetDone(pOperator);
      }

      return false;
    }

    if (buildGot) {
      SColumnInfoData* pProbeCol = taosArrayGet(pJoin->probe->blk->pDataBlock, pJoin->probe->primCol->srcSlot);
      SColumnInfoData* pBuildCol = taosArrayGet(pJoin->build->blk->pDataBlock, pJoin->build->primCol->srcSlot);
      if (*((int64_t*)pProbeCol->pData + pJoin->probe->blkRowIdx) > *((int64_t*)pBuildCol->pData + pJoin->build->blk->info.rows - 1)) {
        pJoin->build->blkRowIdx = pJoin->build->blk->info.rows;
        continue;
      }
    }
    
    break;
  } while (true);

  return true;
}

static int32_t mLeftJoinHashCart(SMJoinMergeCtx* pCtx) {
  return (NULL == pCtx->pJoin->pPreFilter) ? mOuterJoinHashFullCart(pCtx) : mOuterJoinHashSeqCart(pCtx);
}

static FORCE_INLINE int32_t mLeftJoinHandleGrpRemains(SMJoinMergeCtx* pCtx) {
  if (pCtx->lastEqGrp) {
    return (pCtx->hashJoin) ? (*pCtx->hashCartFp)(pCtx) : (*pCtx->mergeCartFp)(pCtx);
  }
  
  return mJoinNonEqCart((SMJoinCommonCtx*)pCtx, &pCtx->probeNEqGrp, true, false);
}

SSDataBlock* mLeftJoinDo(struct SOperatorInfo* pOperator) {
  SMJoinOperatorInfo* pJoin = pOperator->info;
  SMJoinMergeCtx* pCtx = &pJoin->ctx.mergeCtx;
  int32_t code = TSDB_CODE_SUCCESS;
  int64_t probeTs = 0;
  int64_t buildTs = 0;
  SColumnInfoData* pBuildCol = NULL;
  SColumnInfoData* pProbeCol = NULL;

  blockDataCleanup(pCtx->finBlk);

  if (pCtx->midRemains) {
    MJ_ERR_JRET(mJoinHandleMidRemains(pCtx));
    if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
      return pCtx->finBlk;
    }
    pCtx->midRemains = false;
  }

  if (pCtx->grpRemains) {
    MJ_ERR_JRET(mLeftJoinHandleGrpRemains(pCtx));
    if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
      return pCtx->finBlk;
    }
    pCtx->grpRemains = false;
  }

  do {
    if (!mLeftJoinRetrieve(pOperator, pJoin, pCtx)) {
      if (pCtx->groupJoin && pCtx->finBlk->info.rows <= 0 && !mJoinIsDone(pOperator)) {
        continue;
      }

      break;
    }

    MJOIN_GET_TB_COL_TS(pBuildCol, buildTs, pJoin->build);
    MJOIN_GET_TB_COL_TS(pProbeCol, probeTs, pJoin->probe);
    
    if (probeTs == pCtx->lastEqTs) {
      MJ_ERR_JRET(mJoinProcessEqualGrp(pCtx, probeTs, true));
      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }

      if (MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe)) {
        continue;
      } else {
        MJOIN_GET_TB_CUR_TS(pProbeCol, probeTs, pJoin->probe);
      }
    }

    while (!MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe) && !MJOIN_BUILD_TB_ROWS_DONE(pJoin->build)) {
      if (probeTs == buildTs) {
        pCtx->lastEqTs = probeTs;
        MJ_ERR_JRET(mJoinProcessEqualGrp(pCtx, probeTs, false));
        if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
          return pCtx->finBlk;
        }

        MJOIN_GET_TB_COL_TS(pBuildCol, buildTs, pJoin->build);
        MJOIN_GET_TB_COL_TS(pProbeCol, probeTs, pJoin->probe);
      } else if (PROBE_TS_LOWER(pCtx->ascTs, probeTs, buildTs)) {
        MJ_ERR_JRET(mJoinProcessLowerGrp(pCtx, pJoin->probe, pProbeCol, &probeTs, &buildTs));
        if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
          return pCtx->finBlk;
        }
      } else {
        while (++pJoin->build->blkRowIdx < pJoin->build->blk->info.rows) {
          MJOIN_GET_TB_CUR_TS(pBuildCol, buildTs, pJoin->build);
          if (PROBE_TS_GREATER(pCtx->ascTs, probeTs, buildTs)) {
            continue;
          }
          
          break;
        }
      }
    }

    if (!MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe) && (pJoin->build->dsFetchDone || (pCtx->groupJoin && NULL == pJoin->build->blk))) {
      pCtx->probeNEqGrp.blk = pJoin->probe->blk;
      pCtx->probeNEqGrp.beginIdx = pJoin->probe->blkRowIdx;
      pCtx->probeNEqGrp.readIdx = pCtx->probeNEqGrp.beginIdx;
      pCtx->probeNEqGrp.endIdx = pJoin->probe->blk->info.rows - 1;
      
      pJoin->probe->blkRowIdx = pJoin->probe->blk->info.rows;
            
      MJ_ERR_JRET(mJoinNonEqCart((SMJoinCommonCtx*)pCtx, &pCtx->probeNEqGrp, true, false));
      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }
    }
  } while (true);

_return:

  if (code) {
    pJoin->errCode = code;
    return NULL;
  }

  return pCtx->finBlk;
}

void mLeftJoinGroupReset(SMJoinOperatorInfo* pJoin) {
  SMJoinMergeCtx* pCtx = &pJoin->ctx.mergeCtx;

  pCtx->lastEqGrp = false;
  pCtx->lastProbeGrp = false;
  pCtx->hashCan = false;
  pCtx->midRemains = false;
  pCtx->lastEqTs = INT64_MIN;

  mJoinResetGroupTableCtx(pJoin->probe);
  mJoinResetGroupTableCtx(pJoin->build);    
}


static int32_t mInnerJoinMergeCart(SMJoinMergeCtx* pCtx) {
  int32_t rowsLeft = pCtx->finBlk->info.capacity - pCtx->finBlk->info.rows;
  SMJoinTableCtx* probe = pCtx->pJoin->probe;
  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinGrpRows* probeGrp = taosArrayGet(probe->eqGrps, 0);
  int32_t buildGrpNum = taosArrayGetSize(build->eqGrps);
  int32_t probeRows = GRP_REMAIN_ROWS(probeGrp);
  int32_t probeEndIdx = probeGrp->endIdx;

  if (0 == build->grpIdx && probeRows * build->grpTotalRows <= rowsLeft) {
    SMJoinGrpRows* pFirstBuild = taosArrayGet(build->eqGrps, 0);
    if (pFirstBuild->readIdx == pFirstBuild->beginIdx) {
      for (; build->grpIdx < buildGrpNum; ++build->grpIdx) {
        SMJoinGrpRows* buildGrp = taosArrayGet(build->eqGrps, build->grpIdx);
        MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, buildGrp));
        buildGrp->readIdx = buildGrp->beginIdx;
      }

      pCtx->grpRemains = false;
      return TSDB_CODE_SUCCESS;
    }
  }

  for (; !GRP_DONE(probeGrp); ) {
    probeGrp->endIdx = probeGrp->readIdx;
    for (; build->grpIdx < buildGrpNum && rowsLeft > 0; ++build->grpIdx) {
      SMJoinGrpRows* buildGrp = taosArrayGet(build->eqGrps, build->grpIdx);

      if (rowsLeft >= GRP_REMAIN_ROWS(buildGrp)) {
        MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, buildGrp));
        rowsLeft -= GRP_REMAIN_ROWS(buildGrp);
        buildGrp->readIdx = buildGrp->beginIdx;
        continue;
      }
      
      int32_t buildEndIdx = buildGrp->endIdx;
      buildGrp->endIdx = buildGrp->readIdx + rowsLeft - 1;
      mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, buildGrp);
      buildGrp->readIdx += rowsLeft;
      buildGrp->endIdx = buildEndIdx;
      rowsLeft = 0;
      break;
    }
    probeGrp->endIdx = probeEndIdx;

    if (build->grpIdx >= buildGrpNum) {
      build->grpIdx = 0;
      ++probeGrp->readIdx; 
    }

    if (rowsLeft <= 0) {
      break;
    }
  }

  probeGrp->endIdx = probeEndIdx;        

  pCtx->grpRemains = probeGrp->readIdx <= probeGrp->endIdx;
  
  return TSDB_CODE_SUCCESS;  
}


static int32_t mInnerJoinHashCart(SMJoinMergeCtx* pCtx) {
  SMJoinTableCtx* probe = pCtx->pJoin->probe;
  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinGrpRows* probeGrp = taosArrayGet(probe->eqGrps, probe->grpIdx);

  if (build->grpRowIdx >= 0) {
    bool contLoop = mJoinHashGrpCart(pCtx->finBlk, probeGrp, true, probe, build);
    if (build->grpRowIdx < 0) {
      probeGrp->readIdx++;
    }
    
    if (!contLoop) {
      goto _return;
    }
  }

  size_t bufLen = 0;
  int32_t probeEndIdx = probeGrp->endIdx;
  for (; !GRP_DONE(probeGrp) && !BLK_IS_FULL(pCtx->finBlk); ++probeGrp->readIdx) {
    if (mJoinCopyKeyColsDataToBuf(probe, probeGrp->readIdx, &bufLen)) {
      continue;
    }

    SArray** pGrp = tSimpleHashGet(build->pGrpHash, probe->keyData, bufLen);
    if (NULL != pGrp) {
      build->pHashCurGrp = *pGrp;
      build->grpRowIdx = 0;
      bool contLoop = mJoinHashGrpCart(pCtx->finBlk, probeGrp, true, probe, build);
      if (!contLoop) {
        if (build->grpRowIdx < 0) {
          probeGrp->readIdx++;
        }
        goto _return;
      }  
    }
  }

_return:

  pCtx->grpRemains = probeGrp->readIdx <= probeGrp->endIdx;

  return TSDB_CODE_SUCCESS;
}

static FORCE_INLINE int32_t mInnerJoinHandleGrpRemains(SMJoinMergeCtx* pCtx) {
  return (pCtx->hashJoin) ? (*pCtx->hashCartFp)(pCtx) : (*pCtx->mergeCartFp)(pCtx);
}


static bool mInnerJoinRetrieve(SOperatorInfo* pOperator, SMJoinOperatorInfo* pJoin) {
  bool probeGot = mJoinRetrieveBlk(pJoin, &pJoin->probe->blkRowIdx, &pJoin->probe->blk, pJoin->probe);
  bool buildGot = false;

  if (probeGot || MJOIN_DS_NEED_INIT(pOperator, pJoin->build)) {  
    buildGot = mJoinRetrieveBlk(pJoin, &pJoin->build->blkRowIdx, &pJoin->build->blk, pJoin->build);
  }
  
  if (!probeGot) {
    mJoinSetDone(pOperator);
    return false;
  }

  return true;
}


SSDataBlock* mInnerJoinDo(struct SOperatorInfo* pOperator) {
  SMJoinOperatorInfo* pJoin = pOperator->info;
  SMJoinMergeCtx* pCtx = &pJoin->ctx.mergeCtx;
  int32_t code = TSDB_CODE_SUCCESS;
  int64_t probeTs = 0;
  int64_t buildTs = 0;
  SColumnInfoData* pBuildCol = NULL;
  SColumnInfoData* pProbeCol = NULL;

  blockDataCleanup(pCtx->finBlk);

  if (pCtx->grpRemains) {
    MJ_ERR_JRET(mInnerJoinHandleGrpRemains(pCtx));
    if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
      return pCtx->finBlk;
    }
    pCtx->grpRemains = false;
  }

  do {
    if (!mInnerJoinRetrieve(pOperator, pJoin)) {
      break;
    }

    MJOIN_GET_TB_COL_TS(pBuildCol, buildTs, pJoin->build);
    MJOIN_GET_TB_COL_TS(pProbeCol, probeTs, pJoin->probe);
    
    if (probeTs == pCtx->lastEqTs) {
      MJ_ERR_JRET(mJoinProcessEqualGrp(pCtx, probeTs, true));
      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }

      if (MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe) || MJOIN_BUILD_TB_ROWS_DONE(pJoin->build)) {
        continue;
      } 

      MJOIN_GET_TB_CUR_TS(pProbeCol, probeTs, pJoin->probe);
    } else if (MJOIN_BUILD_TB_ROWS_DONE(pJoin->build)) {
      mJoinSetDone(pOperator);
      break;
    }

    do {
      if (probeTs == buildTs) {
        pCtx->lastEqTs = probeTs;
        MJ_ERR_JRET(mJoinProcessEqualGrp(pCtx, probeTs, false));
        if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
          return pCtx->finBlk;
        }

        if (MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe) || MJOIN_BUILD_TB_ROWS_DONE(pJoin->build)) {
          break;
        }
        
        MJOIN_GET_TB_COL_TS(pBuildCol, buildTs, pJoin->build);
        MJOIN_GET_TB_COL_TS(pProbeCol, probeTs, pJoin->probe);
        continue;
      }

      if (PROBE_TS_LOWER(pCtx->ascTs, probeTs, buildTs)) {
        if (++pJoin->probe->blkRowIdx < pJoin->probe->blk->info.rows) {
          MJOIN_GET_TB_CUR_TS(pProbeCol, probeTs, pJoin->probe);
          continue;
        }
      } else {
        if (++pJoin->build->blkRowIdx < pJoin->build->blk->info.rows) {
          MJOIN_GET_TB_CUR_TS(pBuildCol, buildTs, pJoin->build);
          continue;
        }
      }
      
      break;
    } while (true);
  } while (true);

_return:

  if (code) {
    pJoin->errCode = code;
    return NULL;
  }

  return pCtx->finBlk;
}

static FORCE_INLINE int32_t mFullJoinHandleGrpRemains(SMJoinMergeCtx* pCtx) {
  if (pCtx->lastEqGrp) {
    return (pCtx->hashJoin) ? (*pCtx->hashCartFp)(pCtx) : (*pCtx->mergeCartFp)(pCtx);
  }
  
  return pCtx->lastProbeGrp ? mJoinNonEqCart((SMJoinCommonCtx*)pCtx, &pCtx->probeNEqGrp, true, false) : mJoinNonEqCart((SMJoinCommonCtx*)pCtx, &pCtx->buildNEqGrp, false, false);
}

static bool mFullJoinRetrieve(SOperatorInfo* pOperator, SMJoinOperatorInfo* pJoin) {
  bool probeGot = mJoinRetrieveBlk(pJoin, &pJoin->probe->blkRowIdx, &pJoin->probe->blk, pJoin->probe);
  bool buildGot = mJoinRetrieveBlk(pJoin, &pJoin->build->blkRowIdx, &pJoin->build->blk, pJoin->build);
  
  if (!probeGot && !buildGot) {
    return false;
  }

  return true;
}

static FORCE_INLINE int32_t mFullJoinHashCart(SMJoinMergeCtx* pCtx) {
  return (NULL == pCtx->pJoin->pPreFilter) ? mOuterJoinHashFullCart(pCtx) : mOuterJoinHashSeqCart(pCtx);
}

static int32_t mFullJoinMergeCart(SMJoinMergeCtx* pCtx) {
  return (NULL == pCtx->pJoin->pFPreFilter) ? mOuterJoinMergeFullCart(pCtx) : mOuterJoinMergeSeqCart(pCtx);
}

static FORCE_INLINE int32_t mFullJoinOutputHashRow(SMJoinMergeCtx* pCtx, SMJoinHashGrpRows* pGrpRows, int32_t idx) {
  SMJoinGrpRows grp = {0};
  SMJoinRowPos* pPos = taosArrayGet(pGrpRows->pRows, idx);
  grp.blk = pPos->pBlk;
  grp.readIdx = pPos->pos;
  grp.endIdx = pPos->pos;
  return mJoinNonEqGrpCart(pCtx->pJoin, pCtx->finBlk, true, &grp, false);
}

static int32_t mFullJoinOutputHashGrpRows(SMJoinMergeCtx* pCtx, SMJoinHashGrpRows* pGrpRows, SMJoinNMatchCtx* pNMatch, bool* grpDone) {
  int32_t rowNum = taosArrayGetSize(pGrpRows->pRows);
  for (; pNMatch->rowIdx < rowNum && !BLK_IS_FULL(pCtx->finBlk); ++pNMatch->rowIdx) {
    MJ_ERR_RET(mFullJoinOutputHashRow(pCtx, pGrpRows, pNMatch->rowIdx));
  }

  if (pNMatch->rowIdx >= rowNum) {
    *grpDone = true;
    pNMatch->rowIdx = 0;
  }
  
  return TSDB_CODE_SUCCESS;
}

static int32_t mFullJoinHandleHashGrpRemains(SMJoinMergeCtx* pCtx) {
  static const uint8_t lowest_bit_bitmap[] = {32, 7, 6, 32, 5, 3, 32, 0, 4, 1, 2};
  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinNMatchCtx* pNMatch = &build->nMatchCtx;
  if (NULL == pNMatch->pGrp) {
    pNMatch->pGrp = tSimpleHashIterate(build->pGrpHash, pNMatch->pGrp, &pNMatch->iter);
    pNMatch->bitIdx = 0;
  }

  int32_t baseIdx = 0;
  while (NULL != pNMatch->pGrp) {
    SMJoinHashGrpRows* pGrpRows = (SMJoinHashGrpRows*)pNMatch->pGrp;
    if (pGrpRows->allRowsMatch) {
      pNMatch->pGrp = tSimpleHashIterate(build->pGrpHash, pNMatch->pGrp, &pNMatch->iter);
      pNMatch->bitIdx = 0;
      continue;
    }
  
    if (pGrpRows->rowMatchNum <= 0 || pGrpRows->allRowsNMatch) {
      pGrpRows->allRowsNMatch = true;

      bool grpDone = false;      
      MJ_ERR_RET(mFullJoinOutputHashGrpRows(pCtx, pGrpRows, pNMatch, &grpDone));
      if (BLK_IS_FULL(pCtx->finBlk)) {
        if (grpDone) {
          pNMatch->pGrp = tSimpleHashIterate(build->pGrpHash, pNMatch->pGrp, &pNMatch->iter);
          pNMatch->bitIdx = 0;      
        }
        
        pCtx->nmatchRemains = true;
        return TSDB_CODE_SUCCESS;
      }

      pNMatch->pGrp = tSimpleHashIterate(build->pGrpHash, pNMatch->pGrp, &pNMatch->iter);
      pNMatch->bitIdx = 0;      
      continue;
    }

    int32_t grpRowNum = taosArrayGetSize(pGrpRows->pRows);
    int32_t bitBytes = BitmapLen(grpRowNum);
    for (; pNMatch->bitIdx < bitBytes; ++pNMatch->bitIdx) {
      if (0 == build->pRowBitmap[pGrpRows->rowBitmapOffset + pNMatch->bitIdx]) {
        continue;
      }

      baseIdx = 8 * pNMatch->bitIdx;
      char *v = &build->pRowBitmap[pGrpRows->rowBitmapOffset + pNMatch->bitIdx];
      while (*v && !BLK_IS_FULL(pCtx->finBlk)) {
        uint8_t n = lowest_bit_bitmap[((*v & (*v - 1)) ^ *v) % 11];
        if (baseIdx + n >= grpRowNum) {
          MJOIN_SET_ROW_BITMAP(build->pRowBitmap, pGrpRows->rowBitmapOffset + pNMatch->bitIdx, n);
          continue;
        }

        MJ_ERR_RET(mFullJoinOutputHashRow(pCtx, pGrpRows, baseIdx + n));
        MJOIN_SET_ROW_BITMAP(build->pRowBitmap, pGrpRows->rowBitmapOffset + pNMatch->bitIdx, n);
        if (++pGrpRows->rowMatchNum == taosArrayGetSize(pGrpRows->pRows)) {
          pGrpRows->allRowsMatch = true;
          pNMatch->bitIdx = bitBytes;
          break;
        }
      }
  
      if (BLK_IS_FULL(pCtx->finBlk)) {
        if (pNMatch->bitIdx == bitBytes) {
          pNMatch->pGrp = tSimpleHashIterate(build->pGrpHash, pNMatch->pGrp, &pNMatch->iter);
          pNMatch->bitIdx = 0;      
        }

        pCtx->nmatchRemains = true;
        return TSDB_CODE_SUCCESS;
      }
    }

    pNMatch->pGrp = tSimpleHashIterate(build->pGrpHash, pNMatch->pGrp, &pNMatch->iter);
    pNMatch->bitIdx = 0;
  }
  
  pCtx->nmatchRemains = false;
  pCtx->lastEqGrp = false;
  
  return TSDB_CODE_SUCCESS;
}

static FORCE_INLINE int32_t mFullJoinOutputMergeRow(SMJoinMergeCtx* pCtx, SMJoinGrpRows* pGrpRows, int32_t idx) {
  SMJoinGrpRows grp = {0};
  grp.blk = pGrpRows->blk;
  grp.readIdx = idx;
  grp.endIdx = idx;
  return mJoinNonEqGrpCart(pCtx->pJoin, pCtx->finBlk, true, &grp, false);
}


static int32_t mFullJoinOutputMergeGrpRows(SMJoinMergeCtx* pCtx, SMJoinGrpRows* pGrpRows, SMJoinNMatchCtx* pNMatch, bool* grpDone) {
  for (; pNMatch->rowIdx <= pGrpRows->endIdx && !BLK_IS_FULL(pCtx->finBlk); ++pNMatch->rowIdx) {
    MJ_ERR_RET(mFullJoinOutputMergeRow(pCtx, pGrpRows, pNMatch->rowIdx));
  }

  if (pNMatch->rowIdx > pGrpRows->endIdx) {
    *grpDone = true;
    pNMatch->rowIdx = 0;
  }
  
  return TSDB_CODE_SUCCESS;
}


static int32_t mFullJoinHandleMergeGrpRemains(SMJoinMergeCtx* pCtx) {
  static const uint8_t lowest_bit_bitmap[] = {32, 7, 6, 32, 5, 3, 32, 0, 4, 1, 2};
  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinNMatchCtx* pNMatch = &build->nMatchCtx;
  bool grpDone = false;
  int32_t baseIdx = 0;
  int32_t rowNum = 0;
  int32_t grpNum = taosArrayGetSize(build->eqGrps);
  for (; pNMatch->grpIdx < grpNum; ++pNMatch->grpIdx, pNMatch->bitIdx = 0) {
    grpDone = false;
    
    SMJoinGrpRows* pGrpRows = taosArrayGet(build->eqGrps, pNMatch->grpIdx);
    if (pGrpRows->allRowsMatch) {
      continue;
    }

    if (pGrpRows->rowMatchNum <= 0 || pGrpRows->allRowsNMatch) {
      if (!pGrpRows->allRowsNMatch) {
        pGrpRows->allRowsNMatch = true;
        pNMatch->rowIdx = pGrpRows->beginIdx;
      }
      
      MJ_ERR_RET(mFullJoinOutputMergeGrpRows(pCtx, pGrpRows, pNMatch, &grpDone));

      if (BLK_IS_FULL(pCtx->finBlk)) {
        if (grpDone) {
          ++pNMatch->grpIdx;
          pNMatch->bitIdx = 0;
        }
        
        pCtx->nmatchRemains = true;
        return TSDB_CODE_SUCCESS;
      }

      continue;
    }

    int32_t bitBytes = BitmapLen(pGrpRows->endIdx - pGrpRows->beginIdx + 1);
    rowNum = pGrpRows->endIdx - pGrpRows->beginIdx + 1;
    for (; pNMatch->bitIdx < bitBytes; ++pNMatch->bitIdx) {
      if (0 == build->pRowBitmap[pGrpRows->rowBitmapOffset + pNMatch->bitIdx]) {
        continue;
      }

      baseIdx = 8 * pNMatch->bitIdx;
      char *v = &build->pRowBitmap[pGrpRows->rowBitmapOffset + pNMatch->bitIdx];
      while (*v && !BLK_IS_FULL(pCtx->finBlk)) {
        uint8_t n = lowest_bit_bitmap[((*v & (*v - 1)) ^ *v) % 11];
        if (pGrpRows->beginIdx + baseIdx + n > pGrpRows->endIdx) {
          MJOIN_SET_ROW_BITMAP(build->pRowBitmap, pGrpRows->rowBitmapOffset + pNMatch->bitIdx, n);
          continue;
        }
        
        MJ_ERR_RET(mFullJoinOutputMergeRow(pCtx, pGrpRows, pGrpRows->beginIdx + baseIdx + n));

        MJOIN_SET_ROW_BITMAP(build->pRowBitmap, pGrpRows->rowBitmapOffset + pNMatch->bitIdx, n);
        if (++pGrpRows->rowMatchNum == rowNum) {
          pGrpRows->allRowsMatch = true;
          pNMatch->bitIdx = bitBytes;
          break;
        }
      }

      if (BLK_IS_FULL(pCtx->finBlk)) {
        break;
      }
    }

    if (BLK_IS_FULL(pCtx->finBlk)) {
      if (pNMatch->bitIdx >= bitBytes) {
        ++pNMatch->grpIdx;
        pNMatch->bitIdx = 0;
      }
      
      pCtx->nmatchRemains = true;
      return TSDB_CODE_SUCCESS;
    }      
  }

  pCtx->nmatchRemains = false;
  pCtx->lastEqGrp = false;  
  
  return TSDB_CODE_SUCCESS;  
}

static int32_t mFullJoinHandleBuildTableRemains(SMJoinMergeCtx* pCtx) {
  return pCtx->hashJoin ? mFullJoinHandleHashGrpRemains(pCtx) : mFullJoinHandleMergeGrpRemains(pCtx);
}

SSDataBlock* mFullJoinDo(struct SOperatorInfo* pOperator) {
  SMJoinOperatorInfo* pJoin = pOperator->info;
  SMJoinMergeCtx* pCtx = &pJoin->ctx.mergeCtx;
  int32_t code = TSDB_CODE_SUCCESS;
  int64_t probeTs = 0;
  int64_t buildTs = 0;
  SColumnInfoData* pBuildCol = NULL;
  SColumnInfoData* pProbeCol = NULL;

  blockDataCleanup(pCtx->finBlk);

  if (pCtx->midRemains) {
    MJ_ERR_JRET(mJoinHandleMidRemains(pCtx));
    if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
      return pCtx->finBlk;
    }
    pCtx->midRemains = false;
  }

  if (pCtx->grpRemains) {
    MJ_ERR_JRET(mFullJoinHandleGrpRemains(pCtx));
    if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
      return pCtx->finBlk;
    }
    pCtx->grpRemains = false;
  }

  if (pCtx->nmatchRemains) {
    MJ_ERR_JRET(mFullJoinHandleBuildTableRemains(pCtx));
    if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
      return pCtx->finBlk;
    }
  }

  do {
    if (!mFullJoinRetrieve(pOperator, pJoin)) {
      if (pCtx->lastEqGrp && pJoin->build->rowBitmapSize > 0) {
        MJ_ERR_JRET(mFullJoinHandleBuildTableRemains(pCtx));
        if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
          return pCtx->finBlk;
        }
      }

      mJoinSetDone(pOperator);      
      break;
    }

    MJOIN_GET_TB_COL_TS(pBuildCol, buildTs, pJoin->build);
    MJOIN_GET_TB_COL_TS(pProbeCol, probeTs, pJoin->probe);
    
    if (probeTs == pCtx->lastEqTs) {
      MJ_ERR_JRET(mJoinProcessEqualGrp(pCtx, probeTs, true));
      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }

      if (FJOIN_PROBE_TB_ROWS_DONE(pJoin->probe)) {
        continue;
      } else {
        MJOIN_GET_TB_CUR_TS(pProbeCol, probeTs, pJoin->probe);
      }
    }

    if (pCtx->lastEqGrp && pJoin->build->rowBitmapSize > 0) {
      MJ_ERR_JRET(mFullJoinHandleBuildTableRemains(pCtx));
      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }
    }

    while (!FJOIN_PROBE_TB_ROWS_DONE(pJoin->probe) && !MJOIN_BUILD_TB_ROWS_DONE(pJoin->build)) {
      if (probeTs == buildTs) {
        pCtx->lastEqTs = probeTs;
        MJ_ERR_JRET(mJoinProcessEqualGrp(pCtx, probeTs, false));
        if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
          return pCtx->finBlk;
        }

        MJOIN_GET_TB_COL_TS(pBuildCol, buildTs, pJoin->build);
        MJOIN_GET_TB_COL_TS(pProbeCol, probeTs, pJoin->probe);

        if (!FJOIN_PROBE_TB_ROWS_DONE(pJoin->probe) && probeTs != pCtx->lastEqTs && pJoin->build->rowBitmapSize > 0) {
          MJ_ERR_JRET(mFullJoinHandleBuildTableRemains(pCtx));
          if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
            return pCtx->finBlk;
          }
        }

        continue;
      }

      if (PROBE_TS_LOWER(pCtx->ascTs, probeTs, buildTs)) {
        MJ_ERR_JRET(mJoinProcessLowerGrp(pCtx, pJoin->probe, pProbeCol, &probeTs, &buildTs));
      } else {
        MJ_ERR_JRET(mJoinProcessGreaterGrp(pCtx, pJoin->build, pBuildCol, &probeTs, &buildTs));
      }

      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }
    }

    if (pJoin->build->dsFetchDone && !FJOIN_PROBE_TB_ROWS_DONE(pJoin->probe)) {
      if (pCtx->lastEqGrp && pJoin->build->rowBitmapSize > 0) {
        MJ_ERR_JRET(mFullJoinHandleBuildTableRemains(pCtx));
        if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
          return pCtx->finBlk;
        }
      }
      
      pCtx->probeNEqGrp.blk = pJoin->probe->blk;
      pCtx->probeNEqGrp.beginIdx = pJoin->probe->blkRowIdx;
      pCtx->probeNEqGrp.readIdx = pCtx->probeNEqGrp.beginIdx;
      pCtx->probeNEqGrp.endIdx = pJoin->probe->blk->info.rows - 1;
      
      pJoin->probe->blkRowIdx = pJoin->probe->blk->info.rows;
            
      MJ_ERR_JRET(mJoinNonEqCart((SMJoinCommonCtx*)pCtx, &pCtx->probeNEqGrp, true, false));
      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }
    }

    if (pJoin->probe->dsFetchDone && !MJOIN_BUILD_TB_ROWS_DONE(pJoin->build)) {
      if (pCtx->lastEqGrp && pJoin->build->rowBitmapSize > 0) {
        MJ_ERR_JRET(mFullJoinHandleBuildTableRemains(pCtx));
        if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
          return pCtx->finBlk;
        }
      }

      pCtx->buildNEqGrp.blk = pJoin->build->blk;
      pCtx->buildNEqGrp.beginIdx = pJoin->build->blkRowIdx;
      pCtx->buildNEqGrp.readIdx = pCtx->buildNEqGrp.beginIdx;
      pCtx->buildNEqGrp.endIdx = pJoin->build->blk->info.rows - 1;
      
      pJoin->build->blkRowIdx = pJoin->build->blk->info.rows;
            
      MJ_ERR_JRET(mJoinNonEqCart((SMJoinCommonCtx*)pCtx, &pCtx->buildNEqGrp, false, false));
      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }
    }

  } while (true);

_return:

  if (code) {
    pJoin->errCode = code;
    return NULL;
  }

  return pCtx->finBlk;
}


static int32_t mSemiJoinHashGrpCartFilter(SMJoinMergeCtx* pCtx, SMJoinGrpRows* probeGrp) {
  SMJoinTableCtx* probe = pCtx->pJoin->probe;
  SMJoinTableCtx* build = pCtx->pJoin->build;
  
  do {
    blockDataCleanup(pCtx->midBlk);

    mJoinHashGrpCart(pCtx->midBlk, probeGrp, true, probe, build);

    if (pCtx->midBlk->info.rows > 0) {
      MJ_ERR_RET(mJoinFilterAndKeepSingleRow(pCtx->midBlk, pCtx->pJoin->pPreFilter));
    }

    if (pCtx->midBlk->info.rows <= 0) {
      if (build->grpRowIdx < 0) {
        break;
      }
      
      continue;
    }

    ASSERT(1 == pCtx->midBlk->info.rows);
    MJ_ERR_RET(mJoinCopyMergeMidBlk(pCtx, &pCtx->midBlk, &pCtx->finBlk));
    ASSERT(false == pCtx->midRemains);
    
    break;
  } while (true);

  return TSDB_CODE_SUCCESS;
}


static int32_t mSemiJoinHashSeqCart(SMJoinMergeCtx* pCtx) {
  SMJoinTableCtx* probe = pCtx->pJoin->probe;
  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinGrpRows* probeGrp = taosArrayGet(probe->eqGrps, 0);

  size_t bufLen = 0;
  int32_t probeEndIdx = probeGrp->endIdx;
  for (; !GRP_DONE(probeGrp) && !BLK_IS_FULL(pCtx->finBlk); probeGrp->readIdx++) {
    if (mJoinCopyKeyColsDataToBuf(probe, probeGrp->readIdx, &bufLen)) {
      continue;
    }

    void* pGrp = tSimpleHashGet(build->pGrpHash, probe->keyData, bufLen);
    if (NULL == pGrp) {
      continue;
    }

    build->pHashCurGrp = *(SArray**)pGrp;
    build->grpRowIdx = 0;

    probeGrp->endIdx = probeGrp->readIdx;      
    MJ_ERR_RET(mSemiJoinHashGrpCartFilter(pCtx, probeGrp));
    probeGrp->endIdx = probeEndIdx;
  }

  pCtx->grpRemains = probeGrp->readIdx <= probeGrp->endIdx;

  return TSDB_CODE_SUCCESS;
}


static int32_t mSemiJoinHashFullCart(SMJoinMergeCtx* pCtx) {
  SMJoinTableCtx* probe = pCtx->pJoin->probe;
  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinGrpRows* probeGrp = taosArrayGet(probe->eqGrps, probe->grpIdx);
  size_t bufLen = 0;

  for (; !GRP_DONE(probeGrp) && !BLK_IS_FULL(pCtx->finBlk); ++probeGrp->readIdx) {
    if (mJoinCopyKeyColsDataToBuf(probe, probeGrp->readIdx, &bufLen)) {
      continue;
    }

    void* pGrp = tSimpleHashGet(build->pGrpHash, probe->keyData, bufLen);
    if (NULL == pGrp) {
      continue;
    }

    build->pHashCurGrp = *(SArray**)pGrp;
    ASSERT(1 == taosArrayGetSize(build->pHashCurGrp));
    build->grpRowIdx = 0;
    mJoinHashGrpCart(pCtx->finBlk, probeGrp, true, probe, build);
    ASSERT(build->grpRowIdx < 0);
  }

  pCtx->grpRemains = probeGrp->readIdx <= probeGrp->endIdx;

  return TSDB_CODE_SUCCESS;
}


static int32_t mSemiJoinMergeSeqCart(SMJoinMergeCtx* pCtx) {
  SMJoinTableCtx* probe = pCtx->pJoin->probe;
  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinGrpRows* probeGrp = taosArrayGet(probe->eqGrps, probe->grpIdx);
  SMJoinGrpRows* buildGrp = NULL;
  int32_t buildGrpNum = taosArrayGetSize(build->eqGrps);
  int32_t probeEndIdx = probeGrp->endIdx;
  int32_t rowsLeft = pCtx->midBlk->info.capacity;  

  do {
    for (; !GRP_DONE(probeGrp) && !BLK_IS_FULL(pCtx->finBlk); 
      ++probeGrp->readIdx, probeGrp->endIdx = probeEndIdx, build->grpIdx = 0) {
      probeGrp->endIdx = probeGrp->readIdx;
      
      rowsLeft = pCtx->midBlk->info.capacity;

      blockDataCleanup(pCtx->midBlk);      
      for (; build->grpIdx < buildGrpNum && rowsLeft > 0; ++build->grpIdx) {
        buildGrp = taosArrayGet(build->eqGrps, build->grpIdx);

        if (rowsLeft >= GRP_REMAIN_ROWS(buildGrp)) {
          MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->midBlk, true, probeGrp, buildGrp));
          rowsLeft -= GRP_REMAIN_ROWS(buildGrp);
          buildGrp->readIdx = buildGrp->beginIdx;
          continue;
        }
        
        int32_t buildEndIdx = buildGrp->endIdx;
        buildGrp->endIdx = buildGrp->readIdx + rowsLeft - 1;
        ASSERT(buildGrp->endIdx >= buildGrp->readIdx);
        MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->midBlk, true, probeGrp, buildGrp));
        buildGrp->readIdx += rowsLeft;
        buildGrp->endIdx = buildEndIdx;
        rowsLeft = 0;
        break;
      }

      if (pCtx->midBlk->info.rows > 0) {
        MJ_ERR_RET(mJoinFilterAndKeepSingleRow(pCtx->midBlk, pCtx->pJoin->pFPreFilter));
      } 

      if (0 == pCtx->midBlk->info.rows) {
        if (build->grpIdx == buildGrpNum) {
          continue;
        }
      } else {
        ASSERT(1 == pCtx->midBlk->info.rows);
        MJ_ERR_RET(mJoinCopyMergeMidBlk(pCtx, &pCtx->midBlk, &pCtx->finBlk));
        ASSERT(false == pCtx->midRemains);

        if (build->grpIdx == buildGrpNum) {
          continue;
        }

        buildGrp->readIdx = buildGrp->beginIdx;        
        continue;
      }

      //need break

      probeGrp->endIdx = probeEndIdx;
      break;
    }

    if (GRP_DONE(probeGrp) || BLK_IS_FULL(pCtx->finBlk)) {
      break;
    }
  } while (true);

  pCtx->grpRemains = probeGrp->readIdx <= probeGrp->endIdx;

  return TSDB_CODE_SUCCESS;
}


static int32_t mSemiJoinMergeFullCart(SMJoinMergeCtx* pCtx) {
  int32_t rowsLeft = pCtx->finBlk->info.capacity - pCtx->finBlk->info.rows;
  SMJoinTableCtx* probe = pCtx->pJoin->probe;
  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinGrpRows* probeGrp = taosArrayGet(probe->eqGrps, 0);
  SMJoinGrpRows* buildGrp = taosArrayGet(build->eqGrps, 0);
  int32_t probeRows = GRP_REMAIN_ROWS(probeGrp);
  int32_t probeEndIdx = probeGrp->endIdx;

  ASSERT(1 == taosArrayGetSize(build->eqGrps));
  ASSERT(buildGrp->beginIdx == buildGrp->endIdx);

  if (probeRows <= rowsLeft) {
    MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, buildGrp));

    pCtx->grpRemains = false;
    return TSDB_CODE_SUCCESS;
  }

  probeGrp->endIdx = probeGrp->readIdx + rowsLeft - 1;
  MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, buildGrp));
  probeGrp->readIdx = probeGrp->endIdx + 1; 
  probeGrp->endIdx = probeEndIdx;

  pCtx->grpRemains = true;
  
  return TSDB_CODE_SUCCESS;  
}


static int32_t mSemiJoinHashCart(SMJoinMergeCtx* pCtx) {
  return (NULL == pCtx->pJoin->pPreFilter) ? mSemiJoinHashFullCart(pCtx) : mSemiJoinHashSeqCart(pCtx);
}

static int32_t mSemiJoinMergeCart(SMJoinMergeCtx* pCtx) {
  return (NULL == pCtx->pJoin->pFPreFilter) ? mSemiJoinMergeFullCart(pCtx) : mSemiJoinMergeSeqCart(pCtx);
}

static FORCE_INLINE int32_t mSemiJoinHandleGrpRemains(SMJoinMergeCtx* pCtx) {
  return (pCtx->hashJoin) ? (*pCtx->hashCartFp)(pCtx) : (*pCtx->mergeCartFp)(pCtx);
}


SSDataBlock* mSemiJoinDo(struct SOperatorInfo* pOperator) {
  SMJoinOperatorInfo* pJoin = pOperator->info;
  SMJoinMergeCtx* pCtx = &pJoin->ctx.mergeCtx;
  int32_t code = TSDB_CODE_SUCCESS;
  int64_t probeTs = 0;
  int64_t buildTs = 0;
  SColumnInfoData* pBuildCol = NULL;
  SColumnInfoData* pProbeCol = NULL;

  blockDataCleanup(pCtx->finBlk);

  if (pCtx->grpRemains) {
    MJ_ERR_JRET(mSemiJoinHandleGrpRemains(pCtx));
    if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
      return pCtx->finBlk;
    }
    pCtx->grpRemains = false;
  }

  do {
    if (!mInnerJoinRetrieve(pOperator, pJoin)) {
      break;
    }

    MJOIN_GET_TB_COL_TS(pBuildCol, buildTs, pJoin->build);
    MJOIN_GET_TB_COL_TS(pProbeCol, probeTs, pJoin->probe);
    
    if (probeTs == pCtx->lastEqTs) {
      MJ_ERR_JRET(mJoinProcessEqualGrp(pCtx, probeTs, true));
      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }

      if (MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe) || MJOIN_BUILD_TB_ROWS_DONE(pJoin->build)) {
        continue;
      } 

      MJOIN_GET_TB_CUR_TS(pProbeCol, probeTs, pJoin->probe);
    } else if (MJOIN_BUILD_TB_ROWS_DONE(pJoin->build)) {
      mJoinSetDone(pOperator);
      break;
    }

    do {
      if (probeTs == buildTs) {
        pCtx->lastEqTs = probeTs;
        MJ_ERR_JRET(mJoinProcessEqualGrp(pCtx, probeTs, false));
        if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
          return pCtx->finBlk;
        }

        if (MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe) || MJOIN_BUILD_TB_ROWS_DONE(pJoin->build)) {
          break;
        }
        
        MJOIN_GET_TB_COL_TS(pBuildCol, buildTs, pJoin->build);
        MJOIN_GET_TB_COL_TS(pProbeCol, probeTs, pJoin->probe);
        continue;
      }

      if (PROBE_TS_LOWER(pCtx->ascTs, probeTs, buildTs)) {
        if (++pJoin->probe->blkRowIdx < pJoin->probe->blk->info.rows) {
          MJOIN_GET_TB_CUR_TS(pProbeCol, probeTs, pJoin->probe);
          continue;
        }
      } else {
        if (++pJoin->build->blkRowIdx < pJoin->build->blk->info.rows) {
          MJOIN_GET_TB_CUR_TS(pBuildCol, buildTs, pJoin->build);
          continue;
        }
      }
      
      break;
    } while (true);
  } while (true);

_return:

  if (code) {
    pJoin->errCode = code;
    return NULL;
  }

  return pCtx->finBlk;
}


static FORCE_INLINE int32_t mAntiJoinHandleGrpRemains(SMJoinMergeCtx* pCtx) {
  if (pCtx->lastEqGrp) {
    return (pCtx->hashJoin) ? (*pCtx->hashCartFp)(pCtx) : (*pCtx->mergeCartFp)(pCtx);
  }
  
  return mJoinNonEqCart((SMJoinCommonCtx*)pCtx, &pCtx->probeNEqGrp, true, false);
}

static int32_t mAntiJoinHashFullCart(SMJoinMergeCtx* pCtx) {
  SMJoinTableCtx* probe = pCtx->pJoin->probe;
  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinGrpRows* probeGrp = taosArrayGet(probe->eqGrps, probe->grpIdx);
  size_t bufLen = 0;
  int32_t probeEndIdx = probeGrp->endIdx;

  for (; !GRP_DONE(probeGrp) && !BLK_IS_FULL(pCtx->finBlk); ++probeGrp->readIdx) {
    if (mJoinCopyKeyColsDataToBuf(probe, probeGrp->readIdx, &bufLen)) {
      probeGrp->endIdx = probeGrp->readIdx;
      MJ_ERR_RET(mJoinNonEqGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, true));
      probeGrp->endIdx = probeEndIdx;
      continue;
    }

    void* pGrp = tSimpleHashGet(build->pGrpHash, probe->keyData, bufLen);
    if (NULL == pGrp) {
      probeGrp->endIdx = probeGrp->readIdx;
      MJ_ERR_RET(mJoinNonEqGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, true));
      probeGrp->endIdx = probeEndIdx;
    }
  }

  pCtx->grpRemains = probeGrp->readIdx <= probeGrp->endIdx;

  return TSDB_CODE_SUCCESS;
}


static int32_t mAntiJoinHashGrpCartFilter(SMJoinMergeCtx* pCtx, SMJoinGrpRows* probeGrp) {
  SMJoinTableCtx* probe = pCtx->pJoin->probe;
  SMJoinTableCtx* build = pCtx->pJoin->build;
  
  do {
    blockDataCleanup(pCtx->midBlk);

    mJoinHashGrpCart(pCtx->midBlk, probeGrp, true, probe, build);

    if (pCtx->midBlk->info.rows > 0) {
      MJ_ERR_RET(mJoinFilterAndNoKeepRows(pCtx->midBlk, pCtx->pJoin->pPreFilter));
    } 

    if (pCtx->midBlk->info.rows) {
      break;
    }
    
    if (build->grpRowIdx < 0) {
      MJ_ERR_RET(mJoinNonEqGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, true));
      break;
    }
    
    continue;
  } while (true);

  return TSDB_CODE_SUCCESS;
}


static int32_t mAntiJoinHashSeqCart(SMJoinMergeCtx* pCtx) {
  SMJoinTableCtx* probe = pCtx->pJoin->probe;
  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinGrpRows* probeGrp = taosArrayGet(probe->eqGrps, 0);
  size_t bufLen = 0;
  int32_t probeEndIdx = probeGrp->endIdx;

  for (; !GRP_DONE(probeGrp) && !BLK_IS_FULL(pCtx->finBlk); probeGrp->readIdx++) {
    if (mJoinCopyKeyColsDataToBuf(probe, probeGrp->readIdx, &bufLen)) {
      probeGrp->endIdx = probeGrp->readIdx;
      MJ_ERR_RET(mJoinNonEqGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, true));
      probeGrp->endIdx = probeEndIdx;
      continue;
    }

    void* pGrp = tSimpleHashGet(build->pGrpHash, probe->keyData, bufLen);
    if (NULL == pGrp) {
      probeGrp->endIdx = probeGrp->readIdx;
      MJ_ERR_RET(mJoinNonEqGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, true));
      probeGrp->endIdx = probeEndIdx;
      continue;
    }

    build->pHashCurGrp = *(SArray**)pGrp;
    build->grpRowIdx = 0;

    probeGrp->endIdx = probeGrp->readIdx;      
    MJ_ERR_RET(mAntiJoinHashGrpCartFilter(pCtx, probeGrp));
    probeGrp->endIdx = probeEndIdx;
  }

  pCtx->grpRemains = probeGrp->readIdx <= probeGrp->endIdx;

  return TSDB_CODE_SUCCESS;
}

static int32_t mAntiJoinMergeFullCart(SMJoinMergeCtx* pCtx) {
  return TSDB_CODE_SUCCESS;
}

static int32_t mAntiJoinMergeSeqCart(SMJoinMergeCtx* pCtx) {
  SMJoinTableCtx* probe = pCtx->pJoin->probe;
  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinGrpRows* probeGrp = taosArrayGet(probe->eqGrps, probe->grpIdx);
  SMJoinGrpRows* buildGrp = NULL;
  int32_t buildGrpNum = taosArrayGetSize(build->eqGrps);
  int32_t probeEndIdx = probeGrp->endIdx;
  int32_t rowsLeft = pCtx->midBlk->info.capacity;  

  do {
    for (; !GRP_DONE(probeGrp) && !BLK_IS_FULL(pCtx->finBlk); 
      ++probeGrp->readIdx, probeGrp->endIdx = probeEndIdx, build->grpIdx = 0) {
      probeGrp->endIdx = probeGrp->readIdx;
      
      rowsLeft = pCtx->midBlk->info.capacity;

      blockDataCleanup(pCtx->midBlk);      
      for (; build->grpIdx < buildGrpNum && rowsLeft > 0; ++build->grpIdx) {
        buildGrp = taosArrayGet(build->eqGrps, build->grpIdx);
        if (rowsLeft >= GRP_REMAIN_ROWS(buildGrp)) {
          MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->midBlk, true, probeGrp, buildGrp));
          rowsLeft -= GRP_REMAIN_ROWS(buildGrp);
          buildGrp->readIdx = buildGrp->beginIdx;
          continue;
        }
        
        int32_t buildEndIdx = buildGrp->endIdx;
        buildGrp->endIdx = buildGrp->readIdx + rowsLeft - 1;
        ASSERT(buildGrp->endIdx >= buildGrp->readIdx);
        MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->midBlk, true, probeGrp, buildGrp));
        buildGrp->readIdx += rowsLeft;
        buildGrp->endIdx = buildEndIdx;
        rowsLeft = 0;
        break;
      }

      if (pCtx->midBlk->info.rows > 0) {
        MJ_ERR_RET(mJoinFilterAndNoKeepRows(pCtx->midBlk, pCtx->pJoin->pFPreFilter));
      } 

      if (pCtx->midBlk->info.rows > 0) {
        if (build->grpIdx < buildGrpNum) {
          buildGrp->readIdx = buildGrp->beginIdx;        
        }

        continue;
      }
      
      if (build->grpIdx >= buildGrpNum) {
        MJ_ERR_RET(mJoinNonEqGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, true));
        continue;
      }

      //need break

      probeGrp->endIdx = probeEndIdx;
      break;
    }

    if (GRP_DONE(probeGrp) || BLK_IS_FULL(pCtx->finBlk)) {
      break;
    }
  } while (true);

  pCtx->grpRemains = probeGrp->readIdx <= probeGrp->endIdx;

  return TSDB_CODE_SUCCESS;
}


static int32_t mAntiJoinHashCart(SMJoinMergeCtx* pCtx) {
  return (NULL == pCtx->pJoin->pPreFilter) ? mAntiJoinHashFullCart(pCtx) : mAntiJoinHashSeqCart(pCtx);
}

static int32_t mAntiJoinMergeCart(SMJoinMergeCtx* pCtx) {
  return (NULL == pCtx->pJoin->pFPreFilter) ? mAntiJoinMergeFullCart(pCtx) : mAntiJoinMergeSeqCart(pCtx);
}

SSDataBlock* mAntiJoinDo(struct SOperatorInfo* pOperator) {
  SMJoinOperatorInfo* pJoin = pOperator->info;
  SMJoinMergeCtx* pCtx = &pJoin->ctx.mergeCtx;
  int32_t code = TSDB_CODE_SUCCESS;
  int64_t probeTs = 0;
  int64_t buildTs = 0;
  SColumnInfoData* pBuildCol = NULL;
  SColumnInfoData* pProbeCol = NULL;

  blockDataCleanup(pCtx->finBlk);

  if (pCtx->grpRemains) {
    MJ_ERR_JRET(mAntiJoinHandleGrpRemains(pCtx));
    if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
      return pCtx->finBlk;
    }
    pCtx->grpRemains = false;
  }

  do {
    if (!mLeftJoinRetrieve(pOperator, pJoin, pCtx)) {
      break;
    }

    MJOIN_GET_TB_COL_TS(pBuildCol, buildTs, pJoin->build);
    MJOIN_GET_TB_COL_TS(pProbeCol, probeTs, pJoin->probe);
    
    if (probeTs == pCtx->lastEqTs) {
      MJ_ERR_JRET(mJoinProcessEqualGrp(pCtx, probeTs, true));
      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }

      if (MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe)) {
        continue;
      } else {
        MJOIN_GET_TB_CUR_TS(pProbeCol, probeTs, pJoin->probe);
      }
    }

    while (!MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe) && !MJOIN_BUILD_TB_ROWS_DONE(pJoin->build)) {
      if (probeTs == buildTs) {
        pCtx->lastEqTs = probeTs;
        MJ_ERR_JRET(mJoinProcessEqualGrp(pCtx, probeTs, false));
        if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
          return pCtx->finBlk;
        }

        MJOIN_GET_TB_COL_TS(pBuildCol, buildTs, pJoin->build);
        MJOIN_GET_TB_COL_TS(pProbeCol, probeTs, pJoin->probe);
      } else if (PROBE_TS_LOWER(pCtx->ascTs, probeTs, buildTs)) {
        MJ_ERR_JRET(mJoinProcessLowerGrp(pCtx, pJoin->probe, pProbeCol, &probeTs, &buildTs));
        if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
          return pCtx->finBlk;
        }
      } else {
        while (++pJoin->build->blkRowIdx < pJoin->build->blk->info.rows) {
          MJOIN_GET_TB_CUR_TS(pBuildCol, buildTs, pJoin->build);
          if (PROBE_TS_GREATER(pCtx->ascTs, probeTs, buildTs)) {
            continue;
          }
          
          break;
        }
      }
    }

    if (!MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe) && pJoin->build->dsFetchDone) {
      pCtx->probeNEqGrp.blk = pJoin->probe->blk;
      pCtx->probeNEqGrp.beginIdx = pJoin->probe->blkRowIdx;
      pCtx->probeNEqGrp.readIdx = pCtx->probeNEqGrp.beginIdx;
      pCtx->probeNEqGrp.endIdx = pJoin->probe->blk->info.rows - 1;
      
      pJoin->probe->blkRowIdx = pJoin->probe->blk->info.rows;
            
      MJ_ERR_JRET(mJoinNonEqCart((SMJoinCommonCtx*)pCtx, &pCtx->probeNEqGrp, true, false));
      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }
    }
  } while (true);

_return:

  if (code) {
    pJoin->errCode = code;
    return NULL;
  }

  return pCtx->finBlk;
}


int32_t mAsofLowerCalcRowNum(SMJoinWinCache* pCache, int64_t jLimit, int32_t newRows, int32_t* evictRows) {
  if (pCache->outBlk->info.rows <= 0) {
    *evictRows = 0;
    return TMIN(jLimit, newRows);
  }

  if ((pCache->outBlk->info.rows + newRows) <= jLimit) {
    *evictRows = 0;
    return newRows;
  }

  if (newRows >= jLimit) {
    *evictRows = pCache->outBlk->info.rows;
    return jLimit;
  }

  *evictRows = pCache->outBlk->info.rows + newRows - jLimit;
  return newRows;
}

int32_t mAsofLowerAddRowsToCache(SMJoinWindowCtx* pCtx, SMJoinGrpRows* pGrp, bool fromBegin) {
  int32_t evictRows = 0;
  SMJoinWinCache* pCache = &pCtx->cache;
  int32_t rows = mAsofLowerCalcRowNum(pCache, pCtx->jLimit, pGrp->endIdx - pGrp->beginIdx + 1, &evictRows);
  if (evictRows > 0) {
    MJ_ERR_RET(blockDataTrimFirstRows(pCache->outBlk, evictRows));
  }

  int32_t startIdx = fromBegin ? pGrp->beginIdx : pGrp->endIdx - rows + 1;
  return blockDataMergeNRows(pCache->outBlk, pGrp->blk, startIdx, rows);
}


int32_t mAsofLowerAddEqRowsToCache(struct SOperatorInfo* pOperator, SMJoinWindowCtx* pCtx, SMJoinTableCtx* pTable, int64_t timestamp) {
  int64_t eqRowsNum = 0;
  SMJoinGrpRows grp;

  do {
      grp.blk = pTable->blk;
      
      SColumnInfoData* pCol = taosArrayGet(pTable->blk->pDataBlock, pTable->primCol->srcSlot);

      if (*(int64_t*)colDataGetNumData(pCol, pTable->blkRowIdx) != timestamp) {
        return TSDB_CODE_SUCCESS;
      }

      grp.beginIdx = pTable->blkRowIdx;
      
      char* pEndVal = colDataGetNumData(pCol, pTable->blk->info.rows - 1);
      if (timestamp != *(int64_t*)pEndVal) {
        for (; pTable->blkRowIdx < pTable->blk->info.rows; ++pTable->blkRowIdx) {
          char* pNextVal = colDataGetNumData(pCol, pTable->blkRowIdx);
          if (timestamp == *(int64_t*)pNextVal) {
            continue;
          }

          break;
        }

        grp.endIdx = pTable->blkRowIdx - 1;
      } else {
        grp.endIdx = pTable->blk->info.rows - 1;
        pTable->blkRowIdx = pTable->blk->info.rows;
      }

      if (eqRowsNum < pCtx->jLimit) {
        grp.endIdx = grp.beginIdx + TMIN(grp.endIdx - grp.beginIdx + 1, pCtx->jLimit - eqRowsNum) - 1;
        MJ_ERR_RET(mAsofLowerAddRowsToCache(pCtx, &grp, true));
      }
      
      eqRowsNum += grp.endIdx - grp.beginIdx + 1;

    if (pTable->blkRowIdx == pTable->blk->info.rows && !pTable->dsFetchDone) {
      pTable->blk = (*pCtx->pJoin->retrieveFp)(pCtx->pJoin, pTable);
      qDebug("%s merge join %s table got block for same ts, rows:%" PRId64, GET_TASKID(pOperator->pTaskInfo), MJOIN_TBTYPE(pTable->type), pTable->blk ? pTable->blk->info.rows : 0);

      pTable->blkRowIdx = 0;
      pCtx->buildGrp.blk = pTable->blk;

      if (NULL == pTable->blk) {
        break;
      }    
    } else {
      break;
    }
  } while (true);

  return TSDB_CODE_SUCCESS;
}

int32_t mAsofLowerDumpGrpCache(SMJoinWindowCtx* pCtx) {
  if (NULL == pCtx->cache.outBlk || pCtx->cache.outBlk->info.rows <= 0) {
    return mJoinNonEqCart((SMJoinCommonCtx*)pCtx, &pCtx->probeGrp, true, false);
  }

  int32_t rowsLeft = pCtx->finBlk->info.capacity - pCtx->finBlk->info.rows;
  SMJoinGrpRows* probeGrp = &pCtx->probeGrp;
  SMJoinGrpRows buildGrp = {.blk = pCtx->cache.outBlk, .readIdx = pCtx->cache.outRowIdx, .endIdx = pCtx->cache.outBlk->info.rows - 1};
  int32_t probeRows = GRP_REMAIN_ROWS(probeGrp);
  int32_t probeEndIdx = probeGrp->endIdx;
  int64_t totalResRows = (0 == pCtx->cache.outRowIdx) ? (probeRows * pCtx->cache.outBlk->info.rows) : 
    (pCtx->cache.outBlk->info.rows - pCtx->cache.outRowIdx + (probeRows - 1) * pCtx->cache.outBlk->info.rows);

  if (totalResRows <= rowsLeft) {
    if (0 == pCtx->cache.outRowIdx) {
      MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, &buildGrp));

      pCtx->grpRemains = false;
      pCtx->cache.outRowIdx = 0;
      return TSDB_CODE_SUCCESS;
    }

    probeGrp->endIdx = probeGrp->readIdx;
    MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, &buildGrp));
    if (++probeGrp->readIdx <= probeEndIdx) {
      probeGrp->endIdx = probeEndIdx;
      buildGrp.readIdx = 0;
      MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, &buildGrp));
    }
    
    pCtx->grpRemains = false;
    pCtx->cache.outRowIdx = 0;
    return TSDB_CODE_SUCCESS;
  }

  for (; !GRP_DONE(probeGrp) && rowsLeft > 0; ) {
    if (0 == pCtx->cache.outRowIdx) {
      int32_t grpNum = rowsLeft / pCtx->cache.outBlk->info.rows;
      if (grpNum > 0) {
        probeGrp->endIdx = probeGrp->readIdx + grpNum - 1;
        buildGrp.readIdx = 0;
        MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, &buildGrp));
        rowsLeft -= grpNum * pCtx->cache.outBlk->info.rows;
        probeGrp->readIdx += grpNum;
        probeGrp->endIdx = probeEndIdx;
        continue;
      }
    }
    
    probeGrp->endIdx = probeGrp->readIdx;
    buildGrp.readIdx = pCtx->cache.outRowIdx;
    
    int32_t grpRemainRows = pCtx->cache.outBlk->info.rows - pCtx->cache.outRowIdx;
    if (rowsLeft >= grpRemainRows) {
      MJ_ERR_RET(mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, &buildGrp));
      rowsLeft -= grpRemainRows;
      pCtx->cache.outRowIdx = 0;
      probeGrp->readIdx++;
      probeGrp->endIdx = probeEndIdx;
      continue;
    }
    
    buildGrp.endIdx = buildGrp.readIdx + rowsLeft - 1;
    mJoinMergeGrpCart(pCtx->pJoin, pCtx->finBlk, true, probeGrp, &buildGrp);
    pCtx->cache.outRowIdx += rowsLeft;
    break;
  }

  probeGrp->endIdx = probeEndIdx;
  pCtx->grpRemains = probeGrp->readIdx <= probeGrp->endIdx;
  
  return TSDB_CODE_SUCCESS;  
}

int32_t mAsofLowerDumpUpdateEqRows(SMJoinWindowCtx* pCtx, SMJoinOperatorInfo* pJoin, bool lastBuildGrp, bool skipEqPost) {
  if (!pCtx->eqRowsAcq) {
    MJ_ERR_RET(mAsofLowerDumpGrpCache(pCtx));

    pCtx->lastEqGrp = true;
    if (pCtx->grpRemains) {
      return TSDB_CODE_SUCCESS;
    }
  }

  if (!pCtx->eqPostDone && !lastBuildGrp && (pCtx->eqRowsAcq || !skipEqPost)) {
    pCtx->eqPostDone = true;
    MJ_ERR_RET(mAsofLowerAddEqRowsToCache(pJoin->pOperator, pCtx, pJoin->build, pCtx->lastTs));
  }

  if (!pCtx->eqRowsAcq) {
    return TSDB_CODE_SUCCESS;
  }

  MJ_ERR_RET(mAsofLowerDumpGrpCache(pCtx));

  pCtx->lastEqGrp = true;

  return TSDB_CODE_SUCCESS;
}

int32_t mAsofLowerProcessEqualGrp(SMJoinWindowCtx* pCtx, int64_t timestamp, bool lastBuildGrp) {
  SMJoinOperatorInfo* pJoin = pCtx->pJoin;

  if (!lastBuildGrp) {
    pCtx->eqPostDone = false;
  }

  bool wholeBlk = false;
  MJ_ERR_RET(mJoinBuildEqGrp(pJoin->probe, timestamp, &wholeBlk, &pCtx->probeGrp));

  MJ_ERR_RET(mAsofLowerDumpUpdateEqRows(pCtx, pJoin, lastBuildGrp, wholeBlk));
  
  return TSDB_CODE_SUCCESS;
}


int32_t mAsofLowerProcessLowerGrp(SMJoinWindowCtx* pCtx, SMJoinOperatorInfo* pJoin, SColumnInfoData* pCol,  int64_t* probeTs, int64_t* buildTs) {
  pCtx->lastEqGrp = false;
  
  pCtx->probeGrp.beginIdx = pJoin->probe->blkRowIdx;
  pCtx->probeGrp.readIdx = pCtx->probeGrp.beginIdx;
  pCtx->probeGrp.endIdx = pCtx->probeGrp.beginIdx;
  
  while (++pJoin->probe->blkRowIdx < pJoin->probe->blk->info.rows) {
    MJOIN_GET_TB_CUR_TS(pCol, *probeTs, pJoin->probe);
    if (PROBE_TS_LOWER(pCtx->ascTs, *probeTs, *buildTs)) {
      pCtx->probeGrp.endIdx = pJoin->probe->blkRowIdx;
      continue;
    }
    
    break;
  }

  return mAsofLowerDumpGrpCache(pCtx);
}

int32_t mAsofLowerProcessGreaterGrp(SMJoinWindowCtx* pCtx, SMJoinOperatorInfo* pJoin, SColumnInfoData* pCol,  int64_t* probeTs, int64_t* buildTs) {
  pCtx->lastEqGrp = false;

  pCtx->buildGrp.beginIdx = pJoin->build->blkRowIdx;
  pCtx->buildGrp.readIdx = pCtx->buildGrp.beginIdx;
  pCtx->buildGrp.endIdx = pCtx->buildGrp.beginIdx;
  
  while (++pJoin->build->blkRowIdx < pJoin->build->blk->info.rows) {
    MJOIN_GET_TB_CUR_TS(pCol, *buildTs, pJoin->build);
    if (PROBE_TS_GREATER(pCtx->ascTs, *probeTs, *buildTs)) {
      pCtx->buildGrp.endIdx = pJoin->build->blkRowIdx;
      continue;
    }
    
    break;
  }

  pCtx->probeGrp.beginIdx = pJoin->probe->blkRowIdx;
  pCtx->probeGrp.readIdx = pCtx->probeGrp.beginIdx;
  pCtx->probeGrp.endIdx = pCtx->probeGrp.beginIdx;

  return mAsofLowerAddRowsToCache(pCtx, &pCtx->buildGrp, false);
}

int32_t mAsofLowerHandleGrpRemains(SMJoinWindowCtx* pCtx) {
  return (pCtx->lastEqGrp) ? mAsofLowerDumpUpdateEqRows(pCtx, pCtx->pJoin, false, true) : mAsofLowerDumpGrpCache(pCtx);
}

static bool mAsofLowerRetrieve(SOperatorInfo* pOperator, SMJoinOperatorInfo* pJoin, SMJoinWindowCtx* pCtx) {
  bool probeGot = mJoinRetrieveBlk(pJoin, &pJoin->probe->blkRowIdx, &pJoin->probe->blk, pJoin->probe);
  bool buildGot = false;

  do {
    if (probeGot || MJOIN_DS_NEED_INIT(pOperator, pJoin->build)) {  
      buildGot = mJoinRetrieveBlk(pJoin, &pJoin->build->blkRowIdx, &pJoin->build->blk, pJoin->build);
    }
    
    if (!probeGot) {
      if (!pCtx->groupJoin || NULL == pJoin->probe->remainInBlk) {
        mJoinSetDone(pOperator);
      }

      return false;
    }
    
    break;
  } while (true);

  if (buildGot && NULL == pCtx->cache.outBlk) {
    pCtx->cache.outBlk = createOneDataBlock(pJoin->build->blk, false);
    blockDataEnsureCapacity(pCtx->cache.outBlk, pCtx->jLimit);
  }

  pCtx->probeGrp.blk = pJoin->probe->blk;
  pCtx->buildGrp.blk = pJoin->build->blk;

  return true;
}


SSDataBlock* mAsofLowerJoinDo(struct SOperatorInfo* pOperator) {
  SMJoinOperatorInfo* pJoin = pOperator->info;
  SMJoinWindowCtx* pCtx = &pJoin->ctx.windowCtx;
  int32_t code = TSDB_CODE_SUCCESS;
  int64_t probeTs = 0;
  int64_t buildTs = 0;
  SColumnInfoData* pBuildCol = NULL;
  SColumnInfoData* pProbeCol = NULL;

  blockDataCleanup(pCtx->finBlk);

  if (pCtx->grpRemains) {
    MJ_ERR_JRET(mAsofLowerHandleGrpRemains(pCtx));
    if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
      return pCtx->finBlk;
    }
    pCtx->grpRemains = false;
  }

  do {
    if (!mAsofLowerRetrieve(pOperator, pJoin, pCtx)) {
      if (pCtx->groupJoin && pCtx->finBlk->info.rows <= 0 && !mJoinIsDone(pOperator)) {
        continue;
      }
      
      break;
    }

    MJOIN_GET_TB_COL_TS(pBuildCol, buildTs, pJoin->build);
    MJOIN_GET_TB_COL_TS(pProbeCol, probeTs, pJoin->probe);
    
    if (probeTs == pCtx->lastTs) {
      MJ_ERR_JRET(mAsofLowerProcessEqualGrp(pCtx, probeTs, true));
      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }

      if (MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe)) {
        continue;
      } else {
        MJOIN_GET_TB_CUR_TS(pProbeCol, probeTs, pJoin->probe);
      }
    }

    if (pCtx->lastEqGrp && !pCtx->eqPostDone) {
      pCtx->eqPostDone = true;
      MJ_ERR_JRET(mAsofLowerAddEqRowsToCache(pJoin->pOperator, pCtx, pJoin->build, pCtx->lastTs));
      MJOIN_GET_TB_COL_TS(pBuildCol, buildTs, pJoin->build);
    }

    while (!MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe) && !MJOIN_BUILD_TB_ROWS_DONE(pJoin->build)) {
      if (probeTs == buildTs) {
        pCtx->lastTs = probeTs;
        MJ_ERR_JRET(mAsofLowerProcessEqualGrp(pCtx, probeTs, false));
        if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
          return pCtx->finBlk;
        }

        MJOIN_GET_TB_COL_TS(pBuildCol, buildTs, pJoin->build);
        MJOIN_GET_TB_COL_TS(pProbeCol, probeTs, pJoin->probe);
        continue;
      }

      if (PROBE_TS_LOWER(pCtx->ascTs, probeTs, buildTs)) {
        MJ_ERR_JRET(mAsofLowerProcessLowerGrp(pCtx, pJoin, pProbeCol, &probeTs, &buildTs));
      } else {
        MJ_ERR_JRET(mAsofLowerProcessGreaterGrp(pCtx, pJoin, pBuildCol, &probeTs, &buildTs));
      }

      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }
    }

    if (!MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe) && (pJoin->build->dsFetchDone || (pCtx->groupJoin && NULL == pJoin->build->blk))) {
      pCtx->probeGrp.beginIdx = pJoin->probe->blkRowIdx;
      pCtx->probeGrp.readIdx = pCtx->probeGrp.beginIdx;
      pCtx->probeGrp.endIdx = pJoin->probe->blk->info.rows - 1;
      
      MJ_ERR_JRET(mAsofLowerDumpGrpCache(pCtx));
      pCtx->lastEqGrp = false;
      
      pJoin->probe->blkRowIdx = pJoin->probe->blk->info.rows;
            
      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }
    }
  } while (true);

_return:

  if (code) {
    pJoin->errCode = code;
    return NULL;
  }

  return pCtx->finBlk;
}

int32_t mAsofGreaterTrimCacheBlk(SMJoinWindowCtx* pCtx) {
  if (taosArrayGetSize(pCtx->cache.grps) <= 0) {
    return TSDB_CODE_SUCCESS;
  }
  
  SMJoinGrpRows* pGrp = taosArrayGet(pCtx->cache.grps, 0);
  if (pGrp->blk == pCtx->cache.outBlk && pCtx->pJoin->build->blkRowIdx > 0) {
    MJ_ERR_RET(blockDataTrimFirstRows(pGrp->blk, pCtx->pJoin->build->blkRowIdx));
    pCtx->pJoin->build->blkRowIdx = 0;
    ASSERT(pCtx->pJoin->build->blk == pGrp->blk);
    MJOIN_SAVE_TB_BLK(&pCtx->cache, pCtx->pJoin->build);
  }

  return TSDB_CODE_SUCCESS;
}

int32_t mAsofGreaterChkFillGrpCache(SMJoinWindowCtx* pCtx) {
  if (pCtx->cache.rowNum >= pCtx->jLimit || pCtx->pJoin->build->dsFetchDone) {
    return TSDB_CODE_SUCCESS;
  }

  MJ_ERR_RET(mAsofGreaterTrimCacheBlk(pCtx));

  SMJoinTableCtx* build = pCtx->pJoin->build;
  SMJoinWinCache* pCache = &pCtx->cache;
  int32_t grpNum = taosArrayGetSize(pCache->grps);
  if (grpNum >= 1) {
    SMJoinGrpRows* pGrp = taosArrayGet(pCache->grps, grpNum - 1);
    if (pGrp->blk != pCache->outBlk) {
      int32_t beginIdx = (1 == grpNum) ? build->blkRowIdx : 0;
      MJ_ERR_RET(blockDataMergeNRows(pCache->outBlk, pGrp->blk, beginIdx, pGrp->blk->info.rows - beginIdx));
      if (1 == grpNum) {
        pGrp->blk = pCache->outBlk;
        pGrp->beginIdx = 0;
        pGrp->readIdx = 0;
        //pGrp->endIdx = pGrp->blk->info.rows - 1;
      } else {
        taosArrayPop(pCache->grps);
        pGrp = taosArrayGet(pCache->grps, 0);
        ASSERT(pGrp->blk == pCache->outBlk);
        //pGrp->endIdx = pGrp->blk->info.rows - pGrp->beginIdx;
      }
      
      //ASSERT((pGrp->endIdx - pGrp->beginIdx + 1) == pCtx->cache.rowNum);
    }

    
    ASSERT(taosArrayGetSize(pCache->grps) == 1);
    ASSERT(pGrp->blk->info.rows - pGrp->beginIdx == pCtx->cache.rowNum);
  }
  
  do {
    build->blk = (*pCtx->pJoin->retrieveFp)(pCtx->pJoin, build);
    qDebug("%s merge join %s table got block to fill grp, rows:%" PRId64, GET_TASKID(pCtx->pJoin->pOperator->pTaskInfo), MJOIN_TBTYPE(build->type), build->blk ? build->blk->info.rows : 0);
    
    build->blkRowIdx = 0;
    
    if (NULL == build->blk) {
      break;
    }

    if ((pCache->rowNum + build->blk->info.rows) >= pCtx->jLimit) {
      MJOIN_PUSH_BLK_TO_CACHE(pCache, build->blk);
      break;
    }
    
    MJ_ERR_RET(blockDataMergeNRows(pCache->outBlk, build->blk, 0, build->blk->info.rows));
    pCache->rowNum += build->blk->info.rows;
    
    //pGrp->endIdx = pGrp->blk->info.rows - pGrp->beginIdx;
  } while (pCache->rowNum < pCtx->jLimit);

  MJOIN_RESTORE_TB_BLK(pCache, build);

  return TSDB_CODE_SUCCESS;
}

void mAsofGreaterUpdateBuildGrpEndIdx(SMJoinWindowCtx* pCtx) {
  int32_t grpNum = taosArrayGetSize(pCtx->cache.grps);
  if (grpNum <= 0) {
    return;
  }

  SMJoinGrpRows* pGrp = taosArrayGet(pCtx->cache.grps, 0);  
  if (1 == grpNum) {
    pGrp->endIdx = pGrp->beginIdx + TMIN(pGrp->blk->info.rows - pGrp->beginIdx, pCtx->jLimit) - 1;
    return;
  }

  ASSERT(pCtx->jLimit > (pGrp->blk->info.rows - pGrp->beginIdx));
  pGrp->endIdx = pGrp->blk->info.rows - 1;
  
  int64_t remainRows = pCtx->jLimit - (pGrp->endIdx - pGrp->beginIdx + 1);
  
  pGrp = taosArrayGet(pCtx->cache.grps, 1); 
  pGrp->endIdx = pGrp->beginIdx + TMIN(pGrp->blk->info.rows, remainRows) - 1;
}

int32_t mAsofGreaterFillDumpGrpCache(SMJoinWindowCtx* pCtx, bool lastBuildGrp) {
  if (!lastBuildGrp) {
    MJOIN_SAVE_TB_BLK(&pCtx->cache, pCtx->pJoin->build);
    MJ_ERR_RET(mAsofGreaterChkFillGrpCache(pCtx));
  }

  mAsofGreaterUpdateBuildGrpEndIdx(pCtx);
  
  return mWinJoinDumpGrpCache(pCtx);
}

int32_t mAsofGreaterSkipEqRows(SMJoinWindowCtx* pCtx, SMJoinTableCtx* pTable, int64_t timestamp, bool* wholeBlk) {
  SColumnInfoData* pCol = taosArrayGet(pTable->blk->pDataBlock, pTable->primCol->srcSlot);
  
  if (*(int64_t*)colDataGetNumData(pCol, pTable->blkRowIdx) != timestamp) {
    *wholeBlk = false;
    return TSDB_CODE_SUCCESS;
  }

  pTable->blkRowIdx++;
  pCtx->cache.rowNum--;
  
  char* pEndVal = colDataGetNumData(pCol, pTable->blk->info.rows - 1);
  if (timestamp != *(int64_t*)pEndVal) {
    for (; pTable->blkRowIdx < pTable->blk->info.rows; ++pTable->blkRowIdx) {
      char* pNextVal = colDataGetNumData(pCol, pTable->blkRowIdx);
      if (timestamp == *(int64_t*)pNextVal) {
        pCtx->cache.rowNum--;
        continue;
      }

      *wholeBlk = false;  
      return TSDB_CODE_SUCCESS;
    }
  } else {
    pCtx->cache.rowNum -= (pTable->blk->info.rows - pTable->blkRowIdx);
  }

  *wholeBlk = true;
  
  return TSDB_CODE_SUCCESS;
}

int32_t mAsofGreaterSkipAllEqRows(SMJoinWindowCtx* pCtx, int64_t timestamp) {
  SMJoinWinCache* cache = &pCtx->cache;
  SMJoinTableCtx* pTable = pCtx->pJoin->build;
  bool wholeBlk = false;

  do {
    do {
      MJ_ERR_RET(mAsofGreaterSkipEqRows(pCtx, pTable, timestamp, &wholeBlk));
      if (!wholeBlk) {
        return TSDB_CODE_SUCCESS;
      }

      MJOIN_POP_TB_BLK(cache);
      MJOIN_RESTORE_TB_BLK(cache, pTable);
    } while (!MJOIN_BUILD_TB_ROWS_DONE(pTable));

    ASSERT(pCtx->cache.rowNum == 0);
    ASSERT(taosArrayGetSize(pCtx->cache.grps) == 0);

    if (pTable->dsFetchDone) {
      return TSDB_CODE_SUCCESS;
    }
    
    pTable->blk = (*pCtx->pJoin->retrieveFp)(pCtx->pJoin, pTable);
    qDebug("%s merge join %s table got block to skip eq ts, rows:%" PRId64, GET_TASKID(pCtx->pJoin->pOperator->pTaskInfo), MJOIN_TBTYPE(pTable->type), pTable->blk ? pTable->blk->info.rows : 0);

    pTable->blkRowIdx = 0;

    if (NULL == pTable->blk) {
      return TSDB_CODE_SUCCESS;
    }

    MJOIN_PUSH_BLK_TO_CACHE(cache, pTable->blk);
  } while (true);

  return TSDB_CODE_SUCCESS;
}


int32_t mAsofGreaterUpdateDumpEqRows(SMJoinWindowCtx* pCtx, int64_t timestamp, bool lastBuildGrp) {
  if (!pCtx->eqRowsAcq && !lastBuildGrp) {
    MJ_ERR_RET(mAsofGreaterSkipAllEqRows(pCtx, timestamp));
  }

  return mAsofGreaterFillDumpGrpCache(pCtx, lastBuildGrp);
}


int32_t mAsofGreaterProcessEqualGrp(SMJoinWindowCtx* pCtx, int64_t timestamp, bool lastBuildGrp) {
  SMJoinOperatorInfo* pJoin = pCtx->pJoin;

  pCtx->lastEqGrp = true;

  MJ_ERR_RET(mJoinBuildEqGrp(pJoin->probe, timestamp, NULL, &pCtx->probeGrp));

  return mAsofGreaterUpdateDumpEqRows(pCtx, timestamp, lastBuildGrp);
}

int32_t mAsofGreaterProcessLowerGrp(SMJoinWindowCtx* pCtx, SMJoinOperatorInfo* pJoin, SColumnInfoData* pCol,  int64_t* probeTs, int64_t* buildTs) {
  pCtx->lastEqGrp = false;
  
  pCtx->probeGrp.beginIdx = pJoin->probe->blkRowIdx;
  pCtx->probeGrp.readIdx = pCtx->probeGrp.beginIdx;
  pCtx->probeGrp.endIdx = pCtx->probeGrp.beginIdx;
  
  while (++pJoin->probe->blkRowIdx < pJoin->probe->blk->info.rows) {
    MJOIN_GET_TB_CUR_TS(pCol, *probeTs, pJoin->probe);
    if (PROBE_TS_LOWER(pCtx->ascTs, *probeTs, *buildTs)) {
      pCtx->probeGrp.endIdx = pJoin->probe->blkRowIdx;
      continue;
    }
    
    break;
  }

  return mAsofGreaterFillDumpGrpCache(pCtx, false);
}

int32_t mAsofGreaterProcessGreaterGrp(SMJoinWindowCtx* pCtx, SMJoinOperatorInfo* pJoin, SColumnInfoData** pCol,  int64_t* probeTs, int64_t* buildTs) {
  do {
    MJOIN_GET_TB_CUR_TS(*pCol, *buildTs, pJoin->build);
    if (!PROBE_TS_GREATER(pCtx->ascTs, *probeTs, *buildTs)) {
      break;
    }

    pCtx->cache.rowNum--;
    while (++pJoin->build->blkRowIdx < pJoin->build->blk->info.rows) {
      MJOIN_GET_TB_CUR_TS(*pCol, *buildTs, pJoin->build);
      if (PROBE_TS_GREATER(pCtx->ascTs, *probeTs, *buildTs)) {
        pCtx->cache.rowNum--;
        continue;
      }
      
      return TSDB_CODE_SUCCESS;
    }

    MJOIN_POP_TB_BLK(&pCtx->cache);
    MJOIN_RESTORE_TB_BLK(&pCtx->cache, pJoin->build);
    MJOIN_GET_TB_COL_TS(*pCol, *buildTs, pJoin->build);
  } while (!MJOIN_BUILD_TB_ROWS_DONE(pJoin->build));

  return TSDB_CODE_SUCCESS;
}

static bool mAsofGreaterRetrieve(SOperatorInfo* pOperator, SMJoinOperatorInfo* pJoin, SMJoinWindowCtx* pCtx) {
  bool probeGot = mJoinRetrieveBlk(pJoin, &pJoin->probe->blkRowIdx, &pJoin->probe->blk, pJoin->probe);
  bool buildGot = false;

  do {
    if ((probeGot || MJOIN_DS_NEED_INIT(pOperator, pJoin->build)) && pCtx->cache.rowNum < pCtx->jLimit) { 
      pJoin->build->newBlk = false;
      MJOIN_SAVE_TB_BLK(&pCtx->cache, pCtx->pJoin->build);
      ASSERT(taosArrayGetSize(pCtx->cache.grps) <= 1);
      buildGot = mJoinRetrieveBlk(pJoin, &pJoin->build->blkRowIdx, &pJoin->build->blk, pJoin->build);
    }
    
    if (!probeGot) {
      if (!pCtx->groupJoin || NULL == pJoin->probe->remainInBlk) {
        mJoinSetDone(pOperator);
      }

      return false;
    }

    if (buildGot) {
      SColumnInfoData* pProbeCol = taosArrayGet(pJoin->probe->blk->pDataBlock, pJoin->probe->primCol->srcSlot);
      SColumnInfoData* pBuildCol = taosArrayGet(pJoin->build->blk->pDataBlock, pJoin->build->primCol->srcSlot);
      if (*((int64_t*)pProbeCol->pData + pJoin->probe->blkRowIdx) > *((int64_t*)pBuildCol->pData + pJoin->build->blk->info.rows - 1)) {
        pJoin->build->blkRowIdx = pJoin->build->blk->info.rows;
        MJOIN_POP_TB_BLK(&pCtx->cache);
        continue;
      }
    }
    
    break;
  } while (true);

  if (buildGot && pJoin->build->newBlk) {
    if (NULL == pCtx->cache.outBlk) {
      pCtx->cache.outBlk = createOneDataBlock(pJoin->build->blk, false);
      blockDataEnsureCapacity(pCtx->cache.outBlk, pCtx->jLimit);
    }
    
    MJOIN_PUSH_BLK_TO_CACHE(&pCtx->cache, pJoin->build->blk);
    MJOIN_RESTORE_TB_BLK(&pCtx->cache, pJoin->build);
  }

  pCtx->probeGrp.blk = pJoin->probe->blk;

  return true;
}


SSDataBlock* mAsofGreaterJoinDo(struct SOperatorInfo* pOperator) {
  SMJoinOperatorInfo* pJoin = pOperator->info;
  SMJoinWindowCtx* pCtx = &pJoin->ctx.windowCtx;
  int32_t code = TSDB_CODE_SUCCESS;
  int64_t probeTs = 0;
  int64_t buildTs = 0;
  SColumnInfoData* pBuildCol = NULL;
  SColumnInfoData* pProbeCol = NULL;

  blockDataCleanup(pCtx->finBlk);

  if (pCtx->grpRemains) {
    MJ_ERR_JRET(mWinJoinDumpGrpCache(pCtx));
    if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
      return pCtx->finBlk;
    }
    pCtx->grpRemains = false;
  }

  do {
    if (!mAsofGreaterRetrieve(pOperator, pJoin, pCtx)) {
      if (pCtx->groupJoin && pCtx->finBlk->info.rows <= 0 && !mJoinIsDone(pOperator)) {
        continue;
      }

      break;
    }

    MJOIN_GET_TB_COL_TS(pBuildCol, buildTs, pJoin->build);
    MJOIN_GET_TB_COL_TS(pProbeCol, probeTs, pJoin->probe);
    
    if (probeTs == pCtx->lastTs) {
      MJ_ERR_JRET(mAsofGreaterProcessEqualGrp(pCtx, probeTs, true));
      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }

      if (MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe)) {
        continue;
      } else {
        MJOIN_GET_TB_CUR_TS(pProbeCol, probeTs, pJoin->probe);
      }
    }

    while (!MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe) && !MJOIN_BUILD_TB_ROWS_DONE(pJoin->build)) {
      if (probeTs == buildTs) {
        pCtx->lastTs = probeTs;
        MJ_ERR_JRET(mAsofGreaterProcessEqualGrp(pCtx, probeTs, false));
        if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
          return pCtx->finBlk;
        }

        MJOIN_GET_TB_COL_TS(pBuildCol, buildTs, pJoin->build);
        MJOIN_GET_TB_COL_TS(pProbeCol, probeTs, pJoin->probe);
        continue;
      }

      if (PROBE_TS_LOWER(pCtx->ascTs, probeTs, buildTs)) {
        MJ_ERR_JRET(mAsofGreaterProcessLowerGrp(pCtx, pJoin, pProbeCol, &probeTs, &buildTs));
      } else {
        MJ_ERR_JRET(mAsofGreaterProcessGreaterGrp(pCtx, pJoin, &pBuildCol, &probeTs, &buildTs));
      }

      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }
    }

    if (!MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe) && (pJoin->build->dsFetchDone || (pCtx->groupJoin && NULL == pJoin->build->blk))) {
      pCtx->probeGrp.beginIdx = pJoin->probe->blkRowIdx;
      pCtx->probeGrp.readIdx = pCtx->probeGrp.beginIdx;
      pCtx->probeGrp.endIdx = pJoin->probe->blk->info.rows - 1;
      
      MJ_ERR_JRET(mJoinNonEqCart((SMJoinCommonCtx*)pCtx, &pCtx->probeGrp, true, false));
      
      pJoin->probe->blkRowIdx = pJoin->probe->blk->info.rows;
            
      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold) {
        return pCtx->finBlk;
      }
    }
  } while (true);

_return:

  if (code) {
    pJoin->errCode = code;
    return NULL;
  }

  return pCtx->finBlk;
}

void mAsofJoinGroupReset(SMJoinOperatorInfo* pJoin) {
  SMJoinWindowCtx* pWin = &pJoin->ctx.windowCtx;
  SMJoinWinCache* pCache = &pWin->cache;

  pWin->lastEqGrp = false;
  pWin->lastProbeGrp = false;
  pWin->eqPostDone = false;
  pWin->lastTs = INT64_MIN;

  pCache->outRowIdx = 0;
  pCache->rowNum = 0;
  pCache->grpIdx = 0;

  if (pCache->grpsQueue) {
    TSWAP(pCache->grps, pCache->grpsQueue);
  }
  
  taosArrayClear(pCache->grps);
  
  if (pCache->outBlk) {
    blockDataCleanup(pCache->outBlk);
  }

  mJoinResetGroupTableCtx(pJoin->probe);
  mJoinResetGroupTableCtx(pJoin->build);    
}


static int32_t mWinJoinCloneCacheBlk(SMJoinWindowCtx* pCtx) {
  SMJoinWinCache* pCache = &pCtx->cache;
  int32_t grpNum = taosArrayGetSize(pCache->grps);
  if (grpNum <= 0) {
    return TSDB_CODE_SUCCESS;
  }

  SMJoinGrpRows* pGrp = (SMJoinGrpRows*)taosArrayGetLast(pCache->grps);
  if (!pGrp->clonedBlk) {
    if (0 == pGrp->beginIdx) {
      pGrp->blk = createOneDataBlock(pGrp->blk, true);
    } else {
      pGrp->blk = blockDataExtractBlock(pGrp->blk, pGrp->beginIdx, pGrp->blk->info.rows - pGrp->beginIdx);
      pGrp->endIdx -= pGrp->beginIdx;
      pGrp->beginIdx = 0;
      pGrp->readIdx = 0;
    }
    
    pGrp->clonedBlk = true;
  }

  return TSDB_CODE_SUCCESS;
}

static bool mWinJoinRetrieve(SOperatorInfo* pOperator, SMJoinOperatorInfo* pJoin, SMJoinWindowCtx* pCtx) {
  bool probeGot = mJoinRetrieveBlk(pJoin, &pJoin->probe->blkRowIdx, &pJoin->probe->blk, pJoin->probe);
  bool buildGot = false;

  do {
    if (probeGot || MJOIN_DS_NEED_INIT(pOperator, pJoin->build)) { 
      if (NULL == pJoin->build->blk) {
        mWinJoinCloneCacheBlk(pCtx);
      }
      
      buildGot = mJoinRetrieveBlk(pJoin, &pJoin->build->blkRowIdx, &pJoin->build->blk, pJoin->build);
    }
    
    if (!probeGot) {
      if (!pCtx->groupJoin || NULL == pJoin->probe->remainInBlk) {
        mJoinSetDone(pOperator);
      }
      
      return false;
    }

    if (buildGot && !pCtx->lowerRowsAcq) {
      SColumnInfoData* pProbeCol = taosArrayGet(pJoin->probe->blk->pDataBlock, pJoin->probe->primCol->srcSlot);
      SColumnInfoData* pBuildCol = taosArrayGet(pJoin->build->blk->pDataBlock, pJoin->build->primCol->srcSlot);
      if (*((int64_t*)pProbeCol->pData + pJoin->probe->blkRowIdx) > *((int64_t*)pBuildCol->pData + pJoin->build->blk->info.rows - 1)) {
        pJoin->build->blkRowIdx = pJoin->build->blk->info.rows;
        continue;
      }
    }
    
    break;
  } while (true);

  pCtx->probeGrp.blk = pJoin->probe->blk;

  return true;
}

int32_t mWinJoinAddWinBeginBlk(SMJoinWindowCtx* pCtx, SMJoinWinCache* pCache, SMJoinTableCtx* build, bool* winEnd) {
  SSDataBlock* pBlk = build->blk;
  SColumnInfoData* pCol = taosArrayGet(pBlk->pDataBlock, build->primCol->srcSlot);
  if (*((int64_t*)pCol->pData + pBlk->info.rows - 1) >= pCtx->winBeginTs) {
    for (; build->blkRowIdx < pBlk->info.rows; build->blkRowIdx++) {
      if (*((int64_t*)pCol->pData + build->blkRowIdx) < pCtx->winBeginTs) {
        continue;
      }
  
      if (*((int64_t*)pCol->pData + build->blkRowIdx) <= pCtx->winEndTs) {
        SMJoinGrpRows grp = {.blk = pBlk, .beginIdx = build->blkRowIdx};
        SMJoinGrpRows* pGrp = taosArrayPush(pCache->grps, &grp);
  
        pGrp->readIdx = pGrp->beginIdx;
        pGrp->endIdx = pGrp->beginIdx;
  
        build->blk = NULL;
        pCache->rowNum = 1;
      } else {
        pCache->rowNum = 0;
      }

      *winEnd = true;  
      return TSDB_CODE_SUCCESS;
    }
  }

  pCache->rowNum = 0;

  *winEnd = false;
  return TSDB_CODE_SUCCESS;
}


int32_t mWinJoinAddWinEndBlk(SMJoinWindowCtx* pCtx, SMJoinWinCache* pCache, SMJoinTableCtx* build, bool* winEnd) {
  SSDataBlock* pBlk = build->blk;
  SColumnInfoData* pCol = taosArrayGet(pBlk->pDataBlock, build->primCol->srcSlot);
  SMJoinGrpRows grp = {.blk = pBlk, .beginIdx = build->blkRowIdx};

  if (*((int64_t*)pCol->pData + build->blkRowIdx) > pCtx->winEndTs) {
    *winEnd = true;
    return TSDB_CODE_SUCCESS;
  }

  if (*((int64_t*)pCol->pData + pBlk->info.rows - 1) <= pCtx->winEndTs) {
    SMJoinGrpRows* pGrp = taosArrayPush(pCache->grps, &grp);
    
    pGrp->readIdx = pGrp->beginIdx;
    pGrp->endIdx = pBlk->info.rows - 1;

    pCache->rowNum += (pGrp->endIdx - pGrp->beginIdx + 1);
    if (pCache->rowNum >= pCtx->jLimit) {
      pGrp->endIdx = pBlk->info.rows - 1 + pCtx->jLimit - pCache->rowNum;
      pCache->rowNum = pCtx->jLimit;

      build->blk = NULL;
      *winEnd = true;
      return TSDB_CODE_SUCCESS;
    }
    
    build->blk = NULL;
    *winEnd = false;
    return TSDB_CODE_SUCCESS;
  }

  for (; build->blkRowIdx < pBlk->info.rows && pCache->rowNum < pCtx->jLimit; build->blkRowIdx++) {
    if (*((int64_t*)pCol->pData + build->blkRowIdx) <= pCtx->winEndTs) {
      pCache->rowNum++;
      continue;
    }

    break;
  }

  SMJoinGrpRows* pGrp = taosArrayPush(pCache->grps, &grp);
  
  pGrp->readIdx = pGrp->beginIdx;
  pGrp->endIdx = build->blkRowIdx - 1;
  
  build->blk = NULL;
  *winEnd = true;  

  return TSDB_CODE_SUCCESS;
}


int32_t mWinJoinMoveWinBegin(SMJoinWindowCtx* pCtx) {
  SMJoinWinCache* pCache = &pCtx->cache;
  do {
    int32_t grpNum = taosArrayGetSize(pCache->grps);
    for (int32_t i = 0; i < grpNum; ++i) {
      SMJoinGrpRows* pGrp = taosArrayGet(pCache->grps, i);
      SColumnInfoData* pCol = taosArrayGet(pGrp->blk->pDataBlock, pCtx->pJoin->build->primCol->srcSlot);
      if (*((int64_t*)pCol->pData + pGrp->blk->info.rows - 1) < pCtx->winBeginTs) {
        pCache->rowNum -= (pGrp->blk->info.rows - pGrp->beginIdx);
        if (pGrp->blk == pCache->outBlk) {
          blockDataCleanup(pGrp->blk);
        }
        
        taosArrayPopFrontBatch(pCache->grps, 1);
        grpNum--;
        i--;
        continue;
      }

      int32_t startIdx = pGrp->beginIdx;
      for (; pGrp->beginIdx < pGrp->blk->info.rows; pGrp->beginIdx++) {
        if (*((int64_t*)pCol->pData + pGrp->beginIdx) < pCtx->winBeginTs) {
          continue;
        }

        if (*((int64_t*)pCol->pData + pGrp->beginIdx) <= pCtx->winEndTs) {
          pGrp->readIdx = pGrp->beginIdx;
          if (pGrp->endIdx < pGrp->beginIdx) {
            pGrp->endIdx = pGrp->beginIdx;
            pCache->rowNum = 1;
          } else {
            pCache->rowNum -= (pGrp->beginIdx - startIdx);
          }
          return TSDB_CODE_SUCCESS;
        }

        pGrp->endIdx = pGrp->beginIdx;
        pCache->rowNum = 0;
        TSWAP(pCache->grps, pCache->grpsQueue);
        return TSDB_CODE_SUCCESS;
      }
    }

    if (NULL != pCache->grpsQueue) {
      pCache->grps = pCache->grpsQueue;
      pCache->rowNum = 1;
      pCache->grpsQueue = NULL;
      continue;
    }

    break;
  } while (true);

  SMJoinTableCtx* build = pCtx->pJoin->build;
  bool winEnd = false;
  if (NULL != build->blk) {
    MJ_ERR_RET(mWinJoinAddWinBeginBlk(pCtx, &pCtx->cache, build, &winEnd));
    if (winEnd || taosArrayGetSize(pCache->grps) > 0) {
      return TSDB_CODE_SUCCESS;
    }
  }

  if (build->dsFetchDone) {
    goto _return;
  }
  
  do {
    build->blk = (*pCtx->pJoin->retrieveFp)(pCtx->pJoin, pCtx->pJoin->build);
    qDebug("%s merge join %s table got block to start win, rows:%" PRId64, GET_TASKID(pCtx->pJoin->pOperator->pTaskInfo), MJOIN_TBTYPE(build->type), build->blk ? build->blk->info.rows : 0);
    
    build->blkRowIdx = 0;
    
    if (NULL == build->blk) {
      break;
    }

    MJ_ERR_RET(mWinJoinAddWinBeginBlk(pCtx, &pCtx->cache, build, &winEnd));
    if (winEnd || taosArrayGetSize(pCache->grps) > 0) {
      return TSDB_CODE_SUCCESS;
    }
  } while (true);

_return:

  return TSDB_CODE_SUCCESS;
}


int32_t mWinJoinMoveWinEnd(SMJoinWindowCtx* pCtx) {
  SMJoinWinCache* pCache = &pCtx->cache;
  int32_t grpNum = taosArrayGetSize(pCache->grps);
  if (grpNum <= 0 || pCache->rowNum >= pCtx->jLimit) {
    return TSDB_CODE_SUCCESS;
  }
  
  SMJoinGrpRows* pGrp = taosArrayGetLast(pCache->grps);
  SColumnInfoData* pCol = taosArrayGet(pGrp->blk->pDataBlock, pCtx->pJoin->build->primCol->srcSlot);
  if (*((int64_t*)pCol->pData + pGrp->blk->info.rows - 1) <= pCtx->winEndTs) {
    pCache->rowNum += pGrp->blk->info.rows - pGrp->endIdx - 1;
    if (pCache->rowNum >= pCtx->jLimit) {
      pGrp->endIdx = pGrp->blk->info.rows - 1 + pCtx->jLimit - pCache->rowNum;
      pCache->rowNum = pCtx->jLimit;

      return TSDB_CODE_SUCCESS;
    } else {
      pGrp->endIdx = pGrp->blk->info.rows - 1;
    }
  } else {
    int32_t startIdx = pGrp->endIdx;
    for (; pCache->rowNum < pCtx->jLimit && ++pGrp->endIdx < pGrp->blk->info.rows; ) {
      if (*((int64_t*)pCol->pData + pGrp->endIdx) <= pCtx->winEndTs) {
        pCache->rowNum++;
        if ((pGrp->endIdx + 1) >= pGrp->blk->info.rows) {
          break;
        }
        
        continue;
      }

      ASSERT(pGrp->endIdx > startIdx);
      
      pGrp->endIdx--;
      break;
    }

    return TSDB_CODE_SUCCESS;
  }

  SMJoinTableCtx* build = pCtx->pJoin->build;
  bool winEnd = false;
  if (NULL != build->blk) {
    MJ_ERR_RET(mWinJoinAddWinEndBlk(pCtx, &pCtx->cache, build, &winEnd));
    if (winEnd) {
      return TSDB_CODE_SUCCESS;
    }
  }

  if (build->dsFetchDone) {
    goto _return;
  }

  do {
    MJ_ERR_RET(mWinJoinCloneCacheBlk(pCtx));
    
    build->blk = (*pCtx->pJoin->retrieveFp)(pCtx->pJoin, pCtx->pJoin->build);
    qDebug("%s merge join %s table got block to start win, rows:%" PRId64, GET_TASKID(pCtx->pJoin->pOperator->pTaskInfo), MJOIN_TBTYPE(build->type), build->blk ? build->blk->info.rows : 0);
    
    build->blkRowIdx = 0;
    
    if (NULL == build->blk) {
      break;
    }

    MJ_ERR_RET(mWinJoinAddWinEndBlk(pCtx, &pCtx->cache, build, &winEnd));
    if (winEnd) {
      return TSDB_CODE_SUCCESS;
    }
  } while (true);

_return:

  return TSDB_CODE_SUCCESS;
}


int32_t mWinJoinMoveFillWinCache(SMJoinWindowCtx* pCtx) {
  MJ_ERR_RET(mWinJoinMoveWinBegin(pCtx));
  MJ_ERR_RET(mWinJoinMoveWinEnd(pCtx));

  return TSDB_CODE_SUCCESS;
}

SSDataBlock* mWinJoinDo(struct SOperatorInfo* pOperator) {
  SMJoinOperatorInfo* pJoin = pOperator->info;
  SMJoinWindowCtx* pCtx = &pJoin->ctx.windowCtx;
  int32_t code = TSDB_CODE_SUCCESS;
  int64_t probeTs = 0;
  SColumnInfoData* pProbeCol = NULL;

  blockDataCleanup(pCtx->finBlk);

  if (pCtx->grpRemains) {
    MJ_ERR_JRET(mWinJoinDumpGrpCache(pCtx));
    if (pCtx->finBlk->info.rows >= pCtx->blkThreshold || (pCtx->finBlk->info.rows > 0 && pCtx->seqWinGrp)) {
      return pCtx->finBlk;
    }
    pCtx->grpRemains = false;
  }

  do {
    if (!mWinJoinRetrieve(pOperator, pJoin, pCtx)) {
      if (pCtx->groupJoin && pCtx->finBlk->info.rows <= 0 && !mJoinIsDone(pOperator)) {
        continue;
      }
      
      break;
    }

    MJOIN_GET_TB_COL_TS(pProbeCol, probeTs, pJoin->probe);

    while (!MJOIN_PROBE_TB_ROWS_DONE(pJoin->probe)) {
      MJOIN_GET_TB_CUR_TS(pProbeCol, probeTs, pJoin->probe);

      MJ_ERR_JRET(mJoinBuildEqGrp(pJoin->probe, probeTs, NULL, &pCtx->probeGrp));
      
      if (probeTs != pCtx->lastTs) {
        pCtx->lastTs = probeTs;
        pCtx->winBeginTs = probeTs + pCtx->winBeginOffset;
        pCtx->winEndTs = probeTs + pCtx->winEndOffset;
        MJ_ERR_JRET(mWinJoinMoveFillWinCache(pCtx));
      }

      MJ_ERR_JRET(mWinJoinDumpGrpCache(pCtx));
      
      if (pCtx->finBlk->info.rows >= pCtx->blkThreshold || (pCtx->finBlk->info.rows > 0 && pCtx->seqWinGrp)) {
        return pCtx->finBlk;
      }
    }
  } while (true);

_return:

  if (code) {
    pJoin->errCode = code;
    return NULL;
  }

  return pCtx->finBlk;
}

void mWinJoinGroupReset(SMJoinOperatorInfo* pJoin) {
  SMJoinWindowCtx* pWin = &pJoin->ctx.windowCtx;
  SMJoinWinCache* pCache = &pWin->cache;

  pWin->lastEqGrp = false;
  pWin->lastProbeGrp = false;
  pWin->eqPostDone = false;
  pWin->lastTs = INT64_MIN;

  pCache->outRowIdx = 0;
  pCache->rowNum = 0;
  pCache->grpIdx = 0;

  if (pCache->grpsQueue) {
    TSWAP(pCache->grps, pCache->grpsQueue);
  }
  
  taosArrayClear(pCache->grps);
  
  if (pCache->outBlk) {
    blockDataCleanup(pCache->outBlk);
  }

  mJoinResetGroupTableCtx(pJoin->probe);
  mJoinResetGroupTableCtx(pJoin->build);  
}

int32_t mJoinInitWindowCache(SMJoinWinCache* pCache, SMJoinOperatorInfo* pJoin, SMJoinWindowCtx* pCtx) {
  pCache->pageLimit = MJOIN_BLK_SIZE_LIMIT;
  pCache->colNum = pJoin->build->finNum;
  
  pCache->grps = taosArrayInit(2, sizeof(SMJoinGrpRows));
  if (NULL == pCache->grps) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  //taosArrayReserve(pTable->eqGrps, 1);
  
  return TSDB_CODE_SUCCESS;
}

void mJoinDestroyWindowCtx(SMJoinOperatorInfo* pJoin) {
  SMJoinWindowCtx* pCtx = &pJoin->ctx.windowCtx;

  pCtx->finBlk = blockDataDestroy(pCtx->finBlk);
  pCtx->cache.outBlk = blockDataDestroy(pCtx->cache.outBlk);

  taosArrayDestroy(pCtx->cache.grps);
}

int32_t mJoinInitWindowCtx(SMJoinOperatorInfo* pJoin, SSortMergeJoinPhysiNode* pJoinNode) {
  SMJoinWindowCtx* pCtx = &pJoin->ctx.windowCtx;
  
  pCtx->pJoin = pJoin;
  pCtx->lastTs = INT64_MIN;
  pCtx->seqWinGrp = pJoinNode->seqWinGroup;
  if (pCtx->seqWinGrp) {
    pJoin->outGrpId = 1;
  }

  switch (pJoinNode->subType) {
    case JOIN_STYPE_ASOF:
      pCtx->asofOpType = pJoinNode->asofOpType;
      pCtx->jLimit = pJoinNode->pJLimit ? ((SLimitNode*)pJoinNode->pJLimit)->limit : 1;
      pCtx->eqRowsAcq = ASOF_EQ_ROW_INCLUDED(pCtx->asofOpType);
      pCtx->lowerRowsAcq = (JOIN_TYPE_RIGHT != pJoin->joinType) ? ASOF_LOWER_ROW_INCLUDED(pCtx->asofOpType) : ASOF_GREATER_ROW_INCLUDED(pCtx->asofOpType);
      pCtx->greaterRowsAcq = (JOIN_TYPE_RIGHT != pJoin->joinType) ? ASOF_GREATER_ROW_INCLUDED(pCtx->asofOpType) : ASOF_LOWER_ROW_INCLUDED(pCtx->asofOpType);

      if (pCtx->lowerRowsAcq) {
        pJoin->joinFp = mAsofLowerJoinDo;
      } else if (pCtx->greaterRowsAcq) {
        pJoin->joinFp = mAsofGreaterJoinDo;
      }
      pJoin->grpResetFp = mAsofJoinGroupReset;
      break;
    case JOIN_STYPE_WIN: {
      SWindowOffsetNode* pOffsetNode = (SWindowOffsetNode*)pJoinNode->pWindowOffset;
      SValueNode* pWinBegin = (SValueNode*)pOffsetNode->pStartOffset;
      SValueNode* pWinEnd = (SValueNode*)pOffsetNode->pEndOffset;
      pCtx->jLimit = pJoinNode->pJLimit ? ((SLimitNode*)pJoinNode->pJLimit)->limit : INT64_MAX;
      pCtx->winBeginOffset = pWinBegin->datum.i;
      pCtx->winEndOffset = pWinEnd->datum.i;
      pCtx->eqRowsAcq = (pCtx->winBeginOffset <= 0 && pCtx->winEndOffset >= 0);
      pCtx->lowerRowsAcq = pCtx->winBeginOffset < 0;
      pCtx->greaterRowsAcq = pCtx->winEndOffset > 0;
      break;
    }
    default:
      break;
  }

  if (pJoinNode->node.inputTsOrder != ORDER_DESC) {
    pCtx->ascTs = true;
  }

  pCtx->finBlk = createDataBlockFromDescNode(pJoinNode->node.pOutputDataBlockDesc);
  blockDataEnsureCapacity(pCtx->finBlk, TMAX(MJOIN_DEFAULT_BLK_ROWS_NUM, MJOIN_BLK_SIZE_LIMIT/pJoinNode->node.pOutputDataBlockDesc->totalRowSize));

  pCtx->blkThreshold = pCtx->finBlk->info.capacity * 0.9;

  MJ_ERR_RET(mJoinInitWindowCache(&pCtx->cache, pJoin, pCtx));
  
  return TSDB_CODE_SUCCESS;
}

void mJoinDestroyMergeCtx(SMJoinOperatorInfo* pJoin) {
  SMJoinMergeCtx* pCtx = &pJoin->ctx.mergeCtx;

  pCtx->finBlk = blockDataDestroy(pCtx->finBlk);
  pCtx->midBlk = blockDataDestroy(pCtx->midBlk);
}


int32_t mJoinInitMergeCtx(SMJoinOperatorInfo* pJoin, SSortMergeJoinPhysiNode* pJoinNode) {
  SMJoinMergeCtx* pCtx = &pJoin->ctx.mergeCtx;

  pCtx->pJoin = pJoin;
  pCtx->lastEqTs = INT64_MIN;
  pCtx->hashCan = pJoin->probe->keyNum > 0;

  if (JOIN_STYPE_ASOF == pJoinNode->subType || JOIN_STYPE_WIN == pJoinNode->subType) {
    pCtx->jLimit = pJoinNode->pJLimit ? ((SLimitNode*)pJoinNode->pJLimit)->limit : 1;
    pJoin->subType = JOIN_STYPE_OUTER;
    pJoin->build->eqRowLimit = pCtx->jLimit;
    pJoin->grpResetFp = mLeftJoinGroupReset;
  } else {
    pCtx->jLimit = -1;
  }
    
  if (pJoinNode->node.inputTsOrder != ORDER_DESC) {
    pCtx->ascTs = true;
  }

  pCtx->finBlk = createDataBlockFromDescNode(pJoinNode->node.pOutputDataBlockDesc);
  blockDataEnsureCapacity(pCtx->finBlk, TMAX(MJOIN_DEFAULT_BLK_ROWS_NUM, MJOIN_BLK_SIZE_LIMIT/pJoinNode->node.pOutputDataBlockDesc->totalRowSize));

  if (pJoin->pFPreFilter) {
    pCtx->midBlk = createOneDataBlock(pCtx->finBlk, false);
    blockDataEnsureCapacity(pCtx->midBlk, pCtx->finBlk->info.capacity);
  }

  pCtx->blkThreshold = pCtx->finBlk->info.capacity * 0.9;

  switch (pJoin->joinType) {
    case JOIN_TYPE_INNER:
      pCtx->hashCartFp = (joinCartFp)mInnerJoinHashCart;
      pCtx->mergeCartFp = (joinCartFp)mInnerJoinMergeCart;
      break;
    case JOIN_TYPE_LEFT:
    case JOIN_TYPE_RIGHT: {
      switch (pJoin->subType) {
        case JOIN_STYPE_OUTER:          
          pCtx->hashCartFp = (joinCartFp)mLeftJoinHashCart;
          pCtx->mergeCartFp = (joinCartFp)mLeftJoinMergeCart;
          break;
        case JOIN_STYPE_SEMI: 
          pCtx->hashCartFp = (joinCartFp)mSemiJoinHashCart;
          pCtx->mergeCartFp = (joinCartFp)mSemiJoinMergeCart;
          break;
        case JOIN_STYPE_ANTI:
          pCtx->hashCartFp = (joinCartFp)mAntiJoinHashCart;
          pCtx->mergeCartFp = (joinCartFp)mAntiJoinMergeCart;
          break;
        default:
          break;
      }
      break;
    }
    case JOIN_TYPE_FULL:
      pCtx->hashCartFp = (joinCartFp)mFullJoinHashCart;
      pCtx->mergeCartFp = (joinCartFp)mFullJoinMergeCart;
      break;
    default:
      break;
  }
  
  return TSDB_CODE_SUCCESS;
}


