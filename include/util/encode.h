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

#ifndef _TD_UTIL_ENCODE_H_
#define _TD_UTIL_ENCODE_H_

#include "freelist.h"
#include "tcoding.h"
#include "tmacro.h"

#ifdef __cplusplus
extern "C" {
#endif

#define tPut(TYPE, BUF, VAL) ((TYPE*)(BUF))[0] = (VAL)
#define tGet(TYPE, BUF, VAL) (VAL) = ((TYPE*)(BUF))[0]

#define tRPut16(PDEST, PSRC)                      \
  ((uint8_t*)(PDEST))[0] = ((uint8_t*)(PSRC))[1]; \
  ((uint8_t*)(PDEST))[1] = ((uint8_t*)(PSRC))[0];

#define tRPut32(PDEST, PSRC)                      \
  ((uint8_t*)(PDEST))[0] = ((uint8_t*)(PSRC))[3]; \
  ((uint8_t*)(PDEST))[1] = ((uint8_t*)(PSRC))[2]; \
  ((uint8_t*)(PDEST))[2] = ((uint8_t*)(PSRC))[1]; \
  ((uint8_t*)(PDEST))[3] = ((uint8_t*)(PSRC))[0];

#define tRPut64(PDEST, PSRC)                      \
  ((uint8_t*)(PDEST))[0] = ((uint8_t*)(PSRC))[7]; \
  ((uint8_t*)(PDEST))[1] = ((uint8_t*)(PSRC))[6]; \
  ((uint8_t*)(PDEST))[2] = ((uint8_t*)(PSRC))[5]; \
  ((uint8_t*)(PDEST))[3] = ((uint8_t*)(PSRC))[4]; \
  ((uint8_t*)(PDEST))[4] = ((uint8_t*)(PSRC))[3]; \
  ((uint8_t*)(PDEST))[5] = ((uint8_t*)(PSRC))[2]; \
  ((uint8_t*)(PDEST))[6] = ((uint8_t*)(PSRC))[1]; \
  ((uint8_t*)(PDEST))[7] = ((uint8_t*)(PSRC))[0];

#define tRGet16 tRPut16
#define tRGet32 tRPut32
#define tRGet64 tRPut64

typedef enum { TD_ENCODER, TD_DECODER } td_coder_t;

#define CODER_NODE_FIELDS \
  uint8_t* data;          \
  int32_t  size;          \
  int32_t  pos;

struct SCoderNode {
  TD_SLIST_NODE(SCoderNode);
  CODER_NODE_FIELDS
};

typedef struct {
  td_coder_t  type;
  td_endian_t endian;
  SFreeList   fl;
  CODER_NODE_FIELDS
  TD_SLIST(SCoderNode) stack;
} SCoder;

#define TD_CODER_POS(CODER) ((CODER)->pos)
#define TD_CODER_CURRENT(CODER) ((CODER)->data + (CODER)->pos)
#define TD_CODER_MOVE_POS(CODER, MOVE) ((CODER)->pos += (MOVE))
#define TD_CODER_CHECK_CAPACITY_FAILED(CODER, EXPSIZE) (((CODER)->size - (CODER)->pos) < (EXPSIZE))
#define TCODER_MALLOC(SIZE, CODER) TFL_MALLOC(SIZE, &((CODER)->fl))

void tCoderInit(SCoder* pCoder, td_endian_t endian, uint8_t* data, int32_t size, td_coder_t type);
void tCoderClear(SCoder* pCoder);

/* ------------------------ ENCODE ------------------------ */
static int  tStartEncode(SCoder* pEncoder);
static void tEndEncode(SCoder* pEncoder);
static int  tEncodeU8(SCoder* pEncoder, uint8_t val);
static int  tEncodeI8(SCoder* pEncoder, int8_t val);
static int  tEncodeU16(SCoder* pEncoder, uint16_t val);
static int  tEncodeI16(SCoder* pEncoder, int16_t val);
static int  tEncodeU32(SCoder* pEncoder, uint32_t val);
static int  tEncodeI32(SCoder* pEncoder, int32_t val);
static int  tEncodeU64(SCoder* pEncoder, uint64_t val);
static int  tEncodeI64(SCoder* pEncoder, int64_t val);
static int  tEncodeU16v(SCoder* pEncoder, uint16_t val);
static int  tEncodeI16v(SCoder* pEncoder, int16_t val);
static int  tEncodeU32v(SCoder* pEncoder, uint32_t val);
static int  tEncodeI32v(SCoder* pEncoder, int32_t val);
static int  tEncodeU64v(SCoder* pEncoder, uint64_t val);
static int  tEncodeI64v(SCoder* pEncoder, int64_t val);
static int  tEncodeFloat(SCoder* pEncoder, float val);
static int  tEncodeDouble(SCoder* pEncoder, double val);
static int  tEncodeCstrWithLen(SCoder* pEncoder, const char* val, uint64_t len);
static int  tEncodeCStr(SCoder* pEncoder, const char* val);

/* ------------------------ DECODE ------------------------ */
static int  tStartDecode(SCoder* pDecoder);
static void tEndDecode(SCoder* pDecoder);
static int  tDecodeU8(SCoder* pDecoder, uint8_t* val);
static int  tDecodeI8(SCoder* pDecoder, int8_t* val);
static int  tDecodeU16(SCoder* pDecoder, uint16_t* val);
static int  tDecodeI16(SCoder* pDecoder, int16_t* val);
static int  tDecodeU32(SCoder* pDecoder, uint32_t* val);
static int  tDecodeI32(SCoder* pDecoder, int32_t* val);
static int  tDecodeU64(SCoder* pDecoder, uint64_t* val);
static int  tDecodeI64(SCoder* pDecoder, int64_t* val);
static int  tDecodeU16v(SCoder* pDecoder, uint16_t* val);
static int  tDecodeI16v(SCoder* pDecoder, int16_t* val);
static int  tDecodeU32v(SCoder* pDecoder, uint32_t* val);
static int  tDecodeI32v(SCoder* pDecoder, int32_t* val);
static int  tDecodeU64v(SCoder* pDecoder, uint64_t* val);
static int  tDecodeI64v(SCoder* pDecoder, int64_t* val);
static int  tDecodeFloat(SCoder* pDecoder, float* val);
static int  tDecodeDouble(SCoder* pDecoder, double* val);
static int  tDecodeCStrAndLen(SCoder* pDecoder, const char** val, uint64_t* len);
static int  tDecodeCStr(SCoder* pEncoder, const char** val);

/* ------------------------ IMPL ------------------------ */
#define TD_ENCODE_MACRO(CODER, VAL, TYPE, BITS)                        \
  if ((CODER)->data) {                                                 \
    if (TD_CODER_CHECK_CAPACITY_FAILED(CODER, sizeof(VAL))) return -1; \
    if (TD_RT_ENDIAN() == (CODER)->endian) {                           \
      tPut(TYPE, TD_CODER_CURRENT(CODER), (VAL));                      \
    } else {                                                           \
      tRPut##BITS(TD_CODER_CURRENT(CODER), &(VAL));                    \
    }                                                                  \
  }                                                                    \
  TD_CODER_MOVE_POS(CODER, sizeof(VAL));                               \
  return 0;

#define TD_ENCODE_VARIANT_MACRO(CODER, VAL)                       \
  while ((VAL) > ENCODE_LIMIT) {                                  \
    if ((CODER)->data) {                                          \
      if (TD_CODER_CHECK_CAPACITY_FAILED(CODER, 1)) return -1;    \
      TD_CODER_CURRENT(CODER)[0] = ((VAL) | ENCODE_LIMIT) & 0xff; \
    }                                                             \
                                                                  \
    (VAL) >>= 7;                                                  \
    TD_CODER_MOVE_POS(CODER, 1);                                  \
  }                                                               \
                                                                  \
  if ((CODER)->data) {                                            \
    if (TD_CODER_CHECK_CAPACITY_FAILED(CODER, 1)) return -1;      \
    TD_CODER_CURRENT(CODER)[0] = (uint8_t)(VAL);                  \
  }                                                               \
  TD_CODER_MOVE_POS(CODER, 1);                                    \
  return 0;

#define TD_DECODE_MACRO(CODER, PVAL, TYPE, BITS)                         \
  if (TD_CODER_CHECK_CAPACITY_FAILED(CODER, sizeof(*(PVAL)))) return -1; \
  if (TD_RT_ENDIAN() == (CODER)->endian) {                               \
    tGet(TYPE, TD_CODER_CURRENT(CODER), *(PVAL));                        \
  } else {                                                               \
    tRGet##BITS(PVAL, TD_CODER_CURRENT(CODER));                          \
  }                                                                      \
                                                                         \
  TD_CODER_MOVE_POS(CODER, sizeof(*(PVAL)));                             \
  return 0;

#define TD_DECODE_VARIANT_MACRO(CODER, PVAL, TYPE)           \
  int32_t i = 0;                                             \
  *(PVAL) = 0;                                               \
  for (;;) {                                                 \
    if (TD_CODER_CHECK_CAPACITY_FAILED(CODER, 1)) return -1; \
    TYPE tval = TD_CODER_CURRENT(CODER)[0];                  \
    if (tval < ENCODE_LIMIT) {                               \
      *(PVAL) |= (tval << (7 * i));                          \
      TD_CODER_MOVE_POS(pDecoder, 1);                        \
      break;                                                 \
    } else {                                                 \
      *(PVAL) |= (((tval) & (ENCODE_LIMIT - 1)) << (7 * i)); \
      i++;                                                   \
      TD_CODER_MOVE_POS(pDecoder, 1);                        \
    }                                                        \
  }                                                          \
                                                             \
  return 0;

// 8
static FORCE_INLINE int tEncodeU8(SCoder* pEncoder, uint8_t val) {
  if (pEncoder->data) {
    if (TD_CODER_CHECK_CAPACITY_FAILED(pEncoder, sizeof(val))) return -1;
    tPut(uint8_t, TD_CODER_CURRENT(pEncoder), val);
  }
  TD_CODER_MOVE_POS(pEncoder, sizeof(val));
  return 0;
}

static FORCE_INLINE int tEncodeI8(SCoder* pEncoder, int8_t val) {
  if (pEncoder->data) {
    if (TD_CODER_CHECK_CAPACITY_FAILED(pEncoder, sizeof(val))) return -1;
    tPut(int8_t, TD_CODER_CURRENT(pEncoder), val);
  }
  TD_CODER_MOVE_POS(pEncoder, sizeof(val));
  return 0;
}

// 16
static FORCE_INLINE int tEncodeU16(SCoder* pEncoder, uint16_t val) { TD_ENCODE_MACRO(pEncoder, val, uint16_t, 16); }
static FORCE_INLINE int tEncodeI16(SCoder* pEncoder, int16_t val) { TD_ENCODE_MACRO(pEncoder, val, int16_t, 16); }
// 32
static FORCE_INLINE int tEncodeU32(SCoder* pEncoder, uint32_t val) { TD_ENCODE_MACRO(pEncoder, val, uint32_t, 32); }
static FORCE_INLINE int tEncodeI32(SCoder* pEncoder, int32_t val) { TD_ENCODE_MACRO(pEncoder, val, int32_t, 32); }
// 64
static FORCE_INLINE int tEncodeU64(SCoder* pEncoder, uint64_t val) { TD_ENCODE_MACRO(pEncoder, val, uint64_t, 64); }
static FORCE_INLINE int tEncodeI64(SCoder* pEncoder, int64_t val) { TD_ENCODE_MACRO(pEncoder, val, int64_t, 64); }
// 16v
static FORCE_INLINE int tEncodeU16v(SCoder* pEncoder, uint16_t val) { TD_ENCODE_VARIANT_MACRO(pEncoder, val); }
static FORCE_INLINE int tEncodeI16v(SCoder* pEncoder, int16_t val) {
  return tEncodeU16v(pEncoder, ZIGZAGE(int16_t, val));
}
// 32v
static FORCE_INLINE int tEncodeU32v(SCoder* pEncoder, uint32_t val) { TD_ENCODE_VARIANT_MACRO(pEncoder, val); }
static FORCE_INLINE int tEncodeI32v(SCoder* pEncoder, int32_t val) {
  return tEncodeU32v(pEncoder, ZIGZAGE(int32_t, val));
}
// 64v
static FORCE_INLINE int tEncodeU64v(SCoder* pEncoder, uint64_t val) { TD_ENCODE_VARIANT_MACRO(pEncoder, val); }
static FORCE_INLINE int tEncodeI64v(SCoder* pEncoder, int64_t val) {
  return tEncodeU64v(pEncoder, ZIGZAGE(int64_t, val));
}

static FORCE_INLINE int tEncodeFloat(SCoder* pEncoder, float val) {
  union {
    uint32_t ui;
    float    f;
  } v = {.f = val};

  return tEncodeU32(pEncoder, v.ui);
}

static FORCE_INLINE int tEncodeDouble(SCoder* pEncoder, double val) {
  union {
    uint64_t ui;
    double   d;
  } v = {.d = val};

  return tEncodeU64(pEncoder, v.ui);
}

static FORCE_INLINE int tEncodeCstrWithLen(SCoder* pEncoder, const char* val, uint64_t len) {
  if (tEncodeU64v(pEncoder, len) < 0) return -1;
  if (pEncoder->data) {
    if (TD_CODER_CHECK_CAPACITY_FAILED(pEncoder, len + 1)) return -1;
    memcpy(TD_CODER_CURRENT(pEncoder), val, len + 1);
  }
  TD_CODER_MOVE_POS(pEncoder, len + 1);
  return 0;
}

static FORCE_INLINE int tEncodeCStr(SCoder* pEncoder, const char* val) {
  return tEncodeCstrWithLen(pEncoder, val, (uint64_t)strlen(val));
}

/* ------------------------ FOR DECODER ------------------------ */
// 8
static FORCE_INLINE int tDecodeU8(SCoder* pDecoder, uint8_t* val) {
  if (TD_CODER_CHECK_CAPACITY_FAILED(pDecoder, sizeof(*val))) return -1;
  tGet(uint8_t, TD_CODER_CURRENT(pDecoder), *val);
  TD_CODER_MOVE_POS(pDecoder, sizeof(*val));
  return 0;
}

static FORCE_INLINE int tDecodeI8(SCoder* pDecoder, int8_t* val) {
  if (TD_CODER_CHECK_CAPACITY_FAILED(pDecoder, sizeof(*val))) return -1;
  tGet(int8_t, TD_CODER_CURRENT(pDecoder), *val);
  TD_CODER_MOVE_POS(pDecoder, sizeof(*val));
  return 0;
}

// 16
static FORCE_INLINE int tDecodeU16(SCoder* pDecoder, uint16_t* val) { TD_DECODE_MACRO(pDecoder, val, uint16_t, 16); }
static FORCE_INLINE int tDecodeI16(SCoder* pDecoder, int16_t* val) { TD_DECODE_MACRO(pDecoder, val, int16_t, 16); }
// 32
static FORCE_INLINE int tDecodeU32(SCoder* pDecoder, uint32_t* val) { TD_DECODE_MACRO(pDecoder, val, uint32_t, 32); }
static FORCE_INLINE int tDecodeI32(SCoder* pDecoder, int32_t* val) { TD_DECODE_MACRO(pDecoder, val, int32_t, 32); }
// 64
static FORCE_INLINE int tDecodeU64(SCoder* pDecoder, uint64_t* val) { TD_DECODE_MACRO(pDecoder, val, uint64_t, 64); }
static FORCE_INLINE int tDecodeI64(SCoder* pDecoder, int64_t* val) { TD_DECODE_MACRO(pDecoder, val, int64_t, 64); }

// 16v
static FORCE_INLINE int tDecodeU16v(SCoder* pDecoder, uint16_t* val) {
  TD_DECODE_VARIANT_MACRO(pDecoder, val, uint16_t);
}

static FORCE_INLINE int tDecodeI16v(SCoder* pDecoder, int16_t* val) {
  uint16_t tval;
  if (tDecodeU16v(pDecoder, &tval) < 0) {
    return -1;
  }
  *val = ZIGZAGD(int16_t, tval);
  return 0;
}

// 32v
static FORCE_INLINE int tDecodeU32v(SCoder* pDecoder, uint32_t* val) {
  TD_DECODE_VARIANT_MACRO(pDecoder, val, uint32_t);
}

static FORCE_INLINE int tDecodeI32v(SCoder* pDecoder, int32_t* val) {
  uint32_t tval;
  if (tDecodeU32v(pDecoder, &tval) < 0) {
    return -1;
  }
  *val = ZIGZAGD(int32_t, tval);
  return 0;
}

// 64v
static FORCE_INLINE int tDecodeU64v(SCoder* pDecoder, uint64_t* val) {
  TD_DECODE_VARIANT_MACRO(pDecoder, val, uint64_t);
}

static FORCE_INLINE int tDecodeI64v(SCoder* pDecoder, int64_t* val) {
  uint64_t tval;
  if (tDecodeU64v(pDecoder, &tval) < 0) {
    return -1;
  }
  *val = ZIGZAGD(int64_t, tval);
  return 0;
}

static FORCE_INLINE int tDecodeFloat(SCoder* pDecoder, float* val) {
  union {
    uint32_t ui;
    float    f;
  } v;

  if (tDecodeU32(pDecoder, &(v.ui)) < 0) {
    return -1;
  }

  *val = v.f;
  return 0;
}

static FORCE_INLINE int tDecodeDouble(SCoder* pDecoder, double* val) {
  union {
    uint64_t ui;
    double   d;
  } v;

  if (tDecodeU64(pDecoder, &(v.ui)) < 0) {
    return -1;
  }

  *val = v.d;
  return 0;
}

static FORCE_INLINE int tDecodeCStrAndLen(SCoder* pDecoder, const char** val, uint64_t* len) {
  if (tDecodeU64v(pDecoder, len) < 0) return -1;

  if (TD_CODER_CHECK_CAPACITY_FAILED(pDecoder, *len + 1)) return -1;
  *val = (char*)TD_CODER_CURRENT(pDecoder);

  TD_CODER_MOVE_POS(pDecoder, *len + 1);
  return 0;
}

static FORCE_INLINE int tDecodeCStr(SCoder* pDecoder, const char** val) {
  uint64_t len;
  return tDecodeCStrAndLen(pDecoder, val, &len);
}

#ifdef __cplusplus
}
#endif

#endif /*_TD_UTIL_ENCODE_H_*/