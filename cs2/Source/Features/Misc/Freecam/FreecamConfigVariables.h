#pragma once

#include <Config/ConfigVariable.h>
#include <Config/RangeConstrainedVariableParams.h>

namespace freecam_params
{

inline constexpr auto kSpeed = RangeConstrainedVariableParams<float>{.min = 5.0f, .max = 1000.0f, .def = 250.0f};

}

namespace freecam_vars
{

CONFIG_VARIABLE(Enabled, bool, false);
CONFIG_VARIABLE_RANGE(Speed, freecam_params::kSpeed);

}
