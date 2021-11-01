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
#include "dnodeMnode.h"
#include "dnodeConfig.h"
#include "dnodeTransport.h"
#include "mnode.h"

int32_t dnodeInitMnode() {
  SMnodePara para;
  para.fp.GetDnodeEp = dnodeGetEp;
  para.fp.SendMsgToDnode = dnodeSendMsgToDnode;
  para.fp.SendMsgToMnode = dnodeSendMsgToMnode;
  para.fp.SendRedirectMsg = dnodeSendRedirectMsg;
  para.dnodeId = dnodeGetDnodeId();
  para.clusterId = dnodeGetClusterId();

  return mnodeInit(para);
}

void dnodeCleanupMnode() { mnodeCleanup(); }