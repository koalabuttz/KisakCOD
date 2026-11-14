// The PS Vita's run plan: src/qcommon/run_plan.h with the Vita's setting
// names (vita_quitAfter, vita_atMap, ...). The other clients use the same
// plan with run_* names (src/client_mp/cl_main_mp.cpp).

#pragma once

#include <qcommon/run_plan.h>

namespace kisak::vita_run
{

using run_plan::kMaxCommandLength;
using run_plan::kMaxTimedCommands;
using run_plan::kMaxStepFileName;
using run_plan::kMaxTimedListLength;
using run_plan::kQuitGrace;
using run_plan::kQuitGraceFrames;
using run_plan::kSecond;
using run_plan::ParseTimedCommands;
using run_plan::JoinStepFile;
using run_plan::Plan;
using run_plan::StartText;
using run_plan::Step;
using run_plan::StepFileName;
using run_plan::TimedCommand;
using run_plan::TimedCommands;

// The plan's settings, named as the Vita's dvars.
struct Settings : run_plan::Settings
{
    Settings()
    {
        names = &run_plan::kVitaDvarNames;
    }
};

} // namespace kisak::vita_run
