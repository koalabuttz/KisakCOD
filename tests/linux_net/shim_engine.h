// TEST-ONLY helpers exposing the shimmed engine state to the test driver.
#pragma once

#include <universal/q_shared.h>

// Looks up a dvar previously registered by the code under test (or by the test).
const dvar_t *Shim_DvarFind(const char *name);

// Registers (or updates) an integer dvar before the code under test runs.
void Shim_DvarSeedInt(const char *name, int value);

// Registers (or updates) a string dvar before the code under test runs.
void Shim_DvarSeedString(const char *name, const char *value);

// Registers (or updates) a boolean dvar before the code under test runs.
void Shim_DvarSeedBool(const char *name, bool value);

// Drops the engine's com_dedicated pointer, i.e. the "dedicated" dvar was never
// registered.  Registering or seeding a dvar named "dedicated" links it again.
void Shim_ClearComDedicated();

// Number of Com_Error() diagnostics emitted so far, and their reset.  This shim
// logs and returns where the real engine aborts, so a non-zero count means "the
// code refused", not "the process died": tests must assert the socket state too.
int Shim_ComErrorCount();
void Shim_ComErrorReset();
