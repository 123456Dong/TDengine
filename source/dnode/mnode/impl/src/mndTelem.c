/*
 * Copyright (c) 2020 TAOS Data, Inc. <jhtao@taosdata.com>
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
#include "mndTelem.h"
#include "mndCluster.h"
#include "mndSync.h"
#include "tbuffer.h"
#include "tversion.h"

#define TELEMETRY_SERVER "telemetry.taosdata.com"
#define TELEMETRY_PORT 80
#define REPORT_INTERVAL 86400

static void mndBeginObject(SBufferWriter* bw) { tbufWriteChar(bw, '{'); }

static void mndCloseObject(SBufferWriter* bw) {
  size_t len = tbufTell(bw);
  if (tbufGetData(bw, false)[len - 1] == ',') {
    tbufWriteCharAt(bw, len - 1, '}');
  } else {
    tbufWriteChar(bw, '}');
  }
  tbufWriteChar(bw, ',');
}

#if 0
static void beginArray(SBufferWriter* bw) {
  tbufWriteChar(bw, '[');
}

static void closeArray(SBufferWriter* bw) {
  size_t len = tbufTell(bw);
  if (tbufGetData(bw, false)[len - 1] == ',') {
    tbufWriteCharAt(bw, len - 1, ']');
  } else {
    tbufWriteChar(bw, ']');
  }
  tbufWriteChar(bw, ',');
}
#endif

static void mndWriteString(SBufferWriter* bw, const char* str) {
  tbufWriteChar(bw, '"');
  tbufWrite(bw, str, strlen(str));
  tbufWriteChar(bw, '"');
}

static void mndAddIntField(SBufferWriter* bw, const char* k, int64_t v) {
  mndWriteString(bw, k);
  tbufWriteChar(bw, ':');
  char buf[32];
  sprintf(buf, "%" PRId64, v);
  tbufWrite(bw, buf, strlen(buf));
  tbufWriteChar(bw, ',');
}

static void mndAddStringField(SBufferWriter* bw, const char* k, const char* v) {
  mndWriteString(bw, k);
  tbufWriteChar(bw, ':');
  mndWriteString(bw, v);
  tbufWriteChar(bw, ',');
}

static void mndAddCpuInfo(SMnode* pMnode, SBufferWriter* bw) {
  char*   line = NULL;
  size_t  size = 0;
  int32_t done = 0;

  FILE* fp = fopen("/proc/cpuinfo", "r");
  if (fp == NULL) {
    return;
  }

  while (done != 3 && (size = tgetline(&line, &size, fp)) != -1) {
    line[size - 1] = '\0';
    if (((done & 1) == 0) && strncmp(line, "model name", 10) == 0) {
      const char* v = strchr(line, ':') + 2;
      mndAddStringField(bw, "cpuModel", v);
      done |= 1;
    } else if (((done & 2) == 0) && strncmp(line, "cpu cores", 9) == 0) {
      const char* v = strchr(line, ':') + 2;
      mndWriteString(bw, "numOfCpu");
      tbufWriteChar(bw, ':');
      tbufWrite(bw, v, strlen(v));
      tbufWriteChar(bw, ',');
      done |= 2;
    }
  }

  free(line);
  fclose(fp);
}

static void mndAddOsInfo(SMnode* pMnode, SBufferWriter* bw) {
  char*  line = NULL;
  size_t size = 0;

  FILE* fp = fopen("/etc/os-release", "r");
  if (fp == NULL) {
    return;
  }

  while ((size = tgetline(&line, &size, fp)) != -1) {
    line[size - 1] = '\0';
    if (strncmp(line, "PRETTY_NAME", 11) == 0) {
      const char* p = strchr(line, '=') + 1;
      if (*p == '"') {
        p++;
        line[size - 2] = 0;
      }
      mndAddStringField(bw, "os", p);
      break;
    }
  }

  free(line);
  fclose(fp);
}

static void mndAddMemoryInfo(SMnode* pMnode, SBufferWriter* bw) {
  char*  line = NULL;
  size_t size = 0;

  FILE* fp = fopen("/proc/meminfo", "r");
  if (fp == NULL) {
    return;
  }

  while ((size = tgetline(&line, &size, fp)) != -1) {
    line[size - 1] = '\0';
    if (strncmp(line, "MemTotal", 8) == 0) {
      const char* p = strchr(line, ':') + 1;
      while (*p == ' ') p++;
      mndAddStringField(bw, "memory", p);
      break;
    }
  }

  free(line);
  fclose(fp);
}

static void mndAddVersionInfo(SMnode* pMnode, SBufferWriter* bw) {
  STelemMgmt* pMgmt = &pMnode->telemMgmt;

  char vstr[32] = {0};
  taosVersionIntToStr(pMnode->cfg.sver, vstr, 32);

  mndAddStringField(bw, "version", vstr);
  mndAddStringField(bw, "buildInfo", pMnode->cfg.buildinfo);
  mndAddStringField(bw, "gitInfo", pMnode->cfg.gitinfo);
  mndAddStringField(bw, "email", pMgmt->email);
}

static void mndAddRuntimeInfo(SMnode* pMnode, SBufferWriter* bw) {
  SMnodeLoad load = {0};
  if (mndGetLoad(pMnode, &load) != 0) {
    return;
  }

  mndAddIntField(bw, "numOfDnode", load.numOfDnode);
  mndAddIntField(bw, "numOfMnode", load.numOfMnode);
  mndAddIntField(bw, "numOfVgroup", load.numOfVgroup);
  mndAddIntField(bw, "numOfDatabase", load.numOfDatabase);
  mndAddIntField(bw, "numOfSuperTable", load.numOfSuperTable);
  mndAddIntField(bw, "numOfChildTable", load.numOfChildTable);
  mndAddIntField(bw, "numOfColumn", load.numOfColumn);
  mndAddIntField(bw, "numOfPoint", load.totalPoints);
  mndAddIntField(bw, "totalStorage", load.totalStorage);
  mndAddIntField(bw, "compStorage", load.compStorage);
}

static void mndSendTelemetryReport(SMnode* pMnode) {
  STelemMgmt* pMgmt = &pMnode->telemMgmt;

  char     buf[128] = {0};
  uint32_t ip = taosGetIpv4FromFqdn(TELEMETRY_SERVER);
  if (ip == 0xffffffff) {
    mTrace("failed to get IP address of " TELEMETRY_SERVER " since :%s", strerror(errno));
    return;
  }
  SOCKET fd = taosOpenTcpClientSocket(ip, TELEMETRY_PORT, 0);
  if (fd < 0) {
    mTrace("failed to create socket for telemetry, reason:%s", strerror(errno));
    return;
  }

  char clusterName[64] = {0};
  mndGetClusterName(pMnode, clusterName, sizeof(clusterName));

  SBufferWriter bw = tbufInitWriter(NULL, false);
  mndBeginObject(&bw);
  mndAddStringField(&bw, "instanceId", clusterName);
  mndAddIntField(&bw, "reportVersion", 1);
  mndAddOsInfo(pMnode, &bw);
  mndAddCpuInfo(pMnode, &bw);
  mndAddMemoryInfo(pMnode, &bw);
  mndAddVersionInfo(pMnode, &bw);
  mndAddRuntimeInfo(pMnode, &bw);
  mndCloseObject(&bw);

  const char* header =
      "POST /report HTTP/1.1\n"
      "Host: " TELEMETRY_SERVER
      "\n"
      "Content-Type: application/json\n"
      "Content-Length: ";

  taosWriteSocket(fd, (void*)header, (int32_t)strlen(header));
  int32_t contLen = (int32_t)(tbufTell(&bw) - 1);
  sprintf(buf, "%d\n\n", contLen);
  taosWriteSocket(fd, buf, (int32_t)strlen(buf));
  taosWriteSocket(fd, tbufGetData(&bw, false), contLen);
  tbufCloseWriter(&bw);

  // read something to avoid nginx error 499
  if (taosReadSocket(fd, buf, 10) < 0) {
    mTrace("failed to receive response since %s", strerror(errno));
  }

  taosCloseSocket(fd);
}

static void* mndTelemThreadFp(void* param) {
  SMnode*     pMnode = param;
  STelemMgmt* pMgmt = &pMnode->telemMgmt;

  struct timespec end = {0};
  clock_gettime(CLOCK_REALTIME, &end);
  end.tv_sec += 300;  // wait 5 minutes before send first report

  setThreadName("mnd-telem");

  while (!pMgmt->exit) {
    int32_t         r = 0;
    struct timespec ts = end;
    pthread_mutex_lock(&pMgmt->lock);
    r = pthread_cond_timedwait(&pMgmt->cond, &pMgmt->lock, &ts);
    pthread_mutex_unlock(&pMgmt->lock);
    if (r == 0) break;
    if (r != ETIMEDOUT) continue;

    if (mndIsMaster(pMnode)) {
      mndSendTelemetryReport(pMnode);
    }
    end.tv_sec += REPORT_INTERVAL;
  }

  return NULL;
}

static void mndGetEmail(SMnode* pMnode, char* filepath) {
  STelemMgmt* pMgmt = &pMnode->telemMgmt;

  int32_t fd = taosOpenFileRead(filepath);
  if (fd < 0) {
    return;
  }

  if (taosReadFile(fd, (void*)pMgmt->email, TSDB_FQDN_LEN) < 0) {
    mError("failed to read %d bytes from file %s since %s", TSDB_FQDN_LEN, filepath, strerror(errno));
  }

  taosCloseFile(fd);
}

int32_t mndInitTelem(SMnode* pMnode) {
  STelemMgmt* pMgmt = &pMnode->telemMgmt;
  pMgmt->enable = pMnode->cfg.enableTelem;

  if (!pMgmt->enable) return 0;

  pMgmt->exit = 0;
  pthread_mutex_init(&pMgmt->lock, NULL);
  pthread_cond_init(&pMgmt->cond, NULL);
  pMgmt->email[0] = 0;

  mndGetEmail(pMnode, "/usr/local/taos/email");

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

  int32_t code = pthread_create(&pMgmt->thread, &attr, mndTelemThreadFp, pMnode);
  pthread_attr_destroy(&attr);
  if (code != 0) {
    mTrace("failed to create telemetry thread since :%s", strerror(code));
  }

  mInfo("mnd telemetry is initialized");
  return 0;
}

void mndCleanupTelem(SMnode* pMnode) {
  STelemMgmt* pMgmt = &pMnode->telemMgmt;
  if (!pMgmt->enable) return;

  if (taosCheckPthreadValid(pMgmt->thread)) {
    pthread_mutex_lock(&pMgmt->lock);
    pMgmt->exit = 1;
    pthread_cond_signal(&pMgmt->cond);
    pthread_mutex_unlock(&pMgmt->lock);

    pthread_join(pMgmt->thread, NULL);
  }

  pthread_mutex_destroy(&pMgmt->lock);
  pthread_cond_destroy(&pMgmt->cond);
}
