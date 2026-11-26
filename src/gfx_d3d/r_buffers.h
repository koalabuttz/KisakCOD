#pragma once

#if defined(__linux__) || defined(__APPLE__) || defined(__vita__)
#include <linux/d3d9_types.h> // native Linux: D3D9 type surface only, no D3D9 API
#else
#include <d3d9.h>
#endif
#include "r_rendercmds.h"
#include "rb_backend.h"

void TRACK_r_buffers();
void *R_AllocDynamicVertexBuffer(IDirect3DVertexBuffer9 **vb, int sizeInBytes);
void *R_AllocStaticVertexBuffer(IDirect3DVertexBuffer9 **vb, int sizeInBytes);
void *R_AllocWriteOnlyVertexBuffer(IDirect3DVertexBuffer9 **vb, int sizeInBytes);
void *R_AllocDynamicIndexBuffer(IDirect3DIndexBuffer9 **ib, uint32_t sizeInBytes);
void *R_AllocStaticIndexBuffer(IDirect3DIndexBuffer9 **ib, int sizeInBytes);
void Load_VertexBuffer(IDirect3DVertexBuffer9 **vb, uint8_t *bufferData, int sizeInBytes);
#if defined(KISAK_GFX_GXM)
// The Vita: a static buffer the GPU reads where `data` is, with no GPU
// memory of its own. `data` is a fast file's copy that its zone keeps for as
// long as the buffer lives. Returns `data`, locked as R_AllocStatic*Buffer
// leaves its buffer (R_FinishStatic*Buffer unlocks it), or null when the back
// end refuses: the caller then makes an ordinary buffer.
void *R_AllocStaticVertexBufferInPlace(IDirect3DVertexBuffer9 **vb, void *data, int sizeInBytes);
void *R_AllocStaticIndexBufferInPlace(IDirect3DIndexBuffer9 **ib, void *data, int sizeInBytes);
#endif
void R_InitDynamicVertexBufferState(GfxVertexBufferState *vb, int bytes);
void R_InitDynamicIndexBufferState(GfxIndexBufferState *ib, int indexCount);
void R_InitDynamicIndices(GfxDynamicIndices *ib, int indexCount);
void R_ShutdownDynamicIndices(GfxDynamicIndices *ib);
void R_CreateDynamicBuffers();
void R_CreateParticleCloudBuffer();
void R_UnlockVertexBuffer(IDirect3DVertexBuffer9* handle);
void *R_LockVertexBuffer(IDirect3DVertexBuffer9 *handle, int offset, int bytes, int lockFlags);

void R_FinishStaticVertexBuffer(IDirect3DVertexBuffer9 *vb);

void R_DestroyDynamicBuffers();
void R_DestroyParticleCloudBuffer();
void R_FreeStaticVertexBuffer(IDirect3DVertexBuffer9 *vb);
void R_FreeStaticIndexBuffer(IDirect3DIndexBuffer9 *ib);

void *R_LockIndexBuffer(IDirect3DIndexBuffer9 *handle, int offset, int bytes, int lockFlags);
void R_UnlockIndexBuffer(IDirect3DIndexBuffer9 *handle);

void R_FinishStaticIndexBuffer(IDirect3DIndexBuffer9 *ib);

void R_CreateWorldVertexBuffer(IDirect3DVertexBuffer9 **vb, int *srcData, uint32_t sizeInBytes);

// How full the dynamic buffers get, for sizing them (r_gfx.h): the most one
// frame asked of each since the last report. r_dynamicBufferReport sets the
// seconds between log lines; 0 prints none.
void R_NoteSkinnedCacheUse(uint32_t bytes);      // a frame's total, what found no room included
void R_NoteSkinnedCacheFull();                   // a model was left out
void R_NotePreTessUse(uint32_t indices);         // a frame's total
void R_NotePreTessFull();                        // a batch was drawn unbatched
void R_NoteDynamicIndices(uint32_t indices);     // one write
void R_NoteDynamicVertices(uint32_t bytes);      // one write
void R_NoteStaticModelCacheUse(uint32_t verts);  // what the static model cache holds now
void R_NoteStaticModelCacheFull();               // a model found it full and is drawn by itself
void R_DynamicBufferFrameDone();                 // the back end, once a frame

extern GfxBuffers gfxBuf;