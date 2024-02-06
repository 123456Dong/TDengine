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
#include "tbase58.h"
#include <math.h>
#include <stdbool.h>

#define BASE_BUF_SIZE 256
static const char *basis_58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

char *base58_encode(const uint8_t *value, int32_t vlen) {
  const uint8_t *pb = value;
  const uint8_t *pe = pb + vlen;
  uint8_t        buf[BASE_BUF_SIZE] = {0};
  uint8_t       *pbuf = &buf[0];
  bool           bfree = false;
  int32_t        nz = 0, size = 0, len = 0;

  while (pb != pe && *pb == 0) {
    ++pb;
    ++nz;
  }

  size = (pe - pb) * 69 / 50 + 1;
  if (size > BASE_BUF_SIZE) {
    if (!(pbuf = taosMemoryCalloc(1, size))) {
      terrno = TSDB_CODE_OUT_OF_MEMORY;
      return NULL;
    }
    bfree = true;
  }

  while (pb != pe) {
    int32_t num = *pb;
    int32_t i = 0;
    for (int32_t j = (int32_t)size - 1; (num != 0 || i < len) && j >= 0; --j, ++i) {
      num += ((int32_t)buf[j]) << 8;
      pbuf[j] = num % 58;
      num /= 58;
    }
    len = i;
    ++pb;
  }

  const uint8_t *pi = pbuf + (size - len);
  while (pi != pbuf + size && *pi == 0) ++pi;
  uint8_t *result = taosMemoryCalloc(1, size + 1);
  if (!result) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    if (bfree) taosMemoryFree(pbuf);
    return NULL;
  }
  memset(result, '1', nz);
  while (pi != pbuf + size) result[nz++] = basis_58[*pi++];

  if (bfree) taosMemoryFree(pbuf);
  return result;
}

static const signed char index_58[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,
    -1, -1, -1, -1, -1, -1, -1, 9,  10, 11, 12, 13, 14, 15, 16, -1, 17, 18, 19, 20, 21, -1, 22, 23, 24, 25, 26, 27, 28,
    29, 30, 31, 32, -1, -1, -1, -1, -1, -1, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, -1, 44, 45, 46, 47, 48, 49, 50,
    51, 52, 53, 54, 55, 56, 57, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

uint8_t *base58_decode(const char *value, size_t inlen, int32_t *outlen) {
  const char *pe = value + inlen;
  uint8_t     buf[BASE_BUF_SIZE] = {0};
  uint8_t    *pbuf = &buf[0];
  bool        bfree = false;
  int32_t     nz = 0, size = 0, len = 0;

  while (*value && isspace(*value)) ++value;
  while (*value == '1') {
    ++nz;
    ++value;
  }

  size = (int32_t)(pe - value) * 733 / 1000 + 1;
  if (size > BASE_BUF_SIZE) {
    if (!(pbuf = taosMemoryCalloc(1, size))) {
      terrno = TSDB_CODE_OUT_OF_MEMORY;
      return NULL;
    }
    bfree = true;
  }

  while (*value && !isspace(*value)) {
    int32_t num = index_58[(uint8_t)*value];
    if (num == -1) {
      if (bfree) taosMemoryFree(pbuf);
      return NULL;
    }
    int32_t i = 0;
    for (int32_t j = size - 1; (num != 0 || i < len) && (j >= 0); --j, ++i) {
      num += (int32_t)pbuf[j] * 58;
      pbuf[j] = num & 255;
      num >>= 8;
    }
    len = i;
    ++value;
  }

  while (isspace(*value)) ++value;
  if (*value != 0) {
    if (bfree) taosMemoryFree(pbuf);
    return NULL;
  }
  const uint8_t *it = pbuf + (size - len);
  while (it != pbuf + size && *it == 0) ++it;

  uint8_t *result = taosMemoryCalloc(1, size + 1);
  if (!result) {
    if (bfree) taosMemoryFree(pbuf);
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }

  memset(result, 0, nz);
  while (it != pbuf + size) result[nz++] = *it++;

  if (outlen) *outlen = nz;

  if (bfree) taosMemoryFree(pbuf);
  return result;
}