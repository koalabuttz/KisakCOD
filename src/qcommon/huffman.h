#pragma once
#include <cstdint>

struct nodetype // sizeof=0x14
{                                       // ...
    nodetype* left;
    nodetype* right;
    nodetype* parent;
    int weight;
    int symbol;
};

struct huff_t // sizeof=0x4C14
{                                       // ...
    int blocNode;
    int blocPtrs;
    nodetype* tree;                     // ...
    nodetype* loc[257];
    nodetype** freelist;
    nodetype nodeList[768];
    nodetype* nodePtrs[768];
};

struct huffman_t // sizeof=0x4C14
{                                       // ...
    huff_t compressDecompress;          // ...
};

int get_bit(const uint8_t *fin);
bool Huff_offsetReceive(nodetype *node, int *ch, const uint8_t *fin, int *offset, int maxOffset);
void huffman_send(nodetype *node, nodetype *child, uint8_t *fout);
void add_bit(char bit, uint8_t *fout);
int huffman_bitCountForNode(nodetype *node, nodetype *child);
int Huff_bitCount(huff_t *huff, uint32_t ch);
void Huff_offsetTransmit(huff_t *huff, int ch, uint8_t *fout, int *offset);
void Huff_Init(huffman_t *huff);
nodetype *Huff_initNode(huff_t *huff, int ch, int weight);
int nodeCmp(const void *left, const void *right);
void Huff_BuildFromData(huff_t *huff, const int *msg_hData);
