#pragma once

#include <Config/ConfigVariable.h>
#include <Features/Combat/TargetSelection.h>
#include "AimbotParams.h"

// per-weapon-group knobs (hitboxes, multipoint, pointscale, body aim, force
// shot, hitchance, min damage) live in RageGroupsConfigVariables.h, resolved
// per tick from the active weapon's group (RageGroups.h). old keys remain in
// ConfigSchema.h as parse-and-discard placeholders so old configs keep loading.

namespace aimbot_vars
{

CONFIG_VARIABLE(Enabled, bool, false);
CONFIG_VARIABLE_RANGE(TargetSelection, target_selection::kMode);
CONFIG_VARIABLE(TargetLock, bool, true);

CONFIG_VARIABLE(SpreadCompensation, bool, false);

CONFIG_VARIABLE(RecoilCompensation, bool, true);

CONFIG_VARIABLE(Extrapolate, bool, false);
CONFIG_VARIABLE_RANGE(ExtrapolateTicks, aimbot_params::kExtrapolateTicks);

CONFIG_VARIABLE(Backtrack, bool, false);
CONFIG_VARIABLE_RANGE(BacktrackTicks, aimbot_params::kBacktrackTicks);

CONFIG_VARIABLE(WallCheck, bool, true);
CONFIG_VARIABLE(Autowall, bool, false);

// stand down while smoked/flashed: don't shoot through a smoke volume or
// while blind.
CONFIG_VARIABLE(IgnoreSmoke, bool, true);
CONFIG_VARIABLE(IgnoreFlash, bool, true);

CONFIG_VARIABLE(SpreadCircleFov, bool, false);

CONFIG_VARIABLE(AutoStop, bool, false);

CONFIG_VARIABLE(SpreadGate, bool, false);

CONFIG_VARIABLE(SeedFallback, bool, false);

CONFIG_VARIABLE(ForceShotWait, bool, false);
CONFIG_VARIABLE_RANGE(ForceShotWaitTicks, aimbot_params::kForceShotWaitTicks);

CONFIG_VARIABLE(DuckPeek, bool, false);
CONFIG_VARIABLE(DebugMultipoints, bool, false);

}
