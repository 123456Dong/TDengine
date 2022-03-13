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
 * along with this program. If not, see <http:www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "mm.h"

#if 0
int32_t mmReadFile(SDnode *pDnode) {
  SMnodeMgmt *pMgmt = &pDnode->mmgmt;

  int32_t code = TSDB_CODE_DND_MNODE_READ_FILE_ERROR;
  int32_t len = 0;
  int32_t maxLen = 4096;
  char   *content = calloc(1, maxLen + 1);
  cJSON  *root = NULL;
  char    file[PATH_MAX + 20];

  snprintf(file, sizeof(file), "%s%smnode.json", pDnode->dir.dnode, TD_DIRSEP);

  TdFilePtr pFile = taosOpenFile(file, TD_FILE_READ);
  if (pFile == NULL) {
    dDebug("file %s not exist", file);
    code = 0;
    goto PRASE_MNODE_OVER;
  }

  len = (int32_t)taosReadFile(pFile, content, maxLen);
  if (len <= 0) {
    dError("failed to read %s since content is null", file);
    goto PRASE_MNODE_OVER;
  }

  content[len] = 0;
  root = cJSON_Parse(content);
  if (root == NULL) {
    dError("failed to read %s since invalid json format", file);
    goto PRASE_MNODE_OVER;
  }

  cJSON *deployed = cJSON_GetObjectItem(root, "deployed");
  if (!deployed || deployed->type != cJSON_Number) {
    dError("failed to read %s since deployed not found", file);
    goto PRASE_MNODE_OVER;
  }
  pMgmt->deployed = deployed->valueint;

  cJSON *dropped = cJSON_GetObjectItem(root, "dropped");
  if (!dropped || dropped->type != cJSON_Number) {
    dError("failed to read %s since dropped not found", file);
    goto PRASE_MNODE_OVER;
  }
  pMgmt->dropped = dropped->valueint;

  cJSON *mnodes = cJSON_GetObjectItem(root, "mnodes");
  if (!mnodes || mnodes->type != cJSON_Array) {
    dError("failed to read %s since nodes not found", file);
    goto PRASE_MNODE_OVER;
  }

  pMgmt->replica = cJSON_GetArraySize(mnodes);
  if (pMgmt->replica <= 0 || pMgmt->replica > TSDB_MAX_REPLICA) {
    dError("failed to read %s since mnodes size %d invalid", file, pMgmt->replica);
    goto PRASE_MNODE_OVER;
  }

  for (int32_t i = 0; i < pMgmt->replica; ++i) {
    cJSON *node = cJSON_GetArrayItem(mnodes, i);
    if (node == NULL) break;

    SReplica *pReplica = &pMgmt->replicas[i];

    cJSON *id = cJSON_GetObjectItem(node, "id");
    if (!id || id->type != cJSON_Number) {
      dError("failed to read %s since id not found", file);
      goto PRASE_MNODE_OVER;
    }
    pReplica->id = id->valueint;

    cJSON *fqdn = cJSON_GetObjectItem(node, "fqdn");
    if (!fqdn || fqdn->type != cJSON_String || fqdn->valuestring == NULL) {
      dError("failed to read %s since fqdn not found", file);
      goto PRASE_MNODE_OVER;
    }
    tstrncpy(pReplica->fqdn, fqdn->valuestring, TSDB_FQDN_LEN);

    cJSON *port = cJSON_GetObjectItem(node, "port");
    if (!port || port->type != cJSON_Number) {
      dError("failed to read %s since port not found", file);
      goto PRASE_MNODE_OVER;
    }
    pReplica->port = port->valueint;
  }

  code = 0;
  dDebug("succcessed to read file %s, deployed:%d dropped:%d", file, pMgmt->deployed, pMgmt->dropped);

PRASE_MNODE_OVER:
  if (content != NULL) free(content);
  if (root != NULL) cJSON_Delete(root);
  if (pFile != NULL) taosCloseFile(&pFile);

  terrno = code;
  return code;
}

int32_t mmWriteFile(SDnode *pDnode) {
  SMnodeMgmt *pMgmt = &pDnode->mmgmt;

  char file[PATH_MAX];
  snprintf(file, sizeof(file), "%s%smnode.json.bak", pDnode->dir.dnode, TD_DIRSEP);

  TdFilePtr pFile = taosOpenFile(file, TD_FILE_CTEATE | TD_FILE_WRITE | TD_FILE_TRUNC);
  if (pFile == NULL) {
    terrno = TSDB_CODE_DND_MNODE_WRITE_FILE_ERROR;
    dError("failed to write %s since %s", file, terrstr());
    return -1;
  }

  int32_t len = 0;
  int32_t maxLen = 4096;
  char   *content = calloc(1, maxLen + 1);

  len += snprintf(content + len, maxLen - len, "{\n");
  len += snprintf(content + len, maxLen - len, "  \"deployed\": %d,\n", pMgmt->deployed);

  len += snprintf(content + len, maxLen - len, "  \"dropped\": %d,\n", pMgmt->dropped);
  len += snprintf(content + len, maxLen - len, "  \"mnodes\": [{\n");
  for (int32_t i = 0; i < pMgmt->replica; ++i) {
    SReplica *pReplica = &pMgmt->replicas[i];
    len += snprintf(content + len, maxLen - len, "    \"id\": %d,\n", pReplica->id);
    len += snprintf(content + len, maxLen - len, "    \"fqdn\": \"%s\",\n", pReplica->fqdn);
    len += snprintf(content + len, maxLen - len, "    \"port\": %u\n", pReplica->port);
    if (i < pMgmt->replica - 1) {
      len += snprintf(content + len, maxLen - len, "  },{\n");
    } else {
      len += snprintf(content + len, maxLen - len, "  }]\n");
    }
  }
  len += snprintf(content + len, maxLen - len, "}\n");

  taosWriteFile(pFile, content, len);
  taosFsyncFile(pFile);
  taosCloseFile(&pFile);
  free(content);

  char realfile[PATH_MAX + 20];
  snprintf(realfile, sizeof(realfile), "%s%smnode.json", pDnode->dir.dnode, TD_DIRSEP);

  if (taosRenameFile(file, realfile) != 0) {
    terrno = TSDB_CODE_DND_MNODE_WRITE_FILE_ERROR;
    dError("failed to rename %s since %s", file, terrstr());
    return -1;
  }

  dInfo("successed to write %s, deployed:%d dropped:%d", realfile, pMgmt->deployed, pMgmt->dropped);
  return 0;
}

#endif