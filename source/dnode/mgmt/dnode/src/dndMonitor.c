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
#include "dndMonitor.h"

static int32_t dndGetMonitorDiskInfo(SDnode *pDnode, SMonDiskInfo *pInfo) {
  tstrncpy(pInfo->logdir.name, tsLogDir, sizeof(pInfo->logdir.name));
  pInfo->logdir.size = tsLogSpace.size;
  tstrncpy(pInfo->tempdir.name, tsTempDir, sizeof(pInfo->tempdir.name));
  pInfo->tempdir.size = tsTempSpace.size;

  if (pDnode->pTfs != NULL) {
    return tfsGetMonitorInfo(NULL, pInfo);
  }
  return 0;
}

static void dndGetMonitorBasicInfo(SDnode *pDnode, SMonBasicInfo *pInfo) {
  pInfo->dnode_id = dndGetDnodeId(pDnode);
  tstrncpy(pInfo->dnode_ep, tsLocalEp, TSDB_EP_LEN);
  pInfo->cluster_id = dndGetClusterId(pDnode);
  pInfo->protocol = 1;
}

static void dndGetMonitorDnodeInfo(SDnode *pDnode, SMonDnodeInfo *pInfo) {
  pInfo->uptime = (taosGetTimestampMs() - pDnode->dmgmt.rebootTime) / (86400000.0f);
  taosGetCpuUsage(&pInfo->cpu_engine, &pInfo->cpu_system);
  pInfo->cpu_cores = tsNumOfCores;
  taosGetProcMemory(&pInfo->mem_engine);
  taosGetSysMemory(&pInfo->mem_system);
  pInfo->mem_total = tsTotalMemoryKB;
  pInfo->disk_engine = 0;
  pInfo->disk_used = tsDataSpace.size.used;
  pInfo->disk_total = tsDataSpace.size.total;
  taosGetCardInfo(&pInfo->net_in, &pInfo->net_out);
  taosGetProcIO(&pInfo->io_read, &pInfo->io_write, &pInfo->io_read_disk, &pInfo->io_write_disk);

#if 0
  SVnodesStat *pStat = &pDnode->vmgmt.stat;
  pInfo->req_select = pStat->numOfSelectReqs;
  pInfo->req_insert = pStat->numOfInsertReqs;
  pInfo->req_insert_success = pStat->numOfInsertSuccessReqs;
  pInfo->req_insert_batch = pStat->numOfBatchInsertReqs;
  pInfo->req_insert_batch_success = pStat->numOfBatchInsertSuccessReqs;
  pInfo->errors = tsNumOfErrorLogs;
  pInfo->vnodes_num = pStat->totalVnodes;
  pInfo->masters = pStat->masterNum;
  pInfo->has_mnode = pDnode->mmgmt.deployed;
#endif  
}

void dndSendMonitorReport(SDnode *pDnode) {
  if (!tsEnableMonitor || tsMonitorFqdn[0] == 0 || tsMonitorPort == 0) return;
  dTrace("pDnode:%p, send monitor report to %s:%u", pDnode, tsMonitorFqdn, tsMonitorPort);

  SMonInfo *pMonitor = monCreateMonitorInfo();
  if (pMonitor == NULL) return;

  SMonBasicInfo basicInfo = {0};
  dndGetMonitorBasicInfo(pDnode, &basicInfo);
  monSetBasicInfo(pMonitor, &basicInfo);

  SMonClusterInfo clusterInfo = {0};
  SMonVgroupInfo  vgroupInfo = {0};
  SMonGrantInfo   grantInfo = {0};
  if (mmGetMonitorInfo(pDnode, &clusterInfo, &vgroupInfo, &grantInfo) == 0) {
    monSetClusterInfo(pMonitor, &clusterInfo);
    monSetVgroupInfo(pMonitor, &vgroupInfo);
    monSetGrantInfo(pMonitor, &grantInfo);
  }

  SMonDnodeInfo dnodeInfo = {0};
  dndGetMonitorDnodeInfo(pDnode, &dnodeInfo);
  monSetDnodeInfo(pMonitor, &dnodeInfo);

  SMonDiskInfo diskInfo = {0};
  if (dndGetMonitorDiskInfo(pDnode, &diskInfo) == 0) {
    monSetDiskInfo(pMonitor, &diskInfo);
  }

  taosArrayDestroy(clusterInfo.dnodes);
  taosArrayDestroy(clusterInfo.mnodes);
  taosArrayDestroy(vgroupInfo.vgroups);
  taosArrayDestroy(diskInfo.datadirs);

  monSendReport(pMonitor);
  monCleanupMonitorInfo(pMonitor);
}