#pragma once

#include <cstdint>

#include <CS2/Classes/CUserCmd.h>
#include <CS2/Classes/Vector.h>
#include <Features/Combat/AimTarget.h>
#include <Features/Combat/LegitAimbot/LegitAimbotConfigVariables.h>
#include <Features/Combat/MovementFix.h>
#include <Features/Combat/ShotGeometry.h>
#include <Features/Combat/SubtickShotWriter.h>
#include <Features/Game/StrafeCommand.h>
#include <GameClient/Lagcomp.h>
#include <GameClient/Tracing/Tracing.h>
#include <GameClient/Bind.h>
#include <GameClient/UserCmd.h>
#include <HookContext/HookContextMacros.h>
#include <SDL/SdlFunctions.h>
#include <Utils/Optional.h>
#include "AimAssist.h"
#include <UI/ImGui/GUI.h>
#include <GameClient/ConVars/CvarSystem.h>
#include <Features/Combat/Aimbot/AimbotConfigVariables.h>
#include <Utils/Trig.h>







template <typename HookContext>
class LegitAimbot {
public:
    explicit LegitAimbot(HookContext& hookContext) noexcept
        : hookContext{hookContext}
    {
    }

    void onCreateMove(cs2::CUserCmd* cmd) const noexcept
    {
        staged = false;
        stagedBacktrackSimTime = 0.0f;
        stagedUserFiring = false;
        if (GUI::isMenuOpen()) return;
        if (!GET_CONFIG_VAR(legit_aimbot_vars::Enabled) || GET_CONFIG_VAR(aimbot_vars::Enabled)) {
            lastTargetHandleValue = 0; acquisition.reset();
            return;
        }

        
        if (!GET_CONFIG_VAR(legit_aimbot_vars::AlwaysOn) && !Bind::isDown(GET_CONFIG_VAR(legit_aimbot_vars::AimKey))) {
            lastTargetHandleValue = 0; acquisition.reset();
            return;
        }

        UserCmd userCmd{cmd};
        if (!userCmd) {
            lastTargetHandleValue = 0; acquisition.reset();
            return;
        }

        auto&& localPawn = hookContext.activeLocalPlayerPawn();
        if (!localPawn || localPawn.isAlive() != true) {
            lastTargetHandleValue = 0; acquisition.reset();
            return;
        }

        if (GET_CONFIG_VAR(legit_aimbot_vars::OnlyWhileFiring) && !userCmd.isButtonDown(cs2::CCSGOInput::Buttons::kAttack)) return;
        if (GET_CONFIG_VAR(legit_aimbot_vars::RequireMouseMovement) && !GUI::hasRecentPhysicalMouseMotion()) return;
        if (GET_CONFIG_VAR(legit_aimbot_vars::IgnoreFlash) && localPawn.getRemainingFlashBangTime() > 0.15f) return;
        auto aimTarget = hookContext.template make<AimTarget>();
        const auto eye = aimTarget.eyePosition(localPawn);
        const auto currentPitch = userCmd.viewPitch();
        const auto currentYaw = userCmd.viewYaw();
        if (!eye.hasValue() || !currentPitch.hasValue() || !currentYaw.hasValue()) {
            lastTargetHandleValue = 0; acquisition.reset();
            return;
        }

        
        
        
        float maxFov = static_cast<float>(GET_CONFIG_VAR(legit_aimbot_vars::Fov));
        if (GET_CONFIG_VAR(legit_aimbot_vars::SpreadCircleFov)) {
            if (const auto cone = hookContext.localPlayerBulletInaccuracy(); cone.hasValue() && cone.value() > 0.0f)
                maxFov = trig::arcTangent2(cone.value(), 1.0f) * trig::kRadiansToDegrees;
        }
        cs2::C_BaseEntity* preferredTarget = nullptr;
        if (GET_CONFIG_VAR(legit_aimbot_vars::TargetLock) && lastTargetHandleValue != 0) {
            if (auto* const instance = hookContext.template make<EntitySystem>().getEntityFromHandle(cs2::CEntityHandle{lastTargetHandleValue}))
                preferredTarget = static_cast<cs2::C_BaseEntity*>(instance);
        }
        const auto aim = aimTarget.acquire(eye.value(), currentPitch.value(), currentYaw.value(), maxFov, hitboxFlags(),
            [&](const auto& candidate) {
                return !GET_CONFIG_VAR(legit_aimbot_vars::WallCheck)
                    || Tracing::isVisible(eye.value(), candidate.aimPoint, localPawn.baseEntity(), candidate.entity);
            }, preferredTarget, static_cast<target_selection::Mode>(static_cast<std::uint8_t>(GET_CONFIG_VAR(legit_aimbot_vars::TargetSelection))));
        if (!aim.hasValue()) {
            lastTargetHandleValue = 0; acquisition.reset();
            return;
        }
        lastTargetHandleValue = hookContext.template make<BaseEntity>(aim.value().entity).handle().value;

        const float now = hookContext.globalVars().curtime().valueOr(0.0f);
        if (!acquisition.ready(lastTargetHandleValue, now,
                static_cast<float>(GET_CONFIG_VAR(legit_aimbot_vars::ReactionMs)) / 1000,
                static_cast<float>(GET_CONFIG_VAR(legit_aimbot_vars::SwitchDelayMs)) / 1000)) return;

        // Lag-record refinement (backtrack): steer toward the best record point near
        // the crosshair instead of the live pose, then stamp the shot with the
        // record's sim time in onWriteMoveCrc so the server resolves it at the tick
        // the record was captured. Constrained to the same legit FOV + WallCheck as
        // the live acquire (the ghost can never pull the aim beyond human range),
        // and the smoothing/reaction delays above still gate the walk toward it.
        float pitch = aim.value().angles.pitch, yaw = aim.value().angles.yaw;
        float backtrackSimTime = 0.0f;
        if (GET_CONFIG_VAR(legit_aimbot_vars::Backtrack)) {
            const auto flags = hitboxFlags();
            const auto record = hookContext.template make<Lagcomp>().bestRecord(aim.value().entity, eye.value(),
                currentPitch.value(), currentYaw.value(), flags.head, flags.chest, flags.stomach, flags.arms, flags.legs,
                static_cast<int>(GET_CONFIG_VAR(legit_aimbot_vars::BacktrackTicks)));
            if (record.hasValue()) {
                const auto recordAngles = shot_geometry::anglesTo(eye.value(), record.value().aimPoint);
                const float dPitch = recordAngles.pitch - currentPitch.value();
                const float dYaw = trig::normalizeDegrees(recordAngles.yaw - currentYaw.value());
                const float recordFov = trig::squareRoot(dPitch * dPitch + dYaw * dYaw);
                if (recordFov <= maxFov
                    && (!GET_CONFIG_VAR(legit_aimbot_vars::WallCheck)
                        || Tracing::isVisible(eye.value(), record.value().aimPoint, localPawn.baseEntity(), aim.value().entity))) {
                    // A shot can only be stamped with a tick the server hasn't
                    // simulated yet (the rage aimbot's own guard).
                    const auto serverTick = hookContext.localPlayerController().tickBase();
                    const int recordTick = static_cast<int>(record.value().simulationTime / Lagcomp<HookContext>::kTickInterval);
                    if (!serverTick.hasValue() || recordTick < serverTick.value())
                        backtrackSimTime = record.value().simulationTime;
                    // Visible Mouse Aim keeps the user's own mouse direction; the
                    // stamp alone makes the server resolve it at the record's tick.
                    if (!GET_CONFIG_VAR(legit_aimbot_vars::VisibleAim)) {
                        pitch = recordAngles.pitch;
                        yaw = recordAngles.yaw;
                    }
                }
            }
        }
        const bool userFiring = userCmd.isButtonDown(cs2::CCSGOInput::Buttons::kAttack);
        if (GET_CONFIG_VAR(legit_aimbot_vars::RecoilCompensation)) {
            if (const auto punch = localPawn.aimPunchAngle(); punch.hasValue()) {
                pitch -= punch.value().x; yaw -= punch.value().y;
            }
        }
        const aim_assist::Parameters parameters{
            static_cast<aim_assist::Mode>(static_cast<std::uint8_t>(GET_CONFIG_VAR(legit_aimbot_vars::Mode))),
            static_cast<float>(GET_CONFIG_VAR(legit_aimbot_vars::Smooth)),
            static_cast<float>(GET_CONFIG_VAR(legit_aimbot_vars::Strength)),
            static_cast<float>(GET_CONFIG_VAR(legit_aimbot_vars::Deadzone)),
            static_cast<float>(GET_CONFIG_VAR(legit_aimbot_vars::MaxSpeed)),
            static_cast<float>(GET_CONFIG_VAR(legit_aimbot_vars::SnapFov))};
        const auto step = aim_assist::step(pitch - currentPitch.value(), yaw - currentYaw.value(),
            std::clamp(hookContext.globalVars().frametime().valueOr(1.0f / 64), 0.0001f, 0.05f), parameters);
        if (GET_CONFIG_VAR(legit_aimbot_vars::VisibleAim)) {
            auto cvars = hookContext.template make<CvarSystem>();
            const auto sensitivity = cvars.readFloatConVar("sensitivity");
            const auto yawScale = cvars.readFloatConVar("m_yaw");
            const auto pitchScale = cvars.readFloatConVar("m_pitch");
            float fovScale = 1;
            const auto offset = hookContext.schemaSystem().getFieldOffset("C_BasePlayerPawn", "m_flFOVSensitivityAdjust");
            if (offset.has_value() && *offset > 0)
                std::memcpy(&fovScale, reinterpret_cast<const std::byte*>(static_cast<cs2::C_BaseEntity*>(localPawn.baseEntity())) + *offset, sizeof(fovScale));
            if (sensitivity && yawScale && pitchScale && std::isfinite(fovScale) && fovScale > 0
                && *sensitivity > 0 && std::abs(*yawScale) > .00001f && std::abs(*pitchScale) > .00001f)
                static_cast<void>(GUI::applyAimMotion(-step.yaw / (*sensitivity * *yawScale * fovScale),
                    step.pitch / (*sensitivity * *pitchScale * fovScale)));
            // Mouse-driven aim still gets its shot stamped on the record's tick:
            // the direction is the user's own crosshair, only the resolve tick
            // is historical.
            stagedBacktrackSimTime = backtrackSimTime;
            stagedUserFiring = userFiring;
            stagedCommand = cmd;
            stagedSequence = StrafeCommand::commandNumber(cmd);
            return;
        }
        stagedPitch = std::clamp(currentPitch.value() + step.pitch, -89.0f, 89.0f);
        stagedYaw = std::remainder(currentYaw.value() + step.yaw, 360.0f);
        stagedCommand = cmd;
        stagedSequence = StrafeCommand::commandNumber(cmd);
        staged = true;
        stagedBacktrackSimTime = backtrackSimTime;
        stagedUserFiring = userFiring;
        movement_fix::setViewAngles(userCmd, stagedPitch, stagedYaw);
    }

    void onWriteMoveCrc(cs2::CUserCmd* cmd) const noexcept
    {
        const bool enabled = GET_CONFIG_VAR(legit_aimbot_vars::Enabled) && !GET_CONFIG_VAR(aimbot_vars::Enabled);
        if (staged && enabled && cmd == stagedCommand && StrafeCommand::commandNumber(cmd) == stagedSequence)
            movement_fix::setViewAngles(UserCmd{cmd}, stagedPitch, stagedYaw);
        // Backtrack shot stamping: the user was firing this command (captured at
        // onCreateMove, before any feature presses the button for them) and a lag
        // record was staged - stamp the subtick shot with the record's sim time so
        // the server resolves it at the record's tick (SubtickShotWriter's
        // backtrack path, the rage aimbot's pattern). The cmd's own (smoothed /
        // mouse) view angles stay the direction, so the bullet only goes where the
        // crosshair visibly is.
        if (enabled && stagedBacktrackSimTime > 0.0f && stagedUserFiring && cmd && cmd == stagedCommand
            && StrafeCommand::commandNumber(cmd) == stagedSequence) {
            auto&& localPawn = hookContext.activeLocalPlayerPawn();
            if (localPawn && localPawn.isAlive() == true) {
                const UserCmd userCmd{cmd};
                const auto aimPitch = userCmd.viewPitch();
                const auto aimYaw = userCmd.viewYaw();
                if (aimPitch.hasValue() && aimYaw.hasValue()) {
                    // The writer subtracts the punch itself - hand it over only
                    // when this feature didn't already compensate the view
                    // (the triggerbot's live-shot convention).
                    float punchPitch = 0.0f, punchYaw = 0.0f;
                    if (!GET_CONFIG_VAR(legit_aimbot_vars::RecoilCompensation))
                        if (const auto punch = localPawn.aimPunchAngle(); punch.hasValue()) {
                            punchPitch = punch.value().x;
                            punchYaw = punch.value().y;
                        }
                    static_cast<void>(hookContext.template make<SubtickShotWriter>().run(cmd, localPawn,
                                                                                          aimPitch.value(), aimYaw.value(),
                                                                                          punchPitch, punchYaw,
                                                                                          stagedBacktrackSimTime, false));
                }
            }
        }
        staged = false;
        stagedBacktrackSimTime = 0.0f;
        stagedUserFiring = false;
    }

private:
    [[nodiscard]] typename AimTarget<HookContext>::HitboxFlags hitboxFlags() const noexcept
    {
        return {
            .head = GET_CONFIG_VAR(legit_aimbot_vars::HitHead),
            .chest = GET_CONFIG_VAR(legit_aimbot_vars::HitChest),
            .stomach = GET_CONFIG_VAR(legit_aimbot_vars::HitStomach),
            .arms = GET_CONFIG_VAR(legit_aimbot_vars::HitArms),
            .legs = GET_CONFIG_VAR(legit_aimbot_vars::HitLegs),
        };
    }

    
    
    
    
    

    inline static aim_assist::Acquisition acquisition;
    inline static bool staged{};
    inline static float stagedPitch{}, stagedYaw{};
    inline static cs2::CUserCmd* stagedCommand{};
    inline static std::int32_t stagedSequence{};
    inline static std::uint32_t lastTargetHandleValue{0};
    inline static float stagedBacktrackSimTime{0.0f};
    inline static bool stagedUserFiring{false};
    HookContext& hookContext;
};
