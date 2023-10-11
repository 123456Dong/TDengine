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

#include "streamSnapshot.h"
#include "query.h"
#include "rocksdb/c.h"
#include "streamBackendRocksdb.h"
#include "streamInt.h"
#include "tcommon.h"

enum SBackendFileType {
  ROCKSDB_OPTIONS_TYPE = 1,
  ROCKSDB_MAINFEST_TYPE = 2,
  ROCKSDB_SST_TYPE = 3,
  ROCKSDB_CURRENT_TYPE = 4,
  ROCKSDB_CHECKPOINT_META_TYPE = 5,
};

typedef struct SBackendFileItem {
  char*   name;
  int8_t  type;
  int64_t size;
} SBackendFileItem;
typedef struct SBackendFile {
  char*   pCurrent;
  char*   pMainfest;
  char*   pOptions;
  SArray* pSst;
  char*   pCheckpointMeta;
  char*   path;

} SBanckendFile;

typedef struct SBackendSnapFiles2 {
  char*   pCurrent;
  char*   pMainfest;
  char*   pOptions;
  SArray* pSst;
  char*   pCheckpointMeta;
  char*   path;

  int64_t         checkpointId;
  int64_t         seraial;
  int64_t         offset;
  TdFilePtr       fd;
  int8_t          filetype;
  SArray*         pFileList;
  int32_t         currFileIdx;
  SStreamTaskSnap snapInfo;
  int8_t          inited;

} SBackendSnapFile2;
struct SStreamSnapHandle {
  void*          handle;
  SBanckendFile* pBackendFile;
  int64_t        checkpointId;
  int64_t        seraial;
  int64_t        offset;
  TdFilePtr      fd;
  int8_t         filetype;
  SArray*        pFileList;
  int32_t        currFileIdx;
  char*          metaPath;

  SArray* pBackendSnapSet;
  int32_t currIdx;
};
struct SStreamSnapBlockHdr {
  int8_t  type;
  int8_t  flag;
  int64_t index;
  // int64_t streamId;
  // int64_t taskId;
  SStreamTaskSnap snapInfo;
  char            name[128];
  int64_t         totalSize;
  int64_t         size;
  uint8_t         data[];
};
struct SStreamSnapReader {
  void*             pMeta;
  int64_t           sver;
  int64_t           ever;
  SStreamSnapHandle handle;
  int64_t           checkpointId;
};
struct SStreamSnapWriter {
  void*             pMeta;
  int64_t           sver;
  int64_t           ever;
  SStreamSnapHandle handle;
};
const char*    ROCKSDB_OPTIONS = "OPTIONS";
const char*    ROCKSDB_MAINFEST = "MANIFEST";
const char*    ROCKSDB_SST = "sst";
const char*    ROCKSDB_CURRENT = "CURRENT";
const char*    ROCKSDB_CHECKPOINT_META = "CHECKPOINT";
static int64_t kBlockSize = 64 * 1024;

int32_t streamSnapHandleInit(SStreamSnapHandle* handle, char* path, int64_t chkpId, void* pMeta);
void    streamSnapHandleDestroy(SStreamSnapHandle* handle);

// static void streamBuildFname(char* path, char* file, char* fullname)

#define STREAM_ROCKSDB_BUILD_FULLNAME(path, file, fullname) \
  do {                                                      \
    sprintf(fullname, "%s%s%s", path, TD_DIRSEP, file);     \
  } while (0)

int32_t streamGetFileSize(char* path, char* name, int64_t* sz) {
  int ret = 0;

  char* fullname = taosMemoryCalloc(1, strlen(path) + 32);
  sprintf(fullname, "%s%s%s", path, TD_DIRSEP, name);

  ret = taosStatFile(fullname, sz, NULL, NULL);
  taosMemoryFree(fullname);

  return ret;
}

TdFilePtr streamOpenFile(char* path, char* name, int32_t opt) {
  char fullname[256] = {0};
  STREAM_ROCKSDB_BUILD_FULLNAME(path, name, fullname);
  return taosOpenFile(fullname, opt);
}

int32_t streamBackendGetSnapInfo(void* arg, char* path, int64_t chkpId) { return taskDbBuildSnap(arg, chkpId); }

void snapFileDebugInfo(SBackendSnapFile2* pSnapFile) {
  if (qDebugFlag & DEBUG_DEBUG) {
    char* buf = taosMemoryCalloc(1, 512);
    sprintf(buf, "[current: %s,", pSnapFile->pCurrent);
    sprintf(buf + strlen(buf), "MANIFEST: %s,", pSnapFile->pMainfest);
    sprintf(buf + strlen(buf), "options: %s,", pSnapFile->pOptions);
    if (pSnapFile->pSst) {
      for (int i = 0; i < taosArrayGetSize(pSnapFile->pSst); i++) {
        char* name = taosArrayGetP(pSnapFile->pSst, i);
        sprintf(buf + strlen(buf), "%s,", name);
      }
    }
    sprintf(buf + strlen(buf) - 1, "]");

    qInfo("%s %" PRId64 "-%" PRId64 " get file list: %s", STREAM_STATE_TRANSFER, pSnapFile->snapInfo.streamId,
          pSnapFile->snapInfo.taskId, buf);
    taosMemoryFree(buf);
  }
}

int32_t snapFileCvtMeta(SBackendSnapFile2* pSnapFile) {
  SBackendFileItem item;
  // current
  item.name = pSnapFile->pCurrent;
  item.type = ROCKSDB_CURRENT_TYPE;
  streamGetFileSize(pSnapFile->path, item.name, &item.size);
  taosArrayPush(pSnapFile->pFileList, &item);

  // mainfest
  item.name = pSnapFile->pMainfest;
  item.type = ROCKSDB_MAINFEST_TYPE;
  streamGetFileSize(pSnapFile->path, item.name, &item.size);
  taosArrayPush(pSnapFile->pFileList, &item);

  // options
  item.name = pSnapFile->pOptions;
  item.type = ROCKSDB_OPTIONS_TYPE;
  streamGetFileSize(pSnapFile->path, item.name, &item.size);
  taosArrayPush(pSnapFile->pFileList, &item);
  // sst
  for (int i = 0; i < taosArrayGetSize(pSnapFile->pSst); i++) {
    char* sst = taosArrayGetP(pSnapFile->pSst, i);
    item.name = sst;
    item.type = ROCKSDB_SST_TYPE;
    streamGetFileSize(pSnapFile->path, item.name, &item.size);
    taosArrayPush(pSnapFile->pFileList, &item);
  }
  // meta
  item.name = pSnapFile->pCheckpointMeta;
  item.type = ROCKSDB_CHECKPOINT_META_TYPE;
  if (streamGetFileSize(pSnapFile->path, item.name, &item.size) == 0) {
    taosArrayPush(pSnapFile->pFileList, &item);
  }
  return 0;
}
int32_t snapFileReadMeta(SBackendSnapFile2* pSnapFile) {
  TdDirPtr pDir = taosOpenDir(pSnapFile->path);
  if (NULL == pDir) {
    qError("%s failed to open %s", STREAM_STATE_TRANSFER, pSnapFile->path);
    return -1;
  }

  TdDirEntryPtr pDirEntry;
  while ((pDirEntry = taosReadDir(pDir)) != NULL) {
    char* name = taosGetDirEntryName(pDirEntry);
    if (strlen(name) >= strlen(ROCKSDB_CURRENT) && 0 == strncmp(name, ROCKSDB_CURRENT, strlen(ROCKSDB_CURRENT))) {
      pSnapFile->pCurrent = taosStrdup(name);
      continue;
    }
    if (strlen(name) >= strlen(ROCKSDB_MAINFEST) && 0 == strncmp(name, ROCKSDB_MAINFEST, strlen(ROCKSDB_MAINFEST))) {
      pSnapFile->pMainfest = taosStrdup(name);
      continue;
    }
    if (strlen(name) >= strlen(ROCKSDB_OPTIONS) && 0 == strncmp(name, ROCKSDB_OPTIONS, strlen(ROCKSDB_OPTIONS))) {
      pSnapFile->pOptions = taosStrdup(name);
      continue;
    }
    if (strlen(name) >= strlen(ROCKSDB_CHECKPOINT_META) &&
        0 == strncmp(name, ROCKSDB_CHECKPOINT_META, strlen(ROCKSDB_CHECKPOINT_META))) {
      pSnapFile->pCheckpointMeta = taosStrdup(name);
      continue;
    }
    if (strlen(name) >= strlen(ROCKSDB_SST) &&
        0 == strncmp(name + strlen(name) - strlen(ROCKSDB_SST), ROCKSDB_SST, strlen(ROCKSDB_SST))) {
      char* sst = taosStrdup(name);
      taosArrayPush(pSnapFile->pSst, &sst);
    }
  }
  taosCloseDir(&pDir);
  return 0;
}
int32_t streamBackendSnapInitFile(char* path, SStreamTaskSnap* pSnap, SBackendSnapFile2* pSnapFile) {
  // SBanckendFile* pFile = taosMemoryCalloc(1, sizeof(SBanckendFile));
  int32_t code = -1;

  char* snapPath = taosMemoryCalloc(1, strlen(path) + 256);
  sprintf(snapPath, "%s%s%" PRId64 "_%" PRId64 "%s%s%s%s%scheckpoint%" PRId64 "", path, TD_DIRSEP, pSnap->streamId,
          pSnap->taskId, TD_DIRSEP, "state", TD_DIRSEP, "checkpoints", TD_DIRSEP, pSnap->chkpId);
  if (taosIsDir(snapPath)) {
    goto _ERROR;
  }

  pSnapFile->pSst = taosArrayInit(16, sizeof(void*));
  pSnapFile->pFileList = taosArrayInit(64, sizeof(SBackendFileItem));
  pSnapFile->path = snapPath;
  pSnapFile->snapInfo = *pSnap;
  if ((code = snapFileReadMeta(pSnapFile)) != 0) {
    goto _ERROR;
  }
  if ((code = snapFileCvtMeta(pSnapFile)) != 0) {
    goto _ERROR;
  }

  snapFileDebugInfo(pSnapFile);

  code = 0;

_ERROR:
  taosMemoryFree(snapPath);
  return code;
}
void snapFileDestroy(SBackendSnapFile2* pSnap) {
  taosMemoryFree(pSnap->pCheckpointMeta);
  taosMemoryFree(pSnap->pCurrent);
  taosMemoryFree(pSnap->pMainfest);
  taosMemoryFree(pSnap->pOptions);
  taosMemoryFree(pSnap->path);
  for (int i = 0; i < taosArrayGetSize(pSnap->pSst); i++) {
    char* sst = taosArrayGetP(pSnap->pSst, i);
    taosMemoryFree(sst);
  }
  taosArrayDestroy(pSnap->pFileList);
  taosArrayDestroy(pSnap->pSst);
  taosCloseFile(&pSnap->fd);

  return;
}
int32_t streamSnapHandleInit(SStreamSnapHandle* pHandle, char* path, int64_t chkpId, void* pMeta) {
  // impl later

  SArray* pSnapSet = NULL;
  int32_t code = streamBackendGetSnapInfo(pMeta, path, chkpId);
  if (code != 0) {
    return -1;
  }

  SArray* pBdSnapSet = taosArrayInit(8, sizeof(SBackendSnapFile2));

  for (int i = 0; i < taosArrayGetSize(pSnapSet); i++) {
    SStreamTaskSnap* pSnap = taosArrayGet(pSnapSet, i);

    SBackendSnapFile2 snapFile = {0};
    code = streamBackendSnapInitFile(path, pSnap, &snapFile);
    ASSERT(code == 0);
    taosArrayPush(pBdSnapSet, &snapFile);
  }

  pHandle->pBackendSnapSet = pBdSnapSet;
  pHandle->currIdx = 0;
  return 0;

_err:
  streamSnapHandleDestroy(pHandle);

  code = -1;
  return code;
}

void streamSnapHandleDestroy(SStreamSnapHandle* handle) {
  // SBanckendFile* pFile = handle->pBackendFile;
  if (handle->pBackendSnapSet) {
    for (int i = 0; i < taosArrayGetSize(handle->pBackendSnapSet); i++) {
      SBackendSnapFile2* pSnapFile = taosArrayGet(handle->pBackendSnapSet, i);
      snapFileDebugInfo(pSnapFile);
      snapFileDestroy(pSnapFile);
    }
    taosArrayDestroy(handle->pBackendSnapSet);
  }
  taosMemoryFree(handle->metaPath);

  // if (handle->checkpointId == 0) {
  //   // del tmp dir
  //   if (pFile && taosIsDir(pFile->path)) {
  //     taosRemoveDir(pFile->path);
  //   }
  // } else {
  //   streamBackendDelInUseChkp(handle->handle, handle->checkpointId);
  // }
  // if (pFile) {
  //   taosMemoryFree(pFile->pCheckpointMeta);
  //   taosMemoryFree(pFile->pCurrent);
  //   taosMemoryFree(pFile->pMainfest);
  //   taosMemoryFree(pFile->pOptions);
  //   taosMemoryFree(pFile->path);
  //   for (int i = 0; i < taosArrayGetSize(pFile->pSst); i++) {
  //     char* sst = taosArrayGetP(pFile->pSst, i);
  //     taosMemoryFree(sst);
  //   }
  //   taosArrayDestroy(pFile->pSst);
  //   taosMemoryFree(pFile);
  // }
  // taosArrayDestroy(handle->pFileList);
  // taosCloseFile(&handle->fd);
  return;
}

int32_t streamSnapReaderOpen(void* pMeta, int64_t sver, int64_t chkpId, char* path, SStreamSnapReader** ppReader) {
  // impl later
  SStreamSnapReader* pReader = taosMemoryCalloc(1, sizeof(SStreamSnapReader));
  if (pReader == NULL) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  if (streamSnapHandleInit(&pReader->handle, (char*)path, chkpId, pMeta) < 0) {
    taosMemoryFree(pReader);
    return -1;
  }

  *ppReader = pReader;

  return 0;
}
int32_t streamSnapReaderClose(SStreamSnapReader* pReader) {
  if (pReader == NULL) return 0;

  streamSnapHandleDestroy(&pReader->handle);
  taosMemoryFree(pReader);
  return 0;
}

int32_t streamSnapRead(SStreamSnapReader* pReader, uint8_t** ppData, int64_t* size) {
  // impl later
  int32_t            code = 0;
  SStreamSnapHandle* pHandle = &pReader->handle;
  int32_t            idx = pHandle->currIdx;
  SBackendSnapFile2* pSnapFile = taosArrayGet(pHandle->pBackendSnapSet, idx);
  SBackendFileItem*  item = NULL;

_NEXT:

  if (pSnapFile->fd == NULL) {
    if (pSnapFile->currFileIdx >= taosArrayGetSize(pSnapFile->pFileList)) {
      if (pHandle->currIdx + 1 < taosArrayGetSize(pHandle->pBackendSnapSet)) {
        pHandle->currIdx += 1;

        pSnapFile = taosArrayGet(pHandle->pBackendSnapSet, pHandle->currIdx);
        goto _NEXT;
      } else {
        *ppData = NULL;
        *size = 0;
        return 0;
      }

    } else {
      item = taosArrayGet(pSnapFile->pFileList, pSnapFile->currFileIdx);
      pSnapFile->fd = streamOpenFile(pSnapFile->path, item->name, TD_FILE_READ);
      qDebug("%s open file %s, current offset:%" PRId64 ", size:% " PRId64 ", file no.%d", STREAM_STATE_TRANSFER,
             item->name, (int64_t)pSnapFile->offset, item->size, pSnapFile->currFileIdx);
    }
  }

  qDebug("%s start to read file %s, current offset:%" PRId64 ", size:%" PRId64 ", file no.%d", STREAM_STATE_TRANSFER,
         item->name, (int64_t)pSnapFile->offset, item->size, pSnapFile->currFileIdx);

  uint8_t* buf = taosMemoryCalloc(1, sizeof(SStreamSnapBlockHdr) + kBlockSize);
  int64_t  nread = taosPReadFile(pHandle->fd, buf + sizeof(SStreamSnapBlockHdr), kBlockSize, pSnapFile->offset);
  if (nread == -1) {
    code = TAOS_SYSTEM_ERROR(terrno);
    qError("%s snap failed to read snap, file name:%s, type:%d,reason:%s", STREAM_STATE_TRANSFER, item->name,
           item->type, tstrerror(code));
    return -1;
  } else if (nread > 0 && nread <= kBlockSize) {
    // left bytes less than kBlockSize
    qDebug("%s read file %s, current offset:%" PRId64 ",size:% " PRId64 ", file no.%d", STREAM_STATE_TRANSFER,
           item->name, (int64_t)pSnapFile->offset, item->size, pSnapFile->currFileIdx);
    pSnapFile->offset += nread;
    if (pSnapFile->offset >= item->size || nread < kBlockSize) {
      taosCloseFile(&pSnapFile->fd);
      pSnapFile->offset = 0;
      pSnapFile->currFileIdx += 1;
    }
  } else {
    qDebug("%s no data read, close file no.%d, move to next file, open and read", STREAM_STATE_TRANSFER,
           pSnapFile->currFileIdx);
    taosCloseFile(&pSnapFile->fd);
    pSnapFile->offset = 0;
    pSnapFile->currFileIdx += 1;

    if (pSnapFile->currFileIdx >= taosArrayGetSize(pSnapFile->pFileList)) {
      // finish
      *ppData = NULL;
      *size = 0;
      return 0;
    }
    item = taosArrayGet(pSnapFile->pFileList, pSnapFile->currFileIdx);
    pSnapFile->fd = streamOpenFile(pSnapFile->path, item->name, TD_FILE_READ);

    nread = taosPReadFile(pSnapFile->fd, buf + sizeof(SStreamSnapBlockHdr), kBlockSize, pSnapFile->offset);
    pSnapFile->offset += nread;

    qDebug("%s open file and read file %s, current offset:%" PRId64 ", size:% " PRId64 ", file no.%d",
           STREAM_STATE_TRANSFER, item->name, (int64_t)pSnapFile->offset, item->size, pSnapFile->currFileIdx);
  }

  SStreamSnapBlockHdr* pHdr = (SStreamSnapBlockHdr*)buf;
  pHdr->size = nread;
  pHdr->type = item->type;
  pHdr->totalSize = item->size;
  pHdr->snapInfo = pSnapFile->snapInfo;

  memcpy(pHdr->name, item->name, strlen(item->name));
  pSnapFile->seraial += nread;

  *ppData = buf;
  *size = sizeof(SStreamSnapBlockHdr) + nread;
  return 0;
}
// SMetaSnapWriter ========================================
int32_t streamSnapWriterOpen(void* pMeta, int64_t sver, int64_t ever, char* path, SStreamSnapWriter** ppWriter) {
  // impl later
  SStreamSnapWriter* pWriter = taosMemoryCalloc(1, sizeof(SStreamSnapWriter));
  if (pWriter == NULL) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  SBackendSnapFile2 snapFile = {0};

  SStreamSnapHandle* pHandle = &pWriter->handle;
  pHandle->pBackendSnapSet = taosArrayInit(8, sizeof(SBackendSnapFile2));

  taosArrayPush(pHandle->pBackendSnapSet, &snapFile);
  pHandle->currIdx = 0;
  pHandle->metaPath = taosStrdup(path);

  // SBanckendFile* pFile = taosMemoryCalloc(1, sizeof(SBanckendFile));
  // pFile->path = taosStrdup(path);
  // SArray* list = taosArrayInit(64, sizeof(SBackendFileItem));

  // SBackendFileItem item;
  // item.name = taosStrdup((char*)ROCKSDB_CURRENT);
  // item.type = ROCKSDB_CURRENT_TYPE;
  // taosArrayPush(list, &item);

  // pHandle->pBackendFile = pFile;

  // pHandle->pFileList = list;
  // pHandle->currFileIdx = 0;
  // pHandle->offset = 0;

  *ppWriter = pWriter;
  return 0;
}

int32_t snapInfoEqual(SStreamTaskSnap* a, SStreamTaskSnap* b) {
  if (a->streamId != b->streamId || a->taskId != b->taskId || a->chkpId != b->chkpId) {
    return 0;
  }
  return 1;
}

int32_t streamSnapWriteImpl(SStreamSnapWriter* pWriter, uint8_t* pData, uint32_t nData,
                            SBackendSnapFile2* pBackendFile) {
  int                  code = -1;
  SStreamSnapBlockHdr* pHdr = (SStreamSnapBlockHdr*)pData;
  SStreamSnapHandle*   pHandle = &pWriter->handle;
  SStreamTaskSnap      snapInfo = pHdr->snapInfo;

  SStreamTaskSnap* pSnapInfo = &pBackendFile->snapInfo;

  SBackendFileItem* pItem = taosArrayGet(pBackendFile->pFileList, pBackendFile->currFileIdx);

  if (pBackendFile->fd == 0) {
    pBackendFile->fd = streamOpenFile(pHandle->metaPath, pItem->name, TD_FILE_CREATE | TD_FILE_WRITE | TD_FILE_APPEND);
    if (pBackendFile->fd == NULL) {
      code = TAOS_SYSTEM_ERROR(terrno);
      qError("%s failed to open file name:%s%s%s, reason:%s", STREAM_STATE_TRANSFER, pHandle->metaPath, TD_DIRSEP,
             pHdr->name, tstrerror(code));
    }
  }
  if (strlen(pHdr->name) == strlen(pItem->name) && strcmp(pHdr->name, pItem->name) == 0) {
    int64_t bytes = taosPWriteFile(pBackendFile->fd, pHdr->data, pHdr->size, pBackendFile->offset);
    if (bytes != pHdr->size) {
      code = TAOS_SYSTEM_ERROR(terrno);
      qError("%s failed to write snap, file name:%s, reason:%s", STREAM_STATE_TRANSFER, pHdr->name, tstrerror(code));
      return code;
    }
    pBackendFile->offset += bytes;
  } else {
    taosCloseFile(&pBackendFile->fd);
    pBackendFile->offset = 0;
    pBackendFile->currFileIdx += 1;

    SBackendFileItem item;
    item.name = taosStrdup(pHdr->name);
    item.type = pHdr->type;
    taosArrayPush(pBackendFile->pFileList, &item);

    SBackendFileItem* pItem = taosArrayGet(pBackendFile->pFileList, pBackendFile->currFileIdx);
    pBackendFile->fd = streamOpenFile(pHandle->metaPath, pItem->name, TD_FILE_CREATE | TD_FILE_WRITE | TD_FILE_APPEND);
    if (pBackendFile->fd == NULL) {
      code = TAOS_SYSTEM_ERROR(terrno);
      qError("%s failed to open file name:%s%s%s, reason:%s", STREAM_STATE_TRANSFER, pBackendFile->path, TD_DIRSEP,
             pHdr->name, tstrerror(code));
    }

    taosPWriteFile(pBackendFile->fd, pHdr->data, pHdr->size, pBackendFile->offset);
    pBackendFile->offset += pHdr->size;
  }
  code = 0;
_EXIT:
  return code;
}
int32_t streamSnapWrite(SStreamSnapWriter* pWriter, uint8_t* pData, uint32_t nData) {
  int32_t code = 0;

  SStreamSnapBlockHdr* pHdr = (SStreamSnapBlockHdr*)pData;
  SStreamSnapHandle*   pHandle = &pWriter->handle;
  SStreamTaskSnap      snapInfo = pHdr->snapInfo;

  SBackendSnapFile2* pBackendFile = taosArrayGet(pHandle->pBackendSnapSet, pHandle->currIdx);
  if (pBackendFile->inited == 0) {
    pBackendFile->snapInfo = snapInfo;
    pBackendFile->pFileList = taosArrayInit(64, sizeof(SBackendFileItem));
    pBackendFile->currFileIdx = 0;
    pBackendFile->offset = 0;

    SBackendFileItem item;
    item.name = taosStrdup((char*)ROCKSDB_CURRENT);
    item.type = ROCKSDB_CURRENT_TYPE;
    taosArrayPush(pBackendFile->pFileList, &item);

    pBackendFile->inited = 1;
    return streamSnapWriteImpl(pWriter, pData, nData, pBackendFile);
  } else {
    if (snapInfoEqual(&snapInfo, &pBackendFile->snapInfo)) {
      return streamSnapWriteImpl(pWriter, pData, nData, pBackendFile);
    } else {
      SBackendSnapFile2 snapFile = {0};
      taosArrayPush(pHandle->pBackendSnapSet, &snapFile);
      pHandle->currIdx += 1;

      return streamSnapWrite(pWriter, pData, nData);
    }
  }
  return code;
}
int32_t streamSnapWriterClose(SStreamSnapWriter* pWriter, int8_t rollback) {
  SStreamSnapHandle* handle = &pWriter->handle;
  streamSnapHandleDestroy(handle);
  taosMemoryFree(pWriter);

  return 0;
}
