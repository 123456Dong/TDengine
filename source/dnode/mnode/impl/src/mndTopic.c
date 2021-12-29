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
#include "mndDb.h"
#include "mndDnode.h"
#include "mndMnode.h"
#include "mndShow.h"
#include "mndStb.h"
#include "mndTrans.h"
#include "mndUser.h"
#include "mndVgroup.h"
#include "tname.h"

#define MND_TOPIC_VER_NUMBER 1
#define MND_TOPIC_RESERVE_SIZE 64

static SSdbRaw *mndTopicActionEncode(STopicObj *pTopic);
static SSdbRow *mndTopicActionDecode(SSdbRaw *pRaw);
static int32_t  mndTopicActionInsert(SSdb *pSdb, STopicObj *pTopic);
static int32_t  mndTopicActionDelete(SSdb *pSdb, STopicObj *pTopic);
static int32_t  mndTopicActionUpdate(SSdb *pSdb, STopicObj *pTopic, STopicObj *pNewTopic);
static int32_t  mndProcessCreateTopicMsg(SMnodeMsg *pMsg);
static int32_t  mndProcessDropTopicMsg(SMnodeMsg *pMsg);
static int32_t  mndProcessDropTopicInRsp(SMnodeMsg *pMsg);
static int32_t  mndProcessTopicMetaMsg(SMnodeMsg *pMsg);
static int32_t  mndGetTopicMeta(SMnodeMsg *pMsg, SShowObj *pShow, STableMetaMsg *pMeta);
static int32_t  mndRetrieveTopic(SMnodeMsg *pMsg, SShowObj *pShow, char *data, int32_t rows);
static void     mndCancelGetNextTopic(SMnode *pMnode, void *pIter);

int32_t mndInitTopic(SMnode *pMnode) {
  SSdbTable table = {.sdbType = SDB_TOPIC,
                     .keyType = SDB_KEY_BINARY,
                     .encodeFp = (SdbEncodeFp)mndTopicActionEncode,
                     .decodeFp = (SdbDecodeFp)mndTopicActionDecode,
                     .insertFp = (SdbInsertFp)mndTopicActionInsert,
                     .updateFp = (SdbUpdateFp)mndTopicActionUpdate,
                     .deleteFp = (SdbDeleteFp)mndTopicActionDelete};

  mndSetMsgHandle(pMnode, TDMT_MND_CREATE_TOPIC, mndProcessCreateTopicMsg);
  mndSetMsgHandle(pMnode, TDMT_MND_DROP_TOPIC, mndProcessDropTopicMsg);
  mndSetMsgHandle(pMnode, TDMT_VND_DROP_TOPIC_RSP, mndProcessDropTopicInRsp);

  return sdbSetTable(pMnode->pSdb, table);
}

void mndCleanupTopic(SMnode *pMnode) {}

static SSdbRaw *mndTopicActionEncode(STopicObj *pTopic) {
  int32_t  size = sizeof(STopicObj) + MND_TOPIC_RESERVE_SIZE;
  SSdbRaw *pRaw = sdbAllocRaw(SDB_TOPIC, MND_TOPIC_VER_NUMBER, size);
  if (pRaw == NULL) return NULL;

  int32_t dataPos = 0;
  SDB_SET_BINARY(pRaw, dataPos, pTopic->name, TSDB_TABLE_FNAME_LEN);
  SDB_SET_BINARY(pRaw, dataPos, pTopic->db, TSDB_DB_FNAME_LEN);
  SDB_SET_INT64(pRaw, dataPos, pTopic->createTime);
  SDB_SET_INT64(pRaw, dataPos, pTopic->updateTime);
  SDB_SET_INT64(pRaw, dataPos, pTopic->uid);
  SDB_SET_INT64(pRaw, dataPos, pTopic->dbUid);
  SDB_SET_INT32(pRaw, dataPos, pTopic->version);
  SDB_SET_INT32(pRaw, dataPos, pTopic->execLen);
  SDB_SET_BINARY(pRaw, dataPos, pTopic->executor, pTopic->execLen);
  SDB_SET_INT32(pRaw, dataPos, pTopic->sqlLen);
  SDB_SET_BINARY(pRaw, dataPos, pTopic->sql, pTopic->sqlLen);

  SDB_SET_RESERVE(pRaw, dataPos, MND_TOPIC_RESERVE_SIZE);
  SDB_SET_DATALEN(pRaw, dataPos);

  return pRaw;
}

static SSdbRow *mndTopicActionDecode(SSdbRaw *pRaw) {
  int8_t sver = 0;
  if (sdbGetRawSoftVer(pRaw, &sver) != 0) return NULL;

  if (sver != MND_TOPIC_VER_NUMBER) {
    terrno = TSDB_CODE_SDB_INVALID_DATA_VER;
    mError("failed to decode topic since %s", terrstr());
    return NULL;
  }

  int32_t    size = sizeof(STopicObj) + TSDB_MAX_COLUMNS * sizeof(SSchema);
  SSdbRow   *pRow = sdbAllocRow(size);
  STopicObj *pTopic = sdbGetRowObj(pRow);
  if (pTopic == NULL) return NULL;

  int32_t dataPos = 0;
  SDB_GET_BINARY(pRaw, pRow, dataPos, pTopic->name, TSDB_TABLE_FNAME_LEN);
  SDB_GET_BINARY(pRaw, pRow, dataPos, pTopic->db, TSDB_DB_FNAME_LEN);
  SDB_GET_INT64(pRaw, pRow, dataPos, &pTopic->createTime);
  SDB_GET_INT64(pRaw, pRow, dataPos, &pTopic->updateTime);
  SDB_GET_INT64(pRaw, pRow, dataPos, &pTopic->uid);
  SDB_GET_INT64(pRaw, pRow, dataPos, &pTopic->dbUid);
  SDB_GET_INT32(pRaw, pRow, dataPos, &pTopic->version);
  SDB_GET_INT32(pRaw, pRow, dataPos, &pTopic->execLen);
  SDB_GET_BINARY(pRaw, pRow, dataPos, pTopic->executor, pTopic->execLen);
  SDB_GET_INT32(pRaw, pRow, dataPos, &pTopic->sqlLen);
  SDB_GET_BINARY(pRaw, pRow, dataPos, pTopic->sql, pTopic->sqlLen);

  SDB_GET_RESERVE(pRaw, pRow, dataPos, MND_TOPIC_RESERVE_SIZE);

  return pRow;
}

static int32_t mndTopicActionInsert(SSdb *pSdb, STopicObj *pTopic) {
  mTrace("topic:%s, perform insert action", pTopic->name);
  return 0;
}

static int32_t mndTopicActionDelete(SSdb *pSdb, STopicObj *pTopic) {
  mTrace("topic:%s, perform delete action", pTopic->name);
  return 0;
}

static int32_t mndTopicActionUpdate(SSdb *pSdb, STopicObj *pOldTopic, STopicObj *pNewTopic) {
  mTrace("topic:%s, perform update action", pOldTopic->name);
  atomic_exchange_32(&pOldTopic->updateTime, pNewTopic->updateTime);
  atomic_exchange_32(&pOldTopic->version, pNewTopic->version);

  taosWLockLatch(&pOldTopic->lock);
  
  //TODO handle update

  taosWUnLockLatch(&pOldTopic->lock);
  return 0;
}

STopicObj *mndAcquireTopic(SMnode *pMnode, char *topicName) {
  SSdb      *pSdb = pMnode->pSdb;
  STopicObj *pTopic = sdbAcquire(pSdb, SDB_TOPIC, topicName);
  if (pTopic == NULL) {
    terrno = TSDB_CODE_MND_TOPIC_NOT_EXIST;
  }
  return pTopic;
}

void mndReleaseTopic(SMnode *pMnode, STopicObj *pTopic) {
  SSdb *pSdb = pMnode->pSdb;
  sdbRelease(pSdb, pTopic);
}

static SDbObj *mndAcquireDbByTopic(SMnode *pMnode, char *topicName) {
  SName name = {0};
  tNameFromString(&name, topicName, T_NAME_ACCT | T_NAME_DB | T_NAME_TOPIC);

  char db[TSDB_TABLE_FNAME_LEN] = {0};
  tNameGetFullDbName(&name, db);

  return mndAcquireDb(pMnode, db);
}

static SDropTopicInternalMsg *mndBuildDropTopicMsg(SMnode *pMnode, SVgObj *pVgroup, STopicObj *pTopic) {
  int32_t contLen = sizeof(SDropTopicInternalMsg);

  SDropTopicInternalMsg *pDrop = calloc(1, contLen);
  if (pDrop == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }

  pDrop->head.contLen = htonl(contLen);
  pDrop->head.vgId = htonl(pVgroup->vgId);
  memcpy(pDrop->name, pTopic->name, TSDB_TABLE_FNAME_LEN);
  pDrop->tuid = htobe64(pTopic->uid);

  return pDrop;
}

static int32_t mndCheckCreateTopicMsg(SCMCreateTopicReq *pCreate) {
  // deserialize and other stuff
  return 0;
}

static int32_t mndCreateTopic(SMnode *pMnode, SMnodeMsg *pMsg, SCMCreateTopicReq *pCreate, SDbObj *pDb) {
  STopicObj topicObj = {0};
  tstrncpy(topicObj.name, pCreate->name, TSDB_TABLE_FNAME_LEN);
  tstrncpy(topicObj.db, pDb->name, TSDB_DB_FNAME_LEN);
  topicObj.createTime = taosGetTimestampMs();
  topicObj.updateTime = topicObj.createTime;
  topicObj.uid = mndGenerateUid(pCreate->name, TSDB_TABLE_FNAME_LEN);
  topicObj.dbUid = pDb->uid;
  topicObj.version = 1;

  SSdbRaw *pTopicRaw = mndTopicActionEncode(&topicObj);
  if (pTopicRaw == NULL) return -1;
  if (sdbSetRawStatus(pTopicRaw, SDB_STATUS_READY) != 0) return -1;
  return sdbWrite(pMnode->pSdb, pTopicRaw);
}

static int32_t mndProcessCreateTopicMsg(SMnodeMsg *pMsg) {
  SMnode        *pMnode = pMsg->pMnode;
  char *msgStr = pMsg->rpcMsg.pCont;
  SCMCreateTopicReq* pCreate;
  tDeserializeSCMCreateTopicReq(msgStr, pCreate);

  mDebug("topic:%s, start to create", pCreate->name);

  if (mndCheckCreateTopicMsg(pCreate) != 0) {
    mError("topic:%s, failed to create since %s", pCreate->name, terrstr());
    return -1;
  }

  STopicObj *pTopic = mndAcquireTopic(pMnode, pCreate->name);
  if (pTopic != NULL) {
    sdbRelease(pMnode->pSdb, pTopic);
    if (pCreate->igExists) {
      mDebug("topic:%s, already exist, ignore exist is set", pCreate->name);
      return 0;
    } else {
      terrno = TSDB_CODE_MND_TOPIC_ALREADY_EXIST;
      mError("db:%s, failed to create since %s", pCreate->name, terrstr());
      return -1;
    }
  }

  SDbObj *pDb = mndAcquireDbByTopic(pMnode, pCreate->name);
  if (pDb == NULL) {
    terrno = TSDB_CODE_MND_DB_NOT_SELECTED;
    mError("topic:%s, failed to create since %s", pCreate->name, terrstr());
    return -1;
  }

  int32_t code = mndCreateTopic(pMnode, pMsg, pCreate, pDb);
  mndReleaseDb(pMnode, pDb);

  if (code != 0) {
    terrno = code;
    mError("topic:%s, failed to create since %s", pCreate->name, terrstr());
    return -1;
  }

  return TSDB_CODE_MND_ACTION_IN_PROGRESS;
}

static int32_t mndDropTopic(SMnode *pMnode, SMnodeMsg *pMsg, STopicObj *pTopic) {
  return 0;
}

static int32_t mndProcessDropTopicMsg(SMnodeMsg *pMsg) {
  SMnode        *pMnode = pMsg->pMnode;
  SDropTopicMsg *pDrop = pMsg->rpcMsg.pCont;

  mDebug("topic:%s, start to drop", pDrop->name);

  STopicObj *pTopic = mndAcquireTopic(pMnode, pDrop->name);
  if (pTopic == NULL) {
    if (pDrop->igNotExists) {
      mDebug("topic:%s, not exist, ignore not exist is set", pDrop->name);
      return 0;
    } else {
      terrno = TSDB_CODE_MND_TOPIC_NOT_EXIST;
      mError("topic:%s, failed to drop since %s", pDrop->name, terrstr());
      return -1;
    }
  }

  int32_t code = mndDropTopic(pMnode, pMsg, pTopic);
  mndReleaseTopic(pMnode, pTopic);

  if (code != 0) {
    terrno = code;
    mError("topic:%s, failed to drop since %s", pDrop->name, terrstr());
    return -1;
  }

  return TSDB_CODE_MND_ACTION_IN_PROGRESS;
}

static int32_t mndProcessDropTopicInRsp(SMnodeMsg *pMsg) {
  mndTransProcessRsp(pMsg);
  return 0;
}

static int32_t mndProcessTopicMetaMsg(SMnodeMsg *pMsg) {
  SMnode        *pMnode = pMsg->pMnode;
  STableInfoMsg *pInfo = pMsg->rpcMsg.pCont;

  mDebug("topic:%s, start to retrieve meta", pInfo->tableFname);

#if 0
  SDbObj *pDb = mndAcquireDbByTopic(pMnode, pInfo->tableFname);
  if (pDb == NULL) {
    terrno = TSDB_CODE_MND_DB_NOT_SELECTED;
    mError("topic:%s, failed to retrieve meta since %s", pInfo->tableFname, terrstr());
    return -1;
  }

  STopicObj *pTopic = mndAcquireTopic(pMnode, pInfo->tableFname);
  if (pTopic == NULL) {
    mndReleaseDb(pMnode, pDb);
    terrno = TSDB_CODE_MND_INVALID_TOPIC;
    mError("topic:%s, failed to get meta since %s", pInfo->tableFname, terrstr());
    return -1;
  }

  taosRLockLatch(&pTopic->lock);
  int32_t totalCols = pTopic->numOfColumns + pTopic->numOfTags;
  int32_t contLen = sizeof(STableMetaMsg) + totalCols * sizeof(SSchema);

  STableMetaMsg *pMeta = rpcMallocCont(contLen);
  if (pMeta == NULL) {
    taosRUnLockLatch(&pTopic->lock);
    mndReleaseDb(pMnode, pDb);
    mndReleaseTopic(pMnode, pTopic);
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    mError("topic:%s, failed to get meta since %s", pInfo->tableFname, terrstr());
    return -1;
  }

  memcpy(pMeta->topicFname, pTopic->name, TSDB_TABLE_FNAME_LEN);
  pMeta->numOfTags = htonl(pTopic->numOfTags);
  pMeta->numOfColumns = htonl(pTopic->numOfColumns);
  pMeta->precision = pDb->cfg.precision;
  pMeta->tableType = TSDB_SUPER_TABLE;
  pMeta->update = pDb->cfg.update;
  pMeta->sversion = htonl(pTopic->version);
  pMeta->tuid = htonl(pTopic->uid);

  for (int32_t i = 0; i < totalCols; ++i) {
    SSchema *pSchema = &pMeta->pSchema[i];
    SSchema *pSrcSchema = &pTopic->pSchema[i];
    memcpy(pSchema->name, pSrcSchema->name, TSDB_COL_NAME_LEN);
    pSchema->type = pSrcSchema->type;
    pSchema->colId = htonl(pSrcSchema->colId);
    pSchema->bytes = htonl(pSrcSchema->bytes);
  }
  taosRUnLockLatch(&pTopic->lock);
  mndReleaseDb(pMnode, pDb);
  mndReleaseTopic(pMnode, pTopic);

  pMsg->pCont = pMeta;
  pMsg->contLen = contLen;

  mDebug("topic:%s, meta is retrieved, cols:%d tags:%d", pInfo->tableFname, pTopic->numOfColumns, pTopic->numOfTags);
#endif
  return 0;
}

static int32_t mndGetNumOfTopics(SMnode *pMnode, char *dbName, int32_t *pNumOfTopics) {
  SSdb *pSdb = pMnode->pSdb;

  SDbObj *pDb = mndAcquireDb(pMnode, dbName);
  if (pDb == NULL) {
    terrno = TSDB_CODE_MND_DB_NOT_SELECTED;
    return -1;
  }

  int32_t numOfTopics = 0;
  void   *pIter = NULL;
  while (1) {
    STopicObj *pTopic = NULL;
    pIter = sdbFetch(pSdb, SDB_TOPIC, pIter, (void **)&pTopic);
    if (pIter == NULL) break;

    if (strcmp(pTopic->db, dbName) == 0) {
      numOfTopics++;
    }

    sdbRelease(pSdb, pTopic);
  }

  *pNumOfTopics = numOfTopics;
  return 0;
}

static int32_t mndGetTopicMeta(SMnodeMsg *pMsg, SShowObj *pShow, STableMetaMsg *pMeta) {
  SMnode *pMnode = pMsg->pMnode;
  SSdb   *pSdb = pMnode->pSdb;

  if (mndGetNumOfTopics(pMnode, pShow->db, &pShow->numOfRows) != 0) {
    return -1;
  }

  int32_t  cols = 0;
  SSchema *pSchema = pMeta->pSchema;

  pShow->bytes[cols] = TSDB_TABLE_NAME_LEN + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "name");
  pSchema[cols].bytes = htonl(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 8;
  pSchema[cols].type = TSDB_DATA_TYPE_TIMESTAMP;
  strcpy(pSchema[cols].name, "create_time");
  pSchema[cols].bytes = htonl(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "columns");
  pSchema[cols].bytes = htonl(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "tags");
  pSchema[cols].bytes = htonl(pShow->bytes[cols]);
  cols++;

  pMeta->numOfColumns = htonl(cols);
  pShow->numOfColumns = cols;

  pShow->offset[0] = 0;
  for (int32_t i = 1; i < cols; ++i) {
    pShow->offset[i] = pShow->offset[i - 1] + pShow->bytes[i - 1];
  }

  pShow->numOfRows = sdbGetSize(pSdb, SDB_TOPIC);
  pShow->rowSize = pShow->offset[cols - 1] + pShow->bytes[cols - 1];
  strcpy(pMeta->tbFname, mndShowStr(pShow->type));

  return 0;
}

static void mndExtractTableName(char *tableId, char *name) {
  int32_t pos = -1;
  int32_t num = 0;
  for (pos = 0; tableId[pos] != 0; ++pos) {
    if (tableId[pos] == '.') num++;
    if (num == 2) break;
  }

  if (num == 2) {
    strcpy(name, tableId + pos + 1);
  }
}

static int32_t mndRetrieveTopic(SMnodeMsg *pMsg, SShowObj *pShow, char *data, int32_t rows) {
  SMnode    *pMnode = pMsg->pMnode;
  SSdb      *pSdb = pMnode->pSdb;
  int32_t    numOfRows = 0;
  STopicObj *pTopic = NULL;
  int32_t    cols = 0;
  char      *pWrite;
  char       prefix[64] = {0};

  tstrncpy(prefix, pShow->db, 64);
  strcat(prefix, TS_PATH_DELIMITER);
  int32_t prefixLen = (int32_t)strlen(prefix);

  while (numOfRows < rows) {
    pShow->pIter = sdbFetch(pSdb, SDB_TOPIC, pShow->pIter, (void **)&pTopic);
    if (pShow->pIter == NULL) break;

    if (strncmp(pTopic->name, prefix, prefixLen) != 0) {
      sdbRelease(pSdb, pTopic);
      continue;
    }

    cols = 0;

    char topicName[TSDB_TABLE_NAME_LEN] = {0};
    tstrncpy(topicName, pTopic->name + prefixLen, TSDB_TABLE_NAME_LEN);
    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    STR_TO_VARSTR(pWrite, topicName);
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int64_t *)pWrite = pTopic->createTime;
    cols++;

    /*pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;*/
    /**(int32_t *)pWrite = pTopic->numOfColumns;*/
    /*cols++;*/

    /*pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;*/
    /**(int32_t *)pWrite = pTopic->numOfTags;*/
    /*cols++;*/

    numOfRows++;
    sdbRelease(pSdb, pTopic);
  }

  pShow->numOfReads += numOfRows;
  mndVacuumResult(data, pShow->numOfColumns, numOfRows, rows, pShow);
  return numOfRows;
}

static void mndCancelGetNextTopic(SMnode *pMnode, void *pIter) {
  SSdb *pSdb = pMnode->pSdb;
  sdbCancelFetch(pSdb, pIter);
}
