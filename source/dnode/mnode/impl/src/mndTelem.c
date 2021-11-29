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
#include "tbuffer.h"
#include "tglobal.h"
#include "mndSync.h"

#define TELEMETRY_SERVER "telemetry.taosdata.com"
#define TELEMETRY_PORT 80
#define REPORT_INTERVAL 86400

/*
 * sem_timedwait is NOT implemented on MacOSX
 * thus we use pthread_mutex_t/pthread_cond_t to simulate
 */
static struct {
  bool             enable;
  pthread_mutex_t  lock;
  pthread_cond_t   cond;
  volatile int32_t exit;
  pthread_t        thread;
  char             email[TSDB_FQDN_LEN];
} tsTelem;

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

static void mndAddCpuInfo(SBufferWriter* bw) {
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

static void mndAddOsInfo(SBufferWriter* bw) {
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

static void mndAddMemoryInfo(SBufferWriter* bw) {
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

static void mndAddVersionInfo(SBufferWriter* bw) {
  mndAddStringField(bw, "version", version);
  mndAddStringField(bw, "buildInfo", buildinfo);
  mndAddStringField(bw, "gitInfo", gitinfo);
  mndAddStringField(bw, "email", tsTelem.email);
}

static void mndAddRuntimeInfo(SBufferWriter* bw) {
  SMnodeLoad load = {0};
  if (mndGetLoad(NULL, &load) != 0) {
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

static void mndSendTelemetryReport() {
  char     buf[128] = {0};
  uint32_t ip = taosGetIpv4FromFqdn(TELEMETRY_SERVER);
  if (ip == 0xffffffff) {
    mTrace("failed to get IP address of " TELEMETRY_SERVER ", reason:%s", strerror(errno));
    return;
  }
  SOCKET fd = taosOpenTcpClientSocket(ip, TELEMETRY_PORT, 0);
  if (fd < 0) {
    mTrace("failed to create socket for telemetry, reason:%s", strerror(errno));
    return;
  }

  int64_t clusterId = mndGetClusterId(NULL);
  char    clusterIdStr[20] = {0};
  snprintf(clusterIdStr, sizeof(clusterIdStr), "%" PRId64, clusterId);

  SBufferWriter bw = tbufInitWriter(NULL, false);
  mndBeginObject(&bw);
  mndAddStringField(&bw, "instanceId", clusterIdStr);
  mndAddIntField(&bw, "reportVersion", 1);
  mndAddOsInfo(&bw);
  mndAddCpuInfo(&bw);
  mndAddMemoryInfo(&bw);
  mndAddVersionInfo(&bw);
  mndAddRuntimeInfo(&bw);
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
  struct timespec end = {0};
  clock_gettime(CLOCK_REALTIME, &end);
  end.tv_sec += 300;  // wait 5 minutes before send first report

  setThreadName("mnd-telem");

  while (!tsTelem.exit) {
    int32_t         r = 0;
    struct timespec ts = end;
    pthread_mutex_lock(&tsTelem.lock);
    r = pthread_cond_timedwait(&tsTelem.cond, &tsTelem.lock, &ts);
    pthread_mutex_unlock(&tsTelem.lock);
    if (r == 0) break;
    if (r != ETIMEDOUT) continue;

    if (mndIsMaster()) {
      mndSendTelemetryReport();
    }
    end.tv_sec += REPORT_INTERVAL;
  }

  return NULL;
}

static void mndGetEmail(char* filepath) {
  int32_t fd = taosOpenFileRead(filepath);
  if (fd < 0) {
    return;
  }

  if (taosReadFile(fd, (void*)tsTelem.email, TSDB_FQDN_LEN) < 0) {
    mError("failed to read %d bytes from file %s since %s", TSDB_FQDN_LEN, filepath, strerror(errno));
  }

  taosCloseFile(fd);
}

int32_t mndInitTelem() {
  tsTelem.enable = tsEnableTelemetryReporting;
  if (!tsTelem.enable) return 0;

  tsTelem.exit = 0;
  pthread_mutex_init(&tsTelem.lock, NULL);
  pthread_cond_init(&tsTelem.cond, NULL);
  tsTelem.email[0] = 0;

  mndGetEmail("/usr/local/taos/email");

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

  int32_t code = pthread_create(&tsTelem.thread, &attr, mndTelemThreadFp, NULL);
  pthread_attr_destroy(&attr);
  if (code != 0) {
    mTrace("failed to create telemetry thread since :%s", strerror(code));
  }

  mInfo("mnd telemetry is initialized");
  return 0;
}

void mndCleanupTelem() {
  if (!tsTelem.enable) return;

  if (taosCheckPthreadValid(tsTelem.thread)) {
    pthread_mutex_lock(&tsTelem.lock);
    tsTelem.exit = 1;
    pthread_cond_signal(&tsTelem.cond);
    pthread_mutex_unlock(&tsTelem.lock);

    pthread_join(tsTelem.thread, NULL);
  }

  pthread_mutex_destroy(&tsTelem.lock);
  pthread_cond_destroy(&tsTelem.cond);
}
