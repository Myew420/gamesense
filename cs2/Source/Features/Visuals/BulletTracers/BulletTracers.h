#pragma once

#include <cstdint>
#include <cmath>

#include <CS2/Classes/Entities/C_BaseEntity.h>
#include <CS2/Classes/IGameEventManager2.h>
#include <CS2/Classes/Vector.h>
#include <Features/Combat/Autowall/Autowall.h>
#include <Features/Visuals/BulletTracers/BulletTracersConfigVariables.h>
#include <GameClient/GameEvents/GameEventFields.h>
#include <GameClient/PlayerSlotLookup.h>
#include <GameClient/SchemaSystem/SchemaReadiness.h>
#include <GameClient/Tracing/Tracing.h>
#include <GameClient/WorldToScreen/WorldToClipSpaceConverter.h>
#include <HookContext/HookContextMacros.h>
#include <Utils/ColorUtils.h>
#include <Utils/Optional.h>
#include <Utils/Trig.h>
#include <UI/ImGui/OverlayLayer.h>

// Bullet tracers, drawn through our own overlay: a fading beam from the
// muzzle to the traced impact point of every player's shots.
// CS2 never fires the CSGO-era "bullet_impact" event (no xrefs in libclient);
// "weapon_fire" does and reaches our hook. Recipe: weapon_fire ->
// shooter's networked m_angEyeAngles -> Tracing::traceLine -> beam eye+32u
// along the ray to the endpoint.
template <typename HookContext>
class BulletTracers {
public:
    explicit BulletTracers(HookContext& hookContext) noexcept
        : hookContext{hookContext}
    {
    }

    // Event side (game thread): resolve the shot ray and record the segment.
    void onFireEventClientSide(cs2::IGameEvent* event) const noexcept
    {
        if (!GET_CONFIG_VAR(BulletTracersEnabled))
            return;
        if (!event || !game_events::is(event, "weapon_fire"))
            return;

        const auto curtime = hookContext.globalVars().curtime();
        if (!curtime.hasValue() || curtime.value() < schema_readiness::kMinMapTime)
            return;

        auto&& lookup = hookContext.template make<PlayerSlotLookup>();
        const auto shooterSlot = game_events::entityForKey(event, "userid");
        if (!lookup.isValidSlot(shooterSlot))
            return;
        auto&& pawn = lookup.pawnBySlot(shooterSlot);
        if (!pawn)
            return;

        const auto eye = pawn.eyePosition();
        const auto angles = eyeAngles(pawn);
        if (!eye.hasValue() || !angles.hasValue())
            return;

        const cs2::Vector direction = forwardVector(angles.value());
        const cs2::Vector end{
            eye.value().x + direction.x * kMaxTraceDistance,
            eye.value().y + direction.y * kMaxTraceDistance,
            eye.value().z + direction.z * kMaxTraceDistance,
        };
        // Skip the shooter so the ray starts cleanly at their eyes.
        const auto trace = Tracing::traceLine(eye.value(), end,
            static_cast<cs2::C_BaseEntity*>(pawn.baseEntity()), Autowall::kBulletMask);
        if (!trace.valid)
            return;

        // Muzzle-ish start: kMuzzleOffset units along the shot direction so
        // the beam is visible in first person.
        shots[writeIndex] = Shot{
            eye.value(),
            trace.didHit ? trace.endPos : end,
            curtime.value(),
            true,
        };
        writeIndex = (writeIndex + 1) % kMaxShots;
    }

    // Render side: re-project both ends each frame (the bolts pattern) and
    // publish the fading beams.
    void run() const noexcept
    {
        if (!GET_CONFIG_VAR(BulletTracersEnabled)) {
            if (wasActive)
                overlay_layer::publishTracerBeams(nullptr, 0);
            return;
        }
        const auto curtime = hookContext.globalVars().curtime();
        if (!curtime.hasValue() || curtime.value() < schema_readiness::kMinMapTime) {
            if (wasActive)
                overlay_layer::publishTracerBeams(nullptr, 0);
            return;
        }

        wasActive = true;
        auto&& converter = hookContext.template make<WorldToClipSpaceConverter>();
        const auto color = GET_CONFIG_VAR(BulletTracersColor);

        overlay_layer::TracerBeam beams[overlay_layer::kMaxTracerBeams];
        int published = 0;
        for (int i = 0; i < kMaxShots && published < overlay_layer::kMaxTracerBeams; ++i) {
            auto& shot = shots[(writeIndex + i) % kMaxShots];
            if (!shot.alive)
                continue;
            const float age = curtime.value() - shot.bornTime;
            if (age < 0.0f || age >= kTracerLifetime) {
                shot.alive = false;
                continue;
            }

            const float dx = shot.end.x - shot.start.x;
            const float dy = shot.end.y - shot.start.y;
            const float dz = shot.end.z - shot.start.z;
            const float len = trig::squareRoot(dx * dx + dy * dy + dz * dz);
            if (len < 1.0f)
                continue;
            const cs2::Vector start{
                shot.start.x + dx / len * kMuzzleOffset,
                shot.start.y + dy / len * kMuzzleOffset,
                shot.start.z + dz / len * kMuzzleOffset,
            };

            const auto startClip = converter.toClipSpace(start);
            const auto endClip = converter.toClipSpace(shot.end);
            if (startClip.w < 0.001f || endClip.w < 0.001f)
                continue;
            const float startX = (startClip.x / startClip.w + 1.0f) * 50.0f;
            const float startY = (1.0f - startClip.y / startClip.w) * 50.0f;
            const float endX = (endClip.x / endClip.w + 1.0f) * 50.0f;
            const float endY = (1.0f - endClip.y / endClip.w) * 50.0f;

            const float fade = 1.0f - age / kTracerLifetime;
            beams[published++] = overlay_layer::TracerBeam{
                startX, startY, endX, endY,
                (static_cast<std::uint32_t>(color.r() * fade) << 24)
                    | (static_cast<std::uint32_t>(color.g() * fade) << 16)
                    | (static_cast<std::uint32_t>(color.b() * fade) << 8)
                    | color.a(),
            };
        }
        overlay_layer::publishTracerBeams(beams, published);
    }

    void onUnload() const noexcept
    {
        for (auto& shot : shots)
            shot.alive = false;
        overlay_layer::publishTracerBeams(nullptr, 0);
    }

private:
    struct Shot {
        cs2::Vector start;
        cs2::Vector end;
        float bornTime;
        bool alive;
    };

    [[nodiscard]] Optional<cs2::Vector> eyeAngles(auto&& pawn) const noexcept
    {
        const auto offset = hookContext.schemaSystem().getFieldOffset("C_CSPlayerPawn", "m_angEyeAngles");
        if (!offset.has_value() || *offset <= 0)
            return {};
        cs2::Vector angles{};
        std::memcpy(&angles, reinterpret_cast<const std::byte*>(static_cast<cs2::C_BaseEntity*>(pawn.baseEntity())) + *offset, sizeof(angles));
        return angles;
    }

    // View angles: pitch positive DOWN, yaw counter-clockwise (same
    // convention as AntiAim's crosshairDot).
    [[nodiscard]] static cs2::Vector forwardVector(const cs2::Vector& angles) noexcept
    {
        constexpr float kDegreesToRadians = 0.017453292519943295f;
        const float pitchRad = angles.x * kDegreesToRadians;
        const float yawRad = angles.y * kDegreesToRadians;
        return cs2::Vector{
            std::cos(pitchRad) * std::cos(yawRad),
            std::cos(pitchRad) * std::sin(yawRad),
            -std::sin(pitchRad),
        };
    }

    static constexpr int kMaxShots = 16;
    static constexpr float kTracerLifetime = 0.45f;
    static constexpr float kMuzzleOffset = 32.0f;
    static constexpr float kMaxTraceDistance = 8192.0f;

    HookContext& hookContext;
    inline static Shot shots[kMaxShots]{};
    inline static int writeIndex = 0;
    inline static bool wasActive = false;
};
