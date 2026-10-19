#pragma once

#include <cstring>

#include <CS2/Classes/CUserCmd.h>
#include <Features/Combat/Rcs/RcsConfigVariables.h>
#include <Features/Combat/Rcs/RecoilCompensation.h>
#include <GameClient/Entities/PlayerPawn.h>
#include <HookContext/HookContextMacros.h>

template <typename HookContext>
class Rcs {
public:
    explicit Rcs(HookContext& hookContext) noexcept
        : hookContext{hookContext}
    {
    }

    void onCreateMove(cs2::CUserCmd* cmd) const noexcept
    {
        if (!GET_CONFIG_VAR(rcs_vars::Enabled) || !UserCmd{cmd})
            return;
        auto&& localPawn = hookContext.activeLocalPlayerPawn();
        if (!localPawn || localPawn.isAlive() != true)
            return;
        // snipers and shotguns always excluded, hardcoded no toggle: scoped
        // kicks fight the compensation, shotgun pellets need the raw spread.
        // weaponType: 1 pistol, 2 smg, 3 rifle, 4 shotgun, 5 sniper, 6 lmg.
        const auto weaponType = localPawn.getActiveWeapon().weaponType();
        if (localPawn.isUsingSniperRifle() || (weaponType.hasValue() && weaponType.value() == 4))
            return;
        const auto shots = localPawn.shotsFired();
        if (!shots.hasValue() || shots.value() < 1)
            return;
        const auto punch = localPawn.aimPunchAngle();
        if (!punch.hasValue() || !__builtin_isfinite(punch.value().x) || !__builtin_isfinite(punch.value().y))
            return;

        // full current punch, scaled per axis; 100 = exact compensation.
        const float pitchScale = static_cast<float>(GET_CONFIG_VAR(rcs_vars::Strength)) / 100.0f;
        const float yawScale = static_cast<float>(GET_CONFIG_VAR(rcs_vars::YawStrength)) / 100.0f;
        recoil_compensation::apply(cmd, punch.value().x * pitchScale, punch.value().y * yawScale);
    }

    HookContext& hookContext;
};