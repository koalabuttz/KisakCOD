// TEST-ONLY stand-in for src/server_mp/server_mp.h.
//
// src/linux/net.cpp asks the server one question - SV_UnverifiedGuidsEnabled(),
// the frozen sv_allowUnverifiedGuids answer - so the start-up conflict check in
// NET_InitLoopbackAuthTest() can refuse to run with both trust-widening
// switches on.  Pulling the real server_mp.h into this harness would drag the
// whole client/game/xanim/ode include chain in behind it, so this header
// declares only that one entry point and tests/linux_net/shim_engine.cpp
// implements it.
//
// The production build compiles the real declaration; this file exists only
// because the shim directory precedes src/ on this harness's include path.
#pragma once

bool SV_UnverifiedGuidsEnabled();
