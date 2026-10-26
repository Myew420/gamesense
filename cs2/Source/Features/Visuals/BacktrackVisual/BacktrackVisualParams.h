#pragma once

#include <cstdint>
#include <Config/RangeConstrainedVariableParams.h>

namespace backtrack_visual_params
{

// How to color the records: the age gradient (newest bright -> oldest dark)
// or one fixed user color (age still fades the alpha).
constexpr auto kColorMode = RangeConstrainedVariableParams<std::uint8_t>{.min = 0, .max = 1, .def = 0};

}
