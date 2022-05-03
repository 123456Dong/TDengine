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

#ifndef TDENGINE_TUDF_H
#define TDENGINE_TUDF_H


#include <stdint.h>
#include <stdbool.h>
#include "tmsg.h"
#include "tcommon.h"
#include "function.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UDF_LISTEN_PIPE_NAME_LEN 32
#define UDF_LISTEN_PIPE_NAME_PREFIX "udfd.sock."
#define UDF_DNODE_ID_ENV_NAME "DNODE_ID"

//======================================================================================
//begin API to taosd and qworker

enum {
  UDFC_CODE_STOPPING = -1,
  UDFC_CODE_PIPE_READ_ERR = -2,
  UDFC_CODE_CONNECT_PIPE_ERR = -3,
  UDFC_CODE_LOAD_UDF_FAILURE = -4,
  UDFC_CODE_INVALID_STATE = -5
};

typedef void *UdfcFuncHandle;

/**
 * setup udf
 * @param udf, in
 * @param handle, out
 * @return error code
 */
int32_t setupUdf(char udfName[], UdfcFuncHandle *handle);

typedef struct SUdfColumnMeta {
  int16_t type;
  int32_t bytes;
  uint8_t precision;
  uint8_t scale;
} SUdfColumnMeta;

typedef struct SUdfColumnData {
  int32_t numOfRows;
  bool varLengthColumn;
  union {
    struct {
      int32_t nullBitmapLen;
      char   *nullBitmap;
      int32_t dataLen;
      char   *data;
    } fixLenCol;

    struct {
      int32_t varOffsetsLen;
      char   *varOffsets;
      int32_t payloadLen;
      char   *payload;
    } varLenCol;
  };
} SUdfColumnData;


typedef struct SUdfColumn {
  SUdfColumnMeta colMeta;
  SUdfColumnData colData;
} SUdfColumn;

typedef struct SUdfDataBlock {
  int32_t numOfRows;
  int32_t numOfCols;
  SUdfColumn **udfCols;
} SUdfDataBlock;

typedef struct SUdfInterBuf {
  int32_t bufLen;
  char* buf;
  int8_t numOfResult; //zero or one
} SUdfInterBuf;

// output: interBuf
int32_t callUdfAggInit(UdfcFuncHandle handle, SUdfInterBuf *interBuf);
// input: block, state
// output: newState
int32_t callUdfAggProcess(UdfcFuncHandle handle, SSDataBlock *block, SUdfInterBuf *state, SUdfInterBuf *newState);
// input: interBuf
// output: resultData
int32_t callUdfAggFinalize(UdfcFuncHandle handle, SUdfInterBuf *interBuf, SUdfInterBuf *resultData);
// input: interbuf1, interbuf2
// output: resultBuf
int32_t callUdfAggMerge(UdfcFuncHandle handle, SUdfInterBuf *interBuf1, SUdfInterBuf *interBuf2, SUdfInterBuf *resultBuf);
// input: block
// output: resultData
int32_t callUdfScalarFunc(UdfcFuncHandle handle, SScalarParam *input, int32_t numOfCols, SScalarParam *output);
/**
 * tearn down udf
 * @param handle
 * @return
 */
int32_t teardownUdf(UdfcFuncHandle handle);

bool udfAggGetEnv(struct SFunctionNode* pFunc, SFuncExecEnv* pEnv);
bool udfAggInit(struct SqlFunctionCtx *pCtx, struct SResultRowEntryInfo* pResultCellInfo);
int32_t udfAggProcess(struct SqlFunctionCtx *pCtx);
int32_t udfAggFinalize(struct SqlFunctionCtx *pCtx, SSDataBlock* pBlock);
// end API to taosd and qworker
//=============================================================================================================================
// begin API to UDF writer.

// dynamic lib init and destroy
typedef int32_t (*TUdfSetupFunc)();
typedef int32_t (*TUdfTeardownFunc)();

//TODO: add API to check function arguments type, number etc.
//TODO: another way to manage memory is provide api for UDF to add data to SUdfColumnData and UDF framework will allocate memory.
// then UDF framework will free the memory
// int32_t udfColDataAppend(SUdfColumn* pColumn, uint32_t currentRow, const char* pData, bool isNull)

typedef int32_t (*TUdfFreeUdfColumnFunc)(SUdfColumn* column);
typedef int32_t (*TUdfScalarProcFunc)(SUdfDataBlock* block, SUdfColumn *resultCol);

typedef int32_t (*TUdfAggStartFunc)(SUdfInterBuf *buf);
typedef int32_t (*TUdfAggProcessFunc)(SUdfDataBlock* block, SUdfInterBuf *interBuf, SUdfInterBuf *newInterBuf);
typedef int32_t (*TUdfAggFinishFunc)(SUdfInterBuf* buf, SUdfInterBuf *resultData);


// end API to UDF writer
//=======================================================================================================================

#ifdef __cplusplus
}
#endif

#endif  // TDENGINE_TUDF_H
