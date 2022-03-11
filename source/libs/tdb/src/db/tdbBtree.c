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

#include "tdbInt.h"

#define TDB_BTREE_ROOT 0x1
#define TDB_BTREE_LEAF 0x2

#define TDB_BTREE_PAGE_IS_ROOT(flags) TDB_FLAG_HAS(flags, TDB_BTREE_ROOT)
#define TDB_BTREE_PAGE_IS_LEAF(flags) TDB_FLAG_HAS(flags, TDB_BTREE_LEAF)
#define TDB_BTREE_ASSERT_FLAG(flags)                                                 \
  ASSERT(TDB_FLAG_IS(flags, TDB_BTREE_ROOT) || TDB_FLAG_IS(flags, TDB_BTREE_LEAF) || \
         TDB_FLAG_IS(flags, TDB_BTREE_ROOT | TDB_BTREE_LEAF) || TDB_FLAG_IS(flags, 0))

struct SBTree {
  SPgno          root;
  int            keyLen;
  int            valLen;
  SPager        *pPager;
  FKeyComparator kcmpr;
  u8             fanout;
  int            pageSize;
  int            maxLocal;
  int            minLocal;
  int            maxLeaf;
  int            minLeaf;
};

typedef struct SFreeCell {
  u16 size;
  u16 next;
} SFreeCell;

typedef struct {
  u16     flags;
  SBTree *pBt;
} SBtreeZeroPageArg;

static int tdbBtCursorMoveTo(SBtCursor *pCur, const void *pKey, int kLen);
static int tdbEncodeLength(u8 *pBuf, uint32_t len);
static int tdbCompareKeyAndCell(const void *pKey, int kLen, const void *pCell);
static int tdbDefaultKeyCmprFn(const void *pKey1, int keyLen1, const void *pKey2, int keyLen2);
static int tdbBtreeOpenImpl(SBTree *pBt);
static int tdbBtreeZeroPage(SPage *pPage, void *arg);
static int tdbBtreeInitPage(SPage *pPage, void *arg);

int tdbBtreeOpen(int keyLen, int valLen, SPager *pPager, FKeyComparator kcmpr, SBTree **ppBt) {
  SBTree *pBt;
  int     ret;

  *ppBt = NULL;

  pBt = (SBTree *)calloc(1, sizeof(*pBt));
  if (pBt == NULL) {
    return -1;
  }

  // pBt->keyLen
  pBt->keyLen = keyLen;
  // pBt->valLen
  pBt->valLen = valLen;
  // pBt->pPager
  pBt->pPager = pPager;
  // pBt->kcmpr
  pBt->kcmpr = kcmpr ? kcmpr : tdbDefaultKeyCmprFn;
  // pBt->fanout
  if (keyLen == TDB_VARIANT_LEN) {
    pBt->fanout = TDB_DEFAULT_FANOUT;
  } else {
    ASSERT(0);
    // TODO: pBt->fanout = 0;
  }
  // pBt->pageSize
  pBt->pageSize = tdbPagerGetPageSize(pPager);
  // pBt->maxLocal
  pBt->maxLocal = (pBt->pageSize - sizeof(SPageHdr)) / pBt->fanout;
  // pBt->minLocal: Should not be allowed smaller than 15, which is [nPayload][nKey][nData]
  pBt->minLocal = (pBt->pageSize - sizeof(SPageHdr)) / pBt->fanout / 2;
  // pBt->maxLeaf
  pBt->maxLeaf = pBt->pageSize - sizeof(SPageHdr);
  // pBt->minLeaf
  pBt->minLeaf = pBt->minLocal;

  // TODO: pBt->root
  ret = tdbBtreeOpenImpl(pBt);
  if (ret < 0) {
    free(pBt);
    return -1;
  }

  *ppBt = pBt;
  return 0;
}

int tdbBtreeClose(SBTree *pBt) {
  // TODO
  return 0;
}

int tdbBtreeCursor(SBtCursor *pCur, SBTree *pBt) {
  pCur->pBt = pBt;
  pCur->iPage = -1;
  pCur->pPage = NULL;
  pCur->idx = -1;

  return 0;
}

int tdbBtCursorInsert(SBtCursor *pCur, const void *pKey, int kLen, const void *pVal, int vLen) {
  int     ret;
  SPager *pPager;

  ret = tdbBtCursorMoveTo(pCur, pKey, kLen);
  if (ret < 0) {
    // TODO: handle error
    return -1;
  }

  if (pCur->idx == -1) {
    ASSERT(pCur->pPage->pPageHdr->nCells == 0);
    // TODO: insert the K-V pair to idx 0
  }

  return 0;
}

static int tdbBtCursorMoveTo(SBtCursor *pCur, const void *pKey, int kLen) {
  int     ret;
  SBTree *pBt;
  SPager *pPager;

  pBt = pCur->pBt;
  pPager = pBt->pPager;

  if (pCur->iPage < 0) {
    ASSERT(pCur->iPage == -1);
    ASSERT(pCur->idx == -1);

    // Move from the root
    ret = tdbPagerFetchPage(pPager, pBt->root, &(pCur->pPage), tdbBtreeInitPage, pBt);
    if (ret < 0) {
      ASSERT(0);
      return -1;
    }

    pCur->iPage = 0;

    if (pCur->pPage->pPageHdr->nCells == 0) {
      // Current page is empty
      ASSERT(TDB_FLAG_IS(pCur->pPage->pPageHdr->flags, TDB_BTREE_ROOT | TDB_BTREE_LEAF));
      return 0;
    }

    // Search from root page down to leaf
    {
      // TODO
      ASSERT(0);
      // ret = tdbBtCursorMoveToRoot(pCur);
      // if (ret < 0) {
      //   return -1;
      // }

      // if (pCur->pPage->pHdr->nCells == 0) {
      //   // Tree is empty
      // } else {
      //   for (;;) {
      //     int lidx, ridx, midx, c;

      //     pBtPage = pCur->pPage;
      //     lidx = 0;
      //     ridx = pBtPage->pHdr->nCells - 1;
      //     while (lidx <= ridx) {
      //       midx = (lidx + ridx) >> 1;
      //       pCell = (void *)(pBtPage->aData + pBtPage->aCellIdx[midx]);

      //       c = tdbCompareKeyAndCell(pKey, kLen, pCell);
      //       if (c == 0) {
      //         break;
      //       } else if (c < 0) {
      //         lidx = lidx + 1;
      //       } else {
      //         ridx = ridx - 1;
      //       }
      //     }
      //   }

      //   /* code */
      // }
    }

  } else {
    // TODO: Move the cursor from a some position instead of a clear state
  }

  return 0;
}

static int tdbEncodeKeyValue(const void *pKey, int kLen, int kLenG, const void *pVal, int vLen, int vLenG, void *pBuf,
                             int *bLen) {
  u8 *pPtr;

  ASSERT(kLen > 0 && vLen > 0);
  ASSERT(kLenG == TDB_VARIANT_LEN || kLenG == kLen);
  ASSERT(vLenG == TDB_VARIANT_LEN || vLenG == vLen);

  pPtr = (u8 *)pBuf;
  if (kLenG == TDB_VARIANT_LEN) {
    pPtr += tdbEncodeLength(pPtr, kLen);
  }

  if (vLenG == TDB_VARIANT_LEN) {
    pPtr += tdbEncodeLength(pPtr, vLen);
  }

  memcpy(pPtr, pKey, kLen);
  pPtr += kLen;

  memcpy(pPtr, pVal, vLen);
  pPtr += vLen;

  *bLen = pPtr - (u8 *)pBuf;
  return 0;
}

static int tdbDecodeKeyValue(const void *pBuf, void *pKey, int *kLen, void *pVal, int *vLen) {
  if (*kLen == TDB_VARIANT_LEN) {
    // Decode the key length
  }

  if (*vLen == TDB_VARIANT_LEN) {
    // Decode the value length
  }

  // TODO: decode the key and value

  return 0;
}

static int tdbEncodeLength(u8 *pBuf, uint32_t len) {
  int iCount = 0;

  while (len > 127) {
    pBuf[iCount++] = (u8)((len & 0xff) | 128);
    len >>= 7;
  }

  pBuf[iCount++] = (u8)len;

  return iCount;
}

static int tdbBtCursorMoveToRoot(SBtCursor *pCur) {
  SBTree *pBt;
  SPager *pPager;
  SPage  *pPage;
  int     ret;

  pBt = pCur->pBt;
  pPager = pBt->pPager;

  // pPage = tdbPagerGet(pPager, pBt->root, true);
  // if (pPage == NULL) {
  //   // TODO: handle error
  // }

  // ret = tdbInitBtPage(pPage, &pBtPage);
  // if (ret < 0) {
  //   // TODO
  //   return 0;
  // }

  // pCur->pPage = pBtPage;
  // pCur->iPage = 0;

  return 0;
}

static int tdbCompareKeyAndCell(const void *pKey, int kLen, const void *pCell) {
  /* TODO */
  return 0;
}

static int tdbDefaultKeyCmprFn(const void *pKey1, int keyLen1, const void *pKey2, int keyLen2) {
  int mlen;
  int cret;

  ASSERT(keyLen1 > 0 && keyLen2 > 0 && pKey1 != NULL && pKey2 != NULL);

  mlen = keyLen1 < keyLen2 ? keyLen1 : keyLen2;
  cret = memcmp(pKey1, pKey2, mlen);
  if (cret == 0) {
    if (keyLen1 < keyLen2) {
      cret = -1;
    } else if (keyLen1 > keyLen2) {
      cret = 1;
    } else {
      cret = 0;
    }
  }
  return cret;
}

static int tdbBtreeOpenImpl(SBTree *pBt) {
  // Try to get the root page of the an existing btree

  SPgno  pgno;
  SPage *pPage;
  int    ret;

  {
    // 1. TODO: Search the main DB to check if the DB exists
    pgno = 0;
  }

  if (pgno != 0) {
    pBt->root = pgno;
    return 0;
  }

  // Try to create a new database
  SBtreeZeroPageArg zArg = {.flags = TDB_BTREE_ROOT | TDB_BTREE_LEAF, .pBt = pBt};
  ret = tdbPagerNewPage(pBt->pPager, &pgno, &pPage, tdbBtreeZeroPage, &zArg);
  if (ret < 0) {
    return -1;
  }

  // TODO: Unref the page

  ASSERT(pgno != 0);
  pBt->root = pgno;

  return 0;
}

static int tdbBtreeZeroPage(SPage *pPage, void *arg) {
  u16     flags;
  SBTree *pBt;

  flags = ((SBtreeZeroPageArg *)arg)->flags;
  pBt = ((SBtreeZeroPageArg *)arg)->pBt;

  pPage->pPageHdr = (SPageHdr *)pPage->pData;
  pPage->aCellIdx = (u8 *)(&(pPage->pPageHdr[1]));

  // Init the page header
  pPage->pPageHdr->flags = flags;
  pPage->pPageHdr->nCells = 0;
  pPage->pPageHdr->cellCont = pBt->pageSize;
  pPage->pPageHdr->freeCell = 0;
  pPage->pPageHdr->nFree = 0;

  TDB_BTREE_ASSERT_FLAG(flags);

  if (TDB_BTREE_PAGE_IS_LEAF(pPage->pPageHdr->flags)) {
    pPage->kLen = pBt->keyLen;
    pPage->vLen = pBt->valLen;
    pPage->maxLocal = pBt->maxLeaf;
    pPage->minLocal = pBt->minLeaf;
  } else {
    pPage->kLen = pBt->keyLen;
    pPage->vLen = sizeof(SPgno);
    pPage->maxLocal = pBt->maxLocal;
    pPage->minLocal = pBt->minLocal;
  }

  return 0;
}

static int tdbBtreeInitPage(SPage *pPage, void *arg) {
  SBTree *pBt;

  pBt = (SBTree *)arg;

  pPage->pPageHdr = (SPageHdr *)pPage->pData;
  pPage->aCellIdx = (u8 *)(&(pPage->pPageHdr[1]));

  TDB_BTREE_ASSERT_FLAG(pPage->pPageHdr->flags);

  // Init other fields
  if (TDB_BTREE_PAGE_IS_LEAF(pPage->pPageHdr->flags)) {
    pPage->kLen = pBt->keyLen;
    pPage->vLen = pBt->valLen;
    pPage->maxLocal = pBt->maxLeaf;
    pPage->minLocal = pBt->minLeaf;
  } else {
    pPage->kLen = pBt->keyLen;
    pPage->vLen = sizeof(SPgno);
    pPage->maxLocal = pBt->maxLocal;
    pPage->minLocal = pBt->minLocal;
  }

  return 0;
}

#ifndef TDB_BTREE_BALANCE
typedef struct {
  SBTree *pBt;
  SPage  *pParent;
  int     idx;
  i8      nOldPages;
  SPage  *pOldPages[3];
  i8      nNewPages;
  SPage  *pNewPages[5];
} SBtreeBalanceHelper;

static int tdbBtreeCopyPageContent(SPage *pFrom, SPage *pTo) {
  /* TODO */

  return 0;
}

static int tdbBtreeBalanceDeeper(SBTree *pBt, SPage *pRoot, SPage **ppChild) {
  SPager           *pPager;
  SPage            *pChild;
  SPgno             pgnoChild;
  int               ret;
  SBtreeZeroPageArg zArg;

  pPager = pRoot->pPager;

  // Allocate a new child page
  zArg.flags = TDB_BTREE_LEAF;
  zArg.pBt = pBt;
  ret = tdbPagerNewPage(pPager, &pgnoChild, &pChild, tdbBtreeZeroPage, &zArg);
  if (ret < 0) {
    return -1;
  }

  // Copy the root page content to the child page
  ret = tdbBtreeCopyPageContent(pRoot, pChild);
  if (ret < 0) {
    return -1;
  }

  {
    // TODO: Copy the over flow part of the page
  }

  // Reinitialize the root page
  zArg.flags = TDB_BTREE_ROOT;
  zArg.pBt = pBt;
  ret = tdbBtreeZeroPage(pRoot, &zArg);
  if (ret < 0) {
    return -1;
  }

  *ppChild = pChild;
  return 0;
}

static int tdbBtreeBalanceStep1(SBtreeBalanceHelper *pBlh) {
  int    i;
  SPage *pParent;
  int    nDiv;
  SPgno  pgno;
  SPage *pPage;
  int    ret;

  pParent = pBlh->pParent;
  i = pParent->pPageHdr->nCells + pParent->nOverflow;

  if (i < 1) {
    nDiv = 0;
  } else {
    if (pBlh->idx == 0) {
      nDiv = 0;
    } else if (pBlh->idx == i) {
      nDiv = i - 2;
    } else {
      nDiv = pBlh->idx - 1;
    }
    i = 2;
  }
  pBlh->nOldPages = i + 1;

  if (i + nDiv - pParent->nOverflow == pParent->pPageHdr->nCells) {
    // pgno = pParent->pPageHdr->rChild;
  } else {
    ASSERT(0);
    // TODO
    pgno = 0;
  }
  for (;;) {
    ret = tdbPagerFetchPage(pBlh->pBt->pPager, pgno, &pPage, tdbBtreeInitPage, pBlh->pBt);
    if (ret < 0) {
      ASSERT(0);
      return -1;
    }

    pBlh->pOldPages[i] = pPage;

    if ((i--) == 0) break;

    if (pParent->nOverflow && i + nDiv == pParent->aiOvfl[0]) {
      // pCellDiv[i] = pParent->apOvfl[0];
      // pgno = 0;
      // szNew[i] = tdbPageCellSize(pPage, pCell);
      pParent->nOverflow = 0;
    } else {
      // pCellDiv[i] = TDB_PAGE_CELL_AT(pPage, i + nDiv - pParent->nOverflow);
      // pgno = 0;
      // szNew[i] = tdbPageCellSize(pPage, pCell);

      // Drop the cell from the page
      // ret = tdbPageDropCell(pPage, i + nDiv - pParent->nOverflow, szNew[i]);
      // if (ret < 0) {
      //   return -1;
      // }
    }
  }

  return 0;
}

static int tdbBtreeBalanceStep2(SBtreeBalanceHelper *pBlh) {
  SPage *pPage;
  int    oidx;
  int    cidx;
  int    limit;
  SCell *pCell;

  for (int i = 0; i < pBlh->nOldPages; i++) {
    pPage = pBlh->pOldPages[i];
    oidx = 0;
    cidx = 0;

    if (oidx < pPage->nOverflow) {
      limit = pPage->aiOvfl[oidx];
    } else {
      limit = pPage->pPageHdr->nCells;
    }

    // Loop to copy each cell pointer out
    for (;;) {
      if (oidx >= pPage->nOverflow && cidx >= pPage->pPageHdr->nCells) break;

      if (cidx < limit) {
        // Get local cells
        pCell = TDB_PAGE_CELL_AT(pPage, cidx);
      } else if (cidx == limit) {
        // Get overflow cells
        pCell = pPage->apOvfl[oidx++];

        if (oidx < pPage->nOverflow) {
          limit = pPage->aiOvfl[oidx];
        } else {
          limit = pPage->pPageHdr->nCells;
        }
      } else {
        ASSERT(0);
      }
    }

    {
      // TODO: Copy divider cells here
    }
  }

  /* TODO */

  return 0;
}

static int tdbBtreeBalanceStep3(SBtreeBalanceHelper *pBlh) {
  for (int i = 0; i < pBlh->nOldPages; i++) {
    /* code */
  }

  return 0;
}

static int tdbBtreeBalanceStep4(SBtreeBalanceHelper *pBlh) {
  // TODO
  return 0;
}

static int tdbBtreeBalanceStep5(SBtreeBalanceHelper *pBlh) {
  // TODO
  return 0;
}

static int tdbBtreeBalanceStep6(SBtreeBalanceHelper *pBlh) {
  // TODO
  return 0;
}

static int tdbBtreeBalanceNonRoot(SBTree *pBt, SPage *pParent, int idx) {
  int                 ret;
  SBtreeBalanceHelper blh;

  blh.pBt = pBt;
  blh.pParent = pParent;
  blh.idx = idx;

  // Step 1: find two sibling pages and get engough info about the old pages
  ret = tdbBtreeBalanceStep1(&blh);
  if (ret < 0) {
    ASSERT(0);
    return -1;
  }

  // Step 2: Load all cells on the old page and the divider cells
  ret = tdbBtreeBalanceStep2(&blh);
  if (ret < 0) {
    ASSERT(0);
    return -1;
  }

  // Step 3: Get the number of pages needed to hold all cells
  ret = tdbBtreeBalanceStep3(&blh);
  if (ret < 0) {
    ASSERT(0);
    return -1;
  }

  // Step 4: Allocate enough new pages. Reuse old pages as much as possible
  ret = tdbBtreeBalanceStep4(&blh);
  if (ret < 0) {
    ASSERT(0);
    return -1;
  }

  // Step 5: Insert new divider cells into pParent
  ret = tdbBtreeBalanceStep5(&blh);
  if (ret < 0) {
    ASSERT(0);
    return -1;
  }

  // Step 6: Update the sibling pages
  ret = tdbBtreeBalanceStep6(&blh);
  if (ret < 0) {
    ASSERT(0);
    return -1;
  }

  {
      // TODO: Reset states
  }

  {
    // TODO: Clear resources
  }

  return 0;
}

static int tdbBtreeBalance(SBtCursor *pCur) {
  int    iPage;
  SPage *pParent;
  int    ret;

  // Main loop to balance the BTree
  for (;;) {
    iPage = pCur->iPage;

    // TODO: Get the page free space if not get yet
    // if (pPage->nFree < 0) {
    //   if (tdbBtreeComputeFreeSpace(pPage) < 0) {
    //     return -1;
    //   }
    // }

    if (0 /*TODO: balance is over*/) {
      break;
    }

    if (iPage == 0) {
      // Balance the root page by copy the root page content to
      // a child page and set the root page as empty first
      ASSERT(TDB_BTREE_PAGE_IS_ROOT(pCur->pPage->pPageHdr->flags));

      ret = tdbBtreeBalanceDeeper(pCur->pBt, pCur->pPage, &(pCur->pgStack[1]));
      if (ret < 0) {
        return -1;
      }

      pCur->idx = 0;
      pCur->idxStack[0] = 0;
      pCur->pgStack[0] = pCur->pPage;
      pCur->iPage = 1;
      pCur->pPage = pCur->pgStack[1];

    } else {
      // Generalized balance step
      pParent = pCur->pgStack[pCur->iPage - 1];

      ret = tdbBtreeBalanceNonRoot(pCur->pBt, pParent, pCur->idxStack[pCur->iPage - 1]);
      if (ret < 0) {
        return -1;
      }

      pCur->iPage--;
      pCur->pPage = pCur->pgStack[pCur->iPage];
    }
  }

  return 0;
}
#endif