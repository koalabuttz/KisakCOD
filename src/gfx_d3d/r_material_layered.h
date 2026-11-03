#pragma once
#include "r_material.h"
Material *Material_CreateLayered(char *name, const Material **layerMtl,
    uint32_t layerCount, MaterialTechniqueSet *techSet);
