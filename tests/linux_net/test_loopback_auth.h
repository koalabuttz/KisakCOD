// TEST-ONLY regression coverage for the loopback local-auth test guard
// (sv_loopbackTestAuth) in src/linux/net.cpp.  See tests/linux_net/README.md.
#pragma once

// Forks one child process per start-up decision and folds the children's check
// counts into the totals below.  Must run before the parent's first NET_Init():
// the production guard captures sv_loopbackTestAuth once per process and never
// recomputes it, so a start-up decision can only be observed in a process that
// has not initialised the network layer yet.
void RunLoopbackAuthScenarioProcesses();

// The bypass-disabled checks for this process.  Called after NET_Init(); the
// caller's engine is already running, so this only proves what a process that
// did NOT enable the switch at start-up can and cannot do.
void RunLoopbackAuthDefaultChecks();

int LoopbackAuthCheckCount();
int LoopbackAuthFailureCount();
