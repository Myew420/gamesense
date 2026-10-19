#pragma once

#include <Config/ConfigVariable.h>

namespace grenade_aim_vars
{

// aim at the recorded angles of the closest saved throw with the same
// grenade on the current map while the pin is pulled
CONFIG_VARIABLE(Enabled, bool, false);

}