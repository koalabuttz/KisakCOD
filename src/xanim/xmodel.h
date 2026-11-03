#pragma once

#include <universal/layout_assert.h>
#include <universal/com_math.h>
#include <gfx_d3d/r_material.h>
//#include <physics/phys_local.h>

#define MAX_LODS 4

enum XModelLodRampType : __int32
{                                       // ...
    XMODEL_LOD_RAMP_RIGID = 0x0,
    XMODEL_LOD_RAMP_SKINNED = 0x1,
    XMODEL_LOD_RAMP_COUNT = 0x2,
};

struct XModelLodInfo // sizeof=0x1C
{                                       // ...
    float dist;
    uint16_t numsurfs;
    uint16_t surfIndex;
    int partBits[4];
    unsigned __int8 lod;
    unsigned __int8 smcIndexPlusOne;
    unsigned __int8 smcAllocBits;
    unsigned __int8 unused;
};

struct XModelCollTri_s // sizeof=0x30
{
    float plane[4];
    float svec[4];
    float tvec[4];
};
static_assert(sizeof(XModelCollTri_s) == 48);

struct XBoneInfo // sizeof=0x28
{                                       // ...
    float bounds[2][3];
    float offset[3];
    float radiusSquared;
};
static_assert(sizeof(XBoneInfo) == 40);

struct XModelCollSurf_s // sizeof=0x2C
{
    XModelCollTri_s* collTris;
    int numCollTris;
    float mins[3];
    float maxs[3];
    int boneIdx;
    int contents;
    int surfFlags;
};

struct XModelStreamInfo // sizeof=0x0
{                                       // ...
};

struct XModel // sizeof=0xDC
{                                       // ...
    const char* name;
    unsigned __int8 numBones;
    unsigned __int8 numRootBones;
    unsigned __int8 numsurfs;
    unsigned __int8 lodRampType;
    uint16_t* boneNames;
    unsigned __int8* parentList;
    __int16* quats;
    float* trans;
    unsigned __int8* partClassification;
    DObjAnimMat* baseMat;
    struct XSurface* surfs;
    Material** materialHandles;
    XModelLodInfo lodInfo[4];
    XModelCollSurf_s* collSurfs;
    int numCollSurfs;
    int contents;
    XBoneInfo* boneInfo;
    float radius;
    float mins[3];
    float maxs[3];
    __int16 numLods;
    __int16 collLod;
    XModelStreamInfo streamInfo;
    // padding byte
    // padding byte
    // padding byte
    int memUsage;
    unsigned __int8 flags;
    bool bad;
    // padding byte
    // padding byte
    struct PhysPreset* physPreset;
    struct PhysGeomList* physGeoms;
};
static_assert(LAYOUT_32BIT(sizeof(XModel) == 220));

struct XModelPiece // sizeof=0x10
{
    XModel *model;
    float offset[3];
};
static_assert(LAYOUT_32BIT(sizeof(XModelPiece) == 16));

struct XModelPieces // sizeof=0xC
{                                       // ...
    const char *name;
    int numpieces;
    XModelPiece *pieces;
};
static_assert(LAYOUT_32BIT(sizeof(XModelPieces) == 12));

struct QueueElement // sizeof=0x8
{                                       // ...
    uint32_t beginIndex;            // ...
    uint32_t count;                 // ...
};

struct XSurfaceGetTriCandidatesLocals // sizeof=0x2A4
{                                       // ...
    int mins[3];
    int maxs[3];                        // ...
    const struct XSurfaceCollisionTree *tree;  // ...
    const uint16_t *inIndices;  // ...
    const struct GfxPackedVertex *inVertices0; // ...
    bool(*visitorFunc)(void *, const struct GfxPackedVertex **, const struct GfxPackedVertex **); // ...
    void *visitorContext;               // ...
    uint32_t nodeQueueBegin;        // ...
    uint32_t nodeQueueEnd;          // ...
    uint32_t leafQueueBegin;        // ...
    uint32_t leafQueueEnd;          // ...
    uint32_t triangleQueueBegin;    // ...
    uint32_t triangleQueueEnd;      // ...
    uint32_t vertexQueueBegin;      // ...
    uint32_t vertexQueueEnd;        // ...
    QueueElement nodeQueue[64];         // ...
    QueueElement leafQueue[4];
    QueueElement triangleQueue[4];
    uint16_t vertexQueue[4][3];
};

struct XModelSurfs // sizeof=0x14
{                                       // ...
    struct XSurface *surfs;                    // ...
    int partBits[4];                    // ...
};
static_assert(LAYOUT_32BIT(sizeof(XModelSurfs) == 20));

struct XModelConfigEntry // sizeof=0x404
{                                       // ...
    char filename[1024];                // ...
    float dist;                         // ...
};
struct XModelConfig // sizeof=0x1430
{                                       // ...
    XModelConfigEntry entries[4];       // ...
    float mins[3];
    float maxs[3];                      // ...
    int collLod;                        // ...
    unsigned __int8 flags;              // ...
    char physicsPresetFilename[1024];   // ...
    // padding byte
    // padding byte
    // padding byte
};
struct XModelPartsLoad // sizeof=0x1C
{                                       // ...
    unsigned __int8 numBones;
    unsigned __int8 numRootBones;
    // padding byte
    // padding byte
    uint16_t *boneNames;
    unsigned __int8 *parentList;
    __int16 *quats;
    float *trans;
    unsigned __int8 *partClassification;
    DObjAnimMat *baseMat;
};
static_assert(LAYOUT_32BIT(sizeof(XModelPartsLoad) == 28));

struct XModelDefault // sizeof=0x4C
{                                       // ...
    uint16_t boneNames[1];
    unsigned __int8 parentList[1];      // ...
    // padding byte
    XModelPartsLoad modelParts;         // ...
    XBoneInfo boneInfo;                 // ...
    unsigned __int8 partClassification[1]; // ...
    // padding byte
    uint16_t surfNames[1];
};

struct XVertexInfo_s // sizeof=0x40
{                                       // ...
    float normal[3];
    unsigned __int8 color[4];
    float binormal[3];
    float texCoordX;
    float tangent[3];
    float texCoordY;
    float offset[3];
    unsigned __int8 numWeights;
    unsigned __int8 pad;
    __int16 boneOffset;
};
static_assert(sizeof(XVertexInfo_s) == 64);

struct XBlendLoadInfo // sizeof=0x4
{                                       // ...
    uint16_t boneOffset;
    uint16_t boneWeight;
};
struct XVertexBuffer // sizeof=0x44
{
    XVertexInfo_s v;
    XBlendLoadInfo w[1];
};

struct XVertexInfo0 // sizeof=0x2
{                                       // ...
    uint16_t boneOffset;
};
struct XVertexInfo3 // sizeof=0xE
{
    XVertexInfo0 vert0;
    XBlendLoadInfo blend[3];
};
struct XVertexInfo2 // sizeof=0xA
{
    XVertexInfo0 vert0;
    XBlendLoadInfo blend[2];
};
struct XVertexInfo1 // sizeof=0x6
{
    XVertexInfo0 vert0;
    XBlendLoadInfo blend[1];
};

// xmodel
void XModelPartsFree(XModelPartsLoad *modelParts);
bool XModelBad(const XModel *model);
void TRACK_xmodel();
XModel *XModelPrecache(char *name, void *(*Alloc)(int), void *(*AllocColl)(int));
XModel *XModelPrecache_LoadObj(char *name, void *(*Alloc)(int), void *(*AllocColl)(int));
XModel *XModelPrecache_FastFile(const char *name);
XModel *XModelLoad(char *name, void *(*Alloc)(int), void *(*AllocColl)(int));
XModel *XModelFindExisting(const char *name);
uint16_t *XModelBoneNames(XModel *model);
void XModelDumpInfo();

double XModelGetRadius(const XModel *model);
void XModelGetBounds(const XModel *model, float *mins, float *maxs);
int XModelGetMemUsage(const XModel *model);
int XModelTraceLine(
    const XModel *model,
    struct trace_t *results,
    const float *localStart,
    const float *localEnd,
    int contentmask);
int XModelTraceLineAnimated(
    const struct DObj_s *obj,
    uint32_t modelIndex,
    int baseBoneIndex,
    trace_t *results,
    const DObjAnimMat *boneMtxList,
    float *localStart,
    float *localEnd,
    int contentmask);
void XModelTraceLineAnimatedPartBits(
    const struct DObj_s *obj,
    uint32_t modelIndex,
    int baseBoneIndex,
    int contentmask,
    int *partBits);
char XSurfaceVisitTrianglesInAabb(
    const struct XSurface *surface,
    uint32_t vertListIndex,
    const float *aabbMins,
    const float *aabbMaxs,
    bool(*visitorFunc)(void *, const struct GfxPackedVertex **, const struct GfxPackedVertex **),
    void *visitorContext);
void XSurfaceVisitTrianglesInAabb_ConvertAabb(
    const struct XSurfaceCollisionTree *tree,
    const float *aabbMins,
    const float *aabbMaxs,
    int *mins,
    int *maxs);
bool XSurfaceVisitTrianglesInAabb_ProcessVertices(XSurfaceGetTriCandidatesLocals *locals);
char XSurfaceVisitTrianglesInAabb_ProcessTriangles(XSurfaceGetTriCandidatesLocals *locals);
char XSurfaceVisitTrianglesInAabb_ProcessLeaf(XSurfaceGetTriCandidatesLocals *locals);
char XSurfaceVisitTrianglesInAabb_ProcessNode(XSurfaceGetTriCandidatesLocals *locals);
int XModelGetBoneIndex(const XModel *model, uint32_t name, uint32_t offset, unsigned __int8 *index);
int XModelGetStaticBounds(const XModel *model, mat3x3 &axis, float *mins, float *maxs);

// xmodel_utils
const char *XModelGetName(const XModel *model);
int XModelGetSurfaces(const XModel *model, struct XSurface **surfaces, int lod);
struct XSurface *XModelGetSurface(const XModel *model, int lod, int surfIndex);
const XModelLodInfo *XModelGetLodInfo(const XModel *model, int lod);
uint32_t XModelGetSurfCount(const XModel *model, int lod);
Material **XModelGetSkins(const XModel *model, int lod);
XModelLodRampType XModelGetLodRampType(const XModel *model);
int XModelGetNumLods(const XModel *model);
double XModelGetLodOutDist(const XModel *model);
int XModelNumBones(const XModel *model);
const DObjAnimMat *XModelGetBasePose(const XModel *model);
int XModelGetLodForDist(const XModel *model, float dist);
void XModelSetTestLods(uint32_t lodLevel, float dist);
double XModelGetLodDist(const XModel *model, uint32_t lod);
int XModelGetContents(const XModel *model);
int XModelGetStaticModelCacheVertCount(XModel *model, uint32_t lod);



// xmodel_load_obj
void ConsumeQuatNoSwap(unsigned __int8 **pos, __int16 *out);
int XModelSurfsPrecache(
    XModel *model,
    const char *name,
    void *(*Alloc)(int),
    __int16 modelNumsurfs,
    const char *modelName,
    XModelSurfs *outModelSurfs);


// xmodel_load_phys_collmap
struct PhysGeomList *XModel_LoadPhysicsCollMap(const char *name, void *(*Alloc)(int));