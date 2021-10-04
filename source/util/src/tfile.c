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
#include "taoserror.h"
#include "ulog.h"
#include "tutil.h"
#include "tref.h"

static int32_t tsFileRsetId = -1;

static void tfCloseFile(void *p) {
  taosCloseFile((int32_t)(uintptr_t)p);
}

int32_t tfInit() {
  tsFileRsetId = taosOpenRef(2000, tfCloseFile);
  if (tsFileRsetId > 0) {
    return 0;
  } else {
    return -1;
  }
}

void tfCleanup() {
  if (tsFileRsetId >= 0) taosCloseRef(tsFileRsetId);
  tsFileRsetId = -1;
}

static int64_t tfOpenImp(int32_t fd) {
  if (fd < 0) {
    terrno = TAOS_SYSTEM_ERROR(errno);
    return -1;
  }

  void *  p = (void *)(int64_t)fd;
  int64_t rid = taosAddRef(tsFileRsetId, p);
  if (rid < 0) taosCloseFile(fd);

  return rid;
}

int64_t tfOpenReadWrite(const char *pathname, int32_t flags) {
  int32_t fd = taosOpenFileReadWrite(pathname);
  return tfOpenImp(fd);
}

int64_t tfOpenCreateWrite(const char *pathname, int32_t flags, mode_t mode) {
  int32_t fd = taosOpenFileCreateWrite(pathname);
  return tfOpenImp(fd);
}

int64_t tfOpenCreateWriteAppend(const char *pathname, int32_t flags, mode_t mode) {
  int32_t fd = taosOpenFileCreateWriteAppend(pathname);
  return tfOpenImp(fd);
}

int64_t tfClose(int64_t tfd) {
  return taosRemoveRef(tsFileRsetId, tfd);
}

int64_t tfWrite(int64_t tfd, void *buf, int64_t count) {
  void *p = taosAcquireRef(tsFileRsetId, tfd);
  if (p == NULL) return -1;

  int32_t fd = (int32_t)(uintptr_t)p;

  int64_t ret = taosWriteFile(fd, buf, count);
  if (ret < 0) terrno = TAOS_SYSTEM_ERROR(errno);

  taosReleaseRef(tsFileRsetId, tfd);
  return ret;
}

int64_t tfRead(int64_t tfd, void *buf, int64_t count) {
  void *p = taosAcquireRef(tsFileRsetId, tfd);
  if (p == NULL) return -1;

  int32_t fd = (int32_t)(uintptr_t)p;

  int64_t ret = taosReadFile(fd, buf, count);
  if (ret < 0) terrno = TAOS_SYSTEM_ERROR(errno);

  taosReleaseRef(tsFileRsetId, tfd);
  return ret;
}

int32_t tfFsync(int64_t tfd) {
  void *p = taosAcquireRef(tsFileRsetId, tfd);
  if (p == NULL) return -1;

  int32_t fd = (int32_t)(uintptr_t)p;
  int32_t code = taosFsyncFile(fd);

  taosReleaseRef(tsFileRsetId, tfd);
  return code;
}

bool tfValid(int64_t tfd) {
  void *p = taosAcquireRef(tsFileRsetId, tfd);
  if (p == NULL) return false;

  taosReleaseRef(tsFileRsetId, tfd);
  return true;
}

int64_t tfLseek(int64_t tfd, int64_t offset, int32_t whence) {
  void *p = taosAcquireRef(tsFileRsetId, tfd);
  if (p == NULL) return -1;

  int32_t fd = (int32_t)(uintptr_t)p;
  int64_t ret = taosLSeekFile(fd, offset, whence);

  taosReleaseRef(tsFileRsetId, tfd);
  return ret;
}

int32_t tfFtruncate(int64_t tfd, int64_t length) {
  void *p = taosAcquireRef(tsFileRsetId, tfd);
  if (p == NULL) return -1;

  int32_t fd = (int32_t)(uintptr_t)p;
  int32_t code = taosFtruncateFile(fd, length);

  taosReleaseRef(tsFileRsetId, tfd);
  return code;
}
