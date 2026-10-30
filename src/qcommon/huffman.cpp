#include <universal/q_shared.h>

#include "huffman.h"
#include <universal/assertive.h>

int bloc;

int __cdecl get_bit(const uint8_t *fin)
{
    int t; // [esp+0h] [ebp-4h]

    t = ((int)fin[bloc >> 3] >> (bloc & 7)) & 1;
    ++bloc;
    return t;
}

bool __cdecl Huff_offsetReceive(nodetype *node, int *ch, const uint8_t *fin, int *offset, int maxOffset)
{
    bloc = *offset;
    while (node && node->symbol == 257)
    {
        if (bloc >= maxOffset)
        {
            *ch = 0;
            *offset = bloc;
            return false;
        }
        if (get_bit(fin))
            node = node->right;
        else
            node = node->left;
    }
    if (node)
    {
        *ch = node->symbol;
        *offset = bloc;
        return true;
    }

    *ch = 0;
    *offset = bloc;
    return false;
}

void __cdecl huffman_send(nodetype *node, nodetype *child, uint8_t *fout)
{
    if (node->parent)
        huffman_send(node->parent, node, fout);
    if (child)
    {
        if (node->right == child)
            add_bit(1, fout);
        else
            add_bit(0, fout);
    }
}

void __cdecl add_bit(char bit, uint8_t *fout)
{
    if ((bloc & 7) == 0)
        fout[bloc >> 3] = 0;
    fout[bloc >> 3] |= bit << (bloc & 7);
    ++bloc;
}

int __cdecl huffman_bitCountForNode(nodetype *node, nodetype *child)
{
    int bits; // [esp+0h] [ebp-4h]

    bits = 0;
    if (node->parent)
        bits = huffman_bitCountForNode(node->parent, node);
    if (child)
        ++bits;
    return bits;
}

int __cdecl Huff_bitCount(huff_t *huff, uint32_t ch)
{
    if (ch >= 0x100)
        MyAssertHandler(".\\qcommon\\huffman.cpp", 152, 0, "ch doesn't index 256\n\t%i not in [0, %i)", ch, 256);
    if (!huff->loc[ch])
        MyAssertHandler(".\\qcommon\\huffman.cpp", 153, 0, "%s", "huff->loc[ch] != NULL");
    return huffman_bitCountForNode(huff->loc[ch], 0);
}

void __cdecl Huff_offsetTransmit(huff_t *huff, int ch, uint8_t *fout, int *offset)
{
    bloc = *offset;
    huffman_send(huff->loc[ch], 0, fout);
    *offset = bloc;
}

void __cdecl Huff_Init(huffman_t *huff)
{
    //Com_Memset((uint32_t *)huff, 0, 19476);
    Com_Memset((uint32_t *)huff, 0, sizeof(huffman_t));
    huff->compressDecompress.loc[256] = &huff->compressDecompress.nodeList[huff->compressDecompress.blocNode++];
    huff->compressDecompress.tree = huff->compressDecompress.loc[256];
    huff->compressDecompress.tree->symbol = 256;
    huff->compressDecompress.tree->weight = 0;
    huff->compressDecompress.tree->parent = 0;
    huff->compressDecompress.tree->left = 0;
    huff->compressDecompress.tree->right = 0;
}

nodetype *__cdecl Huff_initNode(huff_t *huff, int ch, int weight)
{
    nodetype *tnode; // [esp+0h] [ebp-4h]

    tnode = &huff->nodeList[huff->blocNode++];
    tnode->symbol = ch;
    tnode->weight = weight;
    tnode->left = 0;
    tnode->right = 0;
    tnode->parent = 0;
    huff->loc[ch] = tnode;
    return tnode;
}

int __cdecl nodeCmp(const void *left, const void *right)
{
    return *(uint32_t *)(*(uint32_t *)left + 12) - *(uint32_t *)(*(uint32_t *)right + 12);
}

// ---------------------------------------------------------------------------
// Deterministic ordering of the merge pool
//
// Huff_BuildFromData orders its pool of nodes by weight alone. Because qsort()
// leaves the relative order of equal elements unspecified, and msg_hData has
// ties (bytes 155 and 205 share weight 3889, bytes 228 and 231 share 4683), the
// resulting tree -- and with it every symbol's bit code -- depended on which C
// library the binary was linked against. The retail Windows client's runtime
// sorts those ties differently than glibc, which moved the codes of bytes 155
// and 205 and made a live client misread any compressed message containing
// them.
//
// The helpers below reproduce that client's ordering, so the tree is identical
// on every platform. They reimplement the sort that client's runtime performs:
// a median-of-three quicksort that recurses into the shorter side and keeps the
// longer one on an explicit stack, a selection sort for runs of eight or fewer
// elements, and -- like the original -- elements that compare equal to the
// pivot are left in place instead of being swapped across it. The ordering was
// verified against the shipped executable rather than assumed: all 256 sorts
// this function performs, and all 256 resulting symbol codes, match it exactly.
// ---------------------------------------------------------------------------

// Runs this short are finished with a selection sort.
static const int HUF_SORT_SMALL_RUN = 8;

// The pool never holds more than 256 entries, so the longer side of every
// split is pushed at most 8 times; the client's runtime reserves 30 slots.
static const int HUF_SORT_STACK = 30;

static void Huff_SwapNodes(nodetype **a, nodetype **b)
{
    nodetype *t = *a;
    *a = *b;
    *b = t;
}

// Move the largest remaining element of [lo, hi] to the end, repeatedly.
static void Huff_SortSmallRun(nodetype **lo, nodetype **hi)
{
    while (hi > lo)
    {
        nodetype **largest = lo;
        nodetype **scan;

        for (scan = lo + 1; scan <= hi; ++scan)
        {
            if ((*scan)->weight > (*largest)->weight)
                largest = scan;
        }
        Huff_SwapNodes(largest, hi);
        --hi;
    }
}

// Sort *count consecutive node pointers, in place.
static void Huff_SortNodes(nodetype **nodes, int count)
{
    nodetype **lostk[HUF_SORT_STACK];
    nodetype **histk[HUF_SORT_STACK];
    nodetype **lo;
    nodetype **hi;
    int stkptr = 0;

    if (count < 2)
        return;

    lo = nodes;
    hi = nodes + count - 1;

    for (;;)
    {
        nodetype **mid;
        nodetype **loguy;
        nodetype **higuy;

        if (hi - lo + 1 <= HUF_SORT_SMALL_RUN)
        {
            Huff_SortSmallRun(lo, hi);

            // Nothing left in this branch: take the pending range, or stop.
            if (stkptr == 0)
                return;
            --stkptr;
            lo = lostk[stkptr];
            hi = histk[stkptr];
            continue;
        }

        // Order lo, mid and hi so mid holds the median: that element becomes
        // the pivot and stays where it is for the whole partition.
        mid = lo + (hi - lo + 1) / 2;
        if ((*lo)->weight > (*mid)->weight)
            Huff_SwapNodes(lo, mid);
        if ((*lo)->weight > (*hi)->weight)
            Huff_SwapNodes(lo, hi);
        if ((*mid)->weight > (*hi)->weight)
            Huff_SwapNodes(mid, hi);

        loguy = lo;
        higuy = hi;
        for (;;)
        {
            // Advance from the left until something belongs on the right...
            if (mid > loguy)
            {
                do
                {
                    ++loguy;
                } while (loguy < mid && (*loguy)->weight <= (*mid)->weight);
            }
            if (mid <= loguy)
            {
                do
                {
                    ++loguy;
                } while (loguy <= hi && (*loguy)->weight <= (*mid)->weight);
            }
            // ...and from the right until something belongs on the left.
            do
            {
                --higuy;
            } while (higuy > mid && (*higuy)->weight > (*mid)->weight);

            if (loguy > higuy)
                break;

            Huff_SwapNodes(loguy, higuy);
            // Follow the pivot if the crossing swap moved it.
            if (mid == higuy)
                mid = loguy;
        }

        // Everything in [lo, higuy] is <= the pivot and everything from loguy
        // on is >= it. Skip the elements equal to the pivot that would
        // otherwise be carried into the shorter side; they already sit between
        // the two halves and need no further sorting.
        ++higuy;
        if (mid < higuy)
        {
            bool reachedPivot = false;

            do
            {
                --higuy;
                if (higuy <= mid)
                {
                    reachedPivot = true;
                    break;
                }
            } while ((*higuy)->weight == (*mid)->weight);

            if (!reachedPivot)
                goto split;
        }
        // Hand those pivot-equal elements that are still on the left over to
        // the right-hand side as well.
        do
        {
            --higuy;
        } while (higuy > lo && (*higuy)->weight == (*mid)->weight);

    split:
        // Keep the longer side for later and continue with the shorter one, so
        // the explicit stack never needs more than a handful of entries.
        if (higuy - lo >= hi - loguy)
        {
            if (lo < higuy)
            {
                lostk[stkptr] = lo;
                histk[stkptr] = higuy;
                ++stkptr;
            }
            if (loguy < hi)
            {
                lo = loguy;
                continue;
            }
        }
        else
        {
            if (loguy < hi)
            {
                lostk[stkptr] = loguy;
                histk[stkptr] = hi;
                ++stkptr;
            }
            if (lo < higuy)
            {
                hi = higuy;
                continue;
            }
        }

        if (stkptr == 0)
            return;
        --stkptr;
        lo = lostk[stkptr];
        hi = histk[stkptr];
    }
}

void __cdecl Huff_BuildFromData(huff_t *huff, const int *msg_hData)
{
    nodetype *inited; // eax
    nodetype *v3; // eax
    nodetype *v4; // eax
    int numNodes; // [esp+0h] [ebp-410h]
    nodetype *heap[256]; // [esp+8h] [ebp-408h] BYREF
    int i; // [esp+408h] [ebp-8h]
    int heapHead; // [esp+40Ch] [ebp-4h]

    numNodes = 256;
    heapHead = 0;
    for (i = 0; i < 256; ++i)
    {
        inited = Huff_initNode(huff, i, msg_hData[i]);
        heap[i] = inited;
    }
    Huff_SortNodes(heap, 256);
    v3 = Huff_initNode(huff, 257, 1);
    v3->left = huff->tree;
    v3->right = heap[0];
    v3->left->parent = v3;
    v3->right->parent = v3;
    v3->weight = v3->right->weight + v3->left->weight;
    heap[0] = v3;
    while (numNodes > 1)
    {
        Huff_SortNodes(&heap[heapHead], 256 - heapHead);
        v4 = Huff_initNode(huff, 257, 1);
        v4->left = heap[heapHead];
        v4->right = heap[heapHead + 1];
        v4->left->parent = v4;
        v4->right->parent = v4;
        v4->weight = v4->right->weight + v4->left->weight;
        heap[++heapHead] = v4;
        --numNodes;
    }
    huff->tree = heap[heapHead];
}

