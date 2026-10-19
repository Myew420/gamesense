#pragma once

#include <cstdint>
#include <Config/RangeConstrainedVariableParams.h>

namespace rcs_params
{







// pitch (Y) and yaw (X) split: full pitch+yaw returns the crosshair to the
// pre-recoil aim point. defaults 100 = exact compensation (old 200
// over-compensated 2x).
constexpr auto kStrength = RangeConstrainedVariableParams<std::uint8_t>{.min = 0, .max = 255, .def = 100};
constexpr auto kYawStrength = RangeConstrainedVariableParams<std::uint8_t>{.min = 0, .max = 255, .def = 100};

}
