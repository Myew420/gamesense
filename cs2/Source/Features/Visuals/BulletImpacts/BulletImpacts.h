#pragma once

#include <cstdint>
#include <cmath>

#include <CS2/Classes/Entities/C_BaseEntity.h>
#include <CS2/Classes/IGameEventManager2.h>
#include <CS2/Classes/Vector.h>
#include <Features/Combat/Autowall/Autowall.h>
#include <Features/Visuals/BulletImpacts/BulletImpactsConfigVariables.h>
#include <GameClient/GameEvents/GameEventFields.h>
#include <GameClient/SchemaSystem/SchemaReadiness.h>
#include <GameClient/Tracing/Tracing.h>
#include <GameClient/WorldToScreen/WorldToClipSpaceConverter.h>
#include <GameClient/PlayerSlotLookup.h>
#include <HookContext/HookContextMacros.h>
#include <Utils/ColorUtils.h>
#include <Utils/Optional.h>
#include <UI/ImGui/OverlayLayer.h>

// Bullet impact markers, the sv_showimpacts equivalent. CS2 never fires the
// CSGO-era "bullet_impact" game event (no standalone string / code xrefs in
// libclient); "weapon_fire" has real FireEvent xrefs and reaches our hook.
// Recipe: on every "weapon_fire" take the shooter's networked m_angEyeAngles,
// trace the shot ray with Tracing::traceLine and mark the endpoint.
template <typename HookContext>
class BulletImpacts {
public:
    explicit BulletImpacts(HookContext& hookContext) noexcept
        : hookContext{hookContext}
    {
    }

    void onFireEventClientSide(cs2::IGameEvent* event) const noexcept
    {
        if (!GET_CONFIG_VAR(bullet_impacts_vars::Enabled))
            return;
        if (!event || !game_events::is(event, "weapon_fire"))
            return;
        if (!traceShot(game_events::entityForKey(event, "userid")))
            return;
    }

    // Re-projects every live marker each frame into the ImpactMarkers
    // channel; age fades toward zero alpha.
    void run() const noexcept
    {
        if (!GET_CONFIG_VAR(bullet_impacts_vars::Enabled)) {
            if (wasActive)
                overlay_layer::publishImpactMarkers(nullptr, 0);
            return;
        }
        const auto curtime = hookContext.globalVars().curtime();
        if (!curtime.hasValue() || curtime.value() < schema_readiness::kMinMapTime) {
            if (wasActive)
                overlay_layer::publishImpactMarkers(nullptr, 0);
            return;
        }

        wasActive = true;
        auto&& converter = hookContext.template make<WorldToClipSpaceConverter>();
        const auto color = GET_CONFIG_VAR(bullet_impacts_vars::Color);
        const float lifetime = static_cast<float>(GET_CONFIG_VAR(bullet_impacts_vars::Lifetime)) / 100.0f;

        overlay_layer::ImpactMarker markers[overlay_layer::kMaxImpactMarkers];
        int published = 0;
        for (int i = 0; i < kMaxImpacts && published < overlay_layer::kMaxImpactMarkers; ++i) {
            auto& impact = impacts[(writeIndex + i) % kMaxImpacts];
            if (!impact.alive)
                continue;
            const float age = curtime.value() - impact.bornTime;
            if (age < 0.0f || age >= lifetime) {
                impact.alive = false;
                continue;
            }
            const auto clip = converter.toClipSpace(impact.position);
            if (clip.w < 0.001f)
                continue;
            const float xPercent = (clip.x / clip.w + 1.0f) * 50.0f;
            // Same convention as the bolts producer: 0 = top of screen.
            const float yPercent = (1.0f - clip.y / clip.w) * 50.0f;
            if (xPercent < 0.0f || xPercent > 100.0f || yPercent < 0.0f || yPercent > 100.0f)
                continue;
            const float fade = 1.0f - age / lifetime;
            markers[published++] = overlay_layer::ImpactMarker{
                xPercent, yPercent,
                (static_cast<std::uint32_t>(color.r() * fade) << 24)
                    | (static_cast<std::uint32_t>(color.g() * fade) << 16)
                    | (static_cast<std::uint32_t>(color.b() * fade) << 8)
                    | color.a(),
            };
        }
        overlay_layer::publishImpactMarkers(markers, published);
    }

    void onUnload() const noexcept
    {
        for (auto& impact : impacts)
            impact.alive = false;
        overlay_layer::publishImpactMarkers(nullptr, 0);
    }

private:
    struct Impact {
        cs2::Vector position;
        float bornTime;
        bool alive;
    };

    // Resolves the shot ray for the firing player and records its world
    // endpoint. Returns false when anything in the chain is unavailable.
    [[nodiscard]] bool traceShot(std::int64_t shooterSlot) const noexcept
    {
        const auto curtime = hookContext.globalVars().curtime();
        if (!curtime.hasValue() || curtime.value() < schema_readiness::kMinMapTime)
            return false;

        auto&& lookup = hookContext.template make<PlayerSlotLookup>();
        if (!lookup.isValidSlot(shooterSlot))
            return false;
        auto&& pawn = lookup.pawnBySlot(shooterSlot);
        if (!pawn)
            return false;

        const auto eye = pawn.eyePosition();
        const auto angles = eyeAngles(pawn);
        if (!eye.hasValue() || !angles.hasValue())
            return false;

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
            return false;

        impacts[writeIndex] = Impact{trace.endPos, curtime.value(), true};
        writeIndex = (writeIndex + 1) % kMaxImpacts;
        return true;
    }

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

    static constexpr int kMaxImpacts = 64;
    static constexpr float kMaxTraceDistance = 8192.0f;

    inline static Impact impacts[kMaxImpacts]{};
    inline static int writeIndex = 0;
    inline static bool wasActive = false;

    HookContext& hookContext;
};
