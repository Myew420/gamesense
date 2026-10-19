#pragma once

#include <Config/ConfigVariable.h>
#include <Config/RangeConstrainedVariableParams.h>

namespace watermark_vars
{

CONFIG_VARIABLE(Enabled, bool, true);



CONFIG_VARIABLE(ShowFps, bool, true);
CONFIG_VARIABLE(ShowSpeed, bool, true);
CONFIG_VARIABLE(ShowPing, bool, true);
CONFIG_VARIABLE(ShowTeamDamage, bool, true);
CONFIG_VARIABLE(ShowClock, bool, true);



// 0..4000 for free drag (drag clamps at Range::kMax); uint16 keeps the uint
// schema conversion, old uint8 configs load unchanged.
inline constexpr auto kOffsetRange = RangeConstrainedVariableParams<std::uint16_t>{.min = 0, .max = 4000, .def = 10};
CONFIG_VARIABLE_RANGE(OffsetX, kOffsetRange);
CONFIG_VARIABLE_RANGE(OffsetY, kOffsetRange);
}
