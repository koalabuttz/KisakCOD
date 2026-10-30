// TEST-ONLY helpers exposing the shimmed engine state to the test driver.
#pragma once

#include <universal/q_shared.h>

// Looks up a dvar previously registered by the code under test (or by the test).
const dvar_t *Shim_DvarFind(const char *name);

// Registers (or updates) an integer dvar before the code under test runs.
void Shim_DvarSeedInt(const char *name, int value);

// Registers (or updates) a string dvar before the code under test runs.
void Shim_DvarSeedString(const char *name, const char *value);
