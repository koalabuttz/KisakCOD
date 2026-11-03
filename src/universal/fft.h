#pragma once
#include <gfx_d3d/r_material.h>


void FFT_Init(int *fftBitswap, complex_s *fftTrigTable);
void FFT(complex_s *data_inout, uint32_t log2_count, int *bitSwap, complex_s *trigTable);