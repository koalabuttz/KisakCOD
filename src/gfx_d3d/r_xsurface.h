#pragma once
#include "r_material.h"

struct XSurface;

void XSurfaceGetVerts(const XSurface *surf, float *pVert, float *pTexCoord, float *pNormal);
int XSurfaceGetNumVerts(const XSurface *surface);
int XSurfaceGetNumTris(const XSurface *surface);

#ifdef KISAK_RADIANT
struct XModel;
// Editor-only XModel CPU geometry extraction (vert-snap + model ray-pick). See r_xsurface.cpp.
uint16_t *R_CheckTris(const XSurface *surface, void *dstIndices, uint16_t baseVert);
int Editor_ExtractXModelGeo(XModel *model, float *verts, int vertLimit,
                                    uint16_t *indices, int indexLimit);
#endif
