#pragma once

// Minimal process services for the standalone engine-network diagnostic.
// Uses production dvar registration; no console commands or game host exists.
void Diagnostic_InitHost();
void Diagnostic_ShutdownHost();
