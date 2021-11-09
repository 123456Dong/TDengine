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
#include "tglobal.h"
#include "tstep.h"
#include "mnodeAcct.h"
#include "mnodeAuth.h"
#include "mnodeBalance.h"
#include "mnodeCluster.h"
#include "mnodeDb.h"
#include "mnodeDnode.h"
#include "mnodeFunc.h"
#include "mnodeMnode.h"
#include "mnodeOper.h"
#include "mnodeProfile.h"
#include "mnodeInt.h"
#include "mnodeShow.h"
#include "mnodeStable.h"
#include "mnodeSync.h"
#include "mnodeUser.h"
#include "mnodeVgroup.h"
#include "mnodeWorker.h"
#include "mnodeTelem.h"

static struct {
  int32_t    state;
  int32_t    dnodeId;
  int64_t    clusterId;
  tmr_h      timer;
  SSteps    *steps1;
  SSteps    *steps2;
  SMnodePara para;
} tsMint;

tmr_h mnodeGetTimer() { return tsMint.timer; }

int32_t mnodeGetDnodeId() { return tsMint.para.dnodeId; }

int64_t mnodeGetClusterId() { return tsMint.para.clusterId; }

EMnStatus mnodeGetStatus() { return tsMint.state; }

void mnodeSendMsgToDnode(struct SEpSet *epSet, struct SRpcMsg *rpcMsg) {
  (*tsMint.para.SendMsgToDnode)(epSet, rpcMsg);
}

void mnodeSendMsgToMnode(struct SRpcMsg *rpcMsg) { return (*tsMint.para.SendMsgToMnode)(rpcMsg); }

void mnodeSendRedirectMsg(struct SRpcMsg *rpcMsg, bool forShell) { (*tsMint.para.SendRedirectMsg)(rpcMsg, forShell); }

int32_t mnodeGetLoad(SMnodeLoad *pLoad) { return 0; }

static int32_t mnodeSetPara(SMnodePara para) {
  tsMint.para = para;

  if (tsMint.para.SendMsgToDnode == NULL) return -1;
  if (tsMint.para.SendMsgToMnode == NULL) return -1;
  if (tsMint.para.SendRedirectMsg == NULL) return -1;
  if (tsMint.para.PutMsgIntoApplyQueue == NULL) return -1;
  if (tsMint.para.dnodeId < 0) return -1;
  if (tsMint.para.clusterId < 0) return -1;

  return 0;
}

static int32_t mnodeInitTimer() {
  if (tsMint.timer == NULL) {
    tsMint.timer = taosTmrInit(tsMaxShellConns, 200, 3600000, "MND");
  }

  return 0;
}

static void mnodeCleanupTimer() {
  if (tsMint.timer != NULL) {
    taosTmrCleanUp(tsMint.timer);
    tsMint.timer = NULL;
  }
}

static int32_t mnodeInitStep1() {
  struct SSteps *steps = taosStepInit(16, NULL);
  if (steps == NULL) return -1;

  taosStepAdd(steps, "mnode-sdb", sdbInit, sdbCleanup);
  taosStepAdd(steps, "mnode-cluster", mnodeInitCluster, mnodeCleanupCluster);
  taosStepAdd(steps, "mnode-dnode", mnodeInitDnode, mnodeCleanupDnode);
  taosStepAdd(steps, "mnode-mnode", mnodeInitMnode, mnodeCleanupMnode);
  taosStepAdd(steps, "mnode-acct", mnodeInitAcct, mnodeCleanupAcct);
  taosStepAdd(steps, "mnode-auth", mnodeInitAuth, mnodeCleanupAuth);
  taosStepAdd(steps, "mnode-user", mnodeInitUser, mnodeCleanupUser);
  taosStepAdd(steps, "mnode-db", mnodeInitDb, mnodeCleanupDb);
  taosStepAdd(steps, "mnode-vgroup", mnodeInitVgroup, mnodeCleanupVgroup);
  taosStepAdd(steps, "mnode-stable", mnodeInitStable, mnodeCleanupStable);
  taosStepAdd(steps, "mnode-func", mnodeInitFunc, mnodeCleanupFunc);
  taosStepAdd(steps, "mnode-oper", mnodeInitOper, mnodeCleanupOper);

  tsMint.steps1 = steps;
  return taosStepExec(tsMint.steps1);
}

static int32_t mnodeInitStep2() {
  struct SSteps *steps = taosStepInit(12, NULL);
  if (steps == NULL) return -1;

  taosStepAdd(steps, "mnode-timer", mnodeInitTimer, NULL);
  taosStepAdd(steps, "mnode-worker", mnodeInitWorker, NULL);
  taosStepAdd(steps, "mnode-balance", mnodeInitBalance, mnodeCleanupBalance);
  taosStepAdd(steps, "mnode-profile", mnodeInitProfile, mnodeCleanupProfile);
  taosStepAdd(steps, "mnode-show", mnodeInitShow, mnodeCleanUpShow);
  taosStepAdd(steps, "mnode-sync", mnodeInitSync, mnodeCleanUpSync);
  taosStepAdd(steps, "mnode-worker", NULL, mnodeCleanupWorker);
  taosStepAdd(steps, "mnode-telem", mnodeInitTelem, mnodeCleanupTelem);
  taosStepAdd(steps, "mnode-timer", NULL, mnodeCleanupTimer);

  tsMint.steps2 = steps;
  return taosStepExec(tsMint.steps2);
}

static void mnodeCleanupStep1() { taosStepCleanup(tsMint.steps1); }

static void mnodeCleanupStep2() { taosStepCleanup(tsMint.steps2); }

static bool mnodeNeedDeploy() {
  if (tsMint.para.dnodeId > 0) return false;
  if (tsMint.para.clusterId > 0) return false;
  if (strcmp(tsFirst, tsLocalEp) != 0) return false;
  return true;
}

int32_t mnodeDeploy(char *path, SMnodeCfg *pCfg) {
  if (tsMint.state != MN_STATUS_UNINIT) {
    mError("failed to deploy mnode since its deployed");
    return 0;
  } else {
    tsMint.state = MN_STATUS_INIT;
  }

  if (tsMint.para.dnodeId <= 0 || tsMint.para.clusterId <= 0) {
    mError("failed to deploy mnode since cluster not ready");
    return TSDB_CODE_MND_NOT_READY;
  }

  mInfo("starting to deploy mnode");

  int32_t code = mnodeInitStep1();
  if (code != 0) {
    mError("failed to deploy mnode since init step1 error");
    tsMint.state = MN_STATUS_UNINIT;
    return TSDB_CODE_MND_APP_ERROR;
  }

  code = mnodeInitStep2();
  if (code != 0) {
    mnodeCleanupStep1();
    mError("failed to deploy mnode since init step2 error");
    tsMint.state = MN_STATUS_UNINIT;
    return TSDB_CODE_MND_APP_ERROR;
  }

  mDebug("mnode is deployed and waiting for raft to confirm");
  tsMint.state = MN_STATUS_READY;
  return 0;
}

void mnodeUnDeploy(char *path) {
  sdbUnDeploy();
  mnodeCleanup();
}

int32_t mnodeInit(SMnodePara para) {
  mDebugFlag = 207;
  if (tsMint.state != MN_STATUS_UNINIT) {
    return 0;
  } else {
    tsMint.state = MN_STATUS_INIT;
  }

  mInfo("starting to initialize mnode ...");

  int32_t code = mnodeSetPara(para);
  if (code != 0) {
    tsMint.state = MN_STATUS_UNINIT;
    return code;
  }

  code = mnodeInitStep1();
  if (code != 0) {
    tsMint.state = MN_STATUS_UNINIT;
    return -1;
  }

  code = sdbRead();
  if (code != 0) {
    if (mnodeNeedDeploy()) {
      code = sdbDeploy();
      if (code != 0) {
        mnodeCleanupStep1();
        tsMint.state = MN_STATUS_UNINIT;
        return -1;
      }
    } else {
      mnodeCleanupStep1();
      tsMint.state = MN_STATUS_UNINIT;
      return -1;
    }
  }

  code = mnodeInitStep2();
  if (code != 0) {
    mnodeCleanupStep1();
    tsMint.state = MN_STATUS_UNINIT;
    return -1;
  }

  tsMint.state = MN_STATUS_READY;
  mInfo("mnode is initialized successfully");
  return 0;
}

void mnodeCleanup() {
  if (tsMint.state != MN_STATUS_UNINIT && tsMint.state != MN_STATUS_CLOSING) {
    mInfo("starting to clean up mnode");
    tsMint.state = MN_STATUS_CLOSING;

    mnodeCleanupStep2();
    mnodeCleanupStep1();

    tsMint.state = MN_STATUS_UNINIT;
    mInfo("mnode is cleaned up");
  }
}

int32_t mnodeStart(char *path, SMnodeCfg *pCfg) { return 0; }
int32_t mnodeAlter(SMnodeCfg *pCfg) { return 0; }
void    mnodeStop() {}