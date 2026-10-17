#pragma once

#include <Config/ConfigVariable.h>
#include <Config/RangeConstrainedVariableParams.h>
#include <Utils/ColorUtils.h>

#include <Features/Visuals/Chams/ChamsMaterials.h>

namespace chams_vars
{



CONFIG_VARIABLE(Enabled, bool, false);
CONFIG_VARIABLE(HideEnemies, bool, false);
CONFIG_VARIABLE(HideLocalPlayer, bool, false);

CONFIG_VARIABLE_RANGE(MaterialType, chams_materials::kMaterialTypeParams);

CONFIG_VARIABLE(EnemyColor, color::Rgba, (color::Rgba{255, 40, 40, 255}));
CONFIG_VARIABLE(EnemyOccludedColor, color::Rgba, (color::Rgba{255, 128, 0, 180}));
CONFIG_VARIABLE(DrawOccluded, bool, true);

CONFIG_VARIABLE(Teammates, bool, false);
CONFIG_VARIABLE(TeamColor, color::Rgba, (color::Rgba{64, 160, 255, 255}));
CONFIG_VARIABLE(TeamOccludedColor, color::Rgba, (color::Rgba{80, 200, 255, 180}));

CONFIG_VARIABLE(ViewmodelEnabled, bool, false);
CONFIG_VARIABLE_RANGE(ViewmodelMaterialType, chams_materials::kMaterialTypeParams);
CONFIG_VARIABLE(ViewmodelColor, color::Rgba, (color::Rgba{140, 200, 255, 255}));

CONFIG_VARIABLE(OwnWeaponsEnabled, bool, false);
CONFIG_VARIABLE_RANGE(OwnWeaponsMaterialType, chams_materials::kMaterialTypeParams);
CONFIG_VARIABLE(OwnWeaponsColor, color::Rgba, (color::Rgba{140, 255, 200, 255}));

}
