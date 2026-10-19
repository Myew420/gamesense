#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <CS2/Classes/Entities/C_BaseEntity.h>
#include <CS2/Classes/Entities/CCSPlayerController.h>
#include <CS2/Classes/EntitySystem/CEntityHandle.h>
#include <Features/Hud/SpectatorList/SpectatorListParams.h>
#include <Features/Hud/SpectatorList/SpectatorSnapshot.h>
#include <GameClient/Entities/BaseEntity.h>
#include <GameClient/Entities/PlayerController.h>
#include <GameClient/Entities/PlayerPawn.h>
#include <GameClient/EntitySystem/EntitySystem.h>
#include <HookContext/HookContextMacros.h>
#include <Utils/Optional.h>
#include <Utils/VerifyConsole.h>

// Spectator detection = velocity's add_spectators recipe: dead players are
// found through their CONTROLLER (m_bPawnIsAlive == false + m_hObserverPawn),
// because a dead player's regular pawn can already be gone from the entity
// list - the controller-owned observer pawn is the entity the game itself
// keeps for the spectate camera. A spectator is whoever's observer pawn's
// m_hObserverTarget resolves to the pawn we watch (ours, or the one our own
// death cam follows).
template <typename HookContext>
class SpectatorList {
public:
    explicit SpectatorList(HookContext& hookContext) noexcept
        : hookContext{hookContext}
    {
    }

    void run() const noexcept
    {
        char names[spectator_list::kMaxNames][40]{};
        int count = 0;
        bool spectatingOthers = false;

        if (GET_CONFIG_VAR(spectator_list_params::SpectatorListEnabled))
            collectSpectators(names, count, spectatingOthers);

        spectator_list::publish(names, count, spectatingOthers);
    }

private:
    void collectSpectators(char (&names)[spectator_list::kMaxNames][40], int& count, bool& spectatingOthers) const noexcept
    {
        count = 0;
        spectatingOthers = false;

        const auto aliveOffset = hookContext.schemaSystem().getFieldOffset("CCSPlayerController", "m_bPawnIsAlive");
        const auto observerPawnOffset = hookContext.schemaSystem().getFieldOffset("CCSPlayerController", "m_hObserverPawn");
        const auto servicesOffset = hookContext.schemaSystem().getFieldOffset("C_BasePlayerPawn", "m_pObserverServices");
        const auto targetOffset = hookContext.schemaSystem().getFieldOffset("CPlayer_ObserverServices", "m_hObserverTarget");
        const auto healthOffset = hookContext.schemaSystem().getFieldOffset("C_BaseEntity", "m_iHealth");
        const auto nameOffset = hookContext.schemaSystem().getFieldOffset("CCSPlayerController", "m_iszPlayerName");
        if (!aliveOffset.has_value() || !observerPawnOffset.has_value() || !servicesOffset.has_value()
            || !targetOffset.has_value() || !healthOffset.has_value() || !nameOffset.has_value()) {
            VerifyConsole::write(30.0f, "spec", "schema offsets unresolved - spectator detection inactive");
            return;
        }

        auto&& localPawn = hookContext.activeLocalPlayerPawn();
        const cs2::C_BaseEntity* povEntity = static_cast<cs2::C_BaseEntity*>(localPawn.baseEntity());
        if (!povEntity)
            return;

        // POV: while dead we spectate someone else - list their spectators.
        int localHealth{};
        std::memcpy(&localHealth, reinterpret_cast<const std::byte*>(povEntity) + *healthOffset, sizeof(localHealth));
        if (localHealth <= 0) {
            void* localServices{};
            std::memcpy(&localServices, reinterpret_cast<const std::byte*>(povEntity) + *servicesOffset, sizeof(localServices));
            if (localServices) {
                std::uint32_t watchedHandle{};
                std::memcpy(&watchedHandle, reinterpret_cast<const std::byte*>(localServices) + *targetOffset, sizeof(watchedHandle));
                if (auto* const watched = hookContext.template make<EntitySystem>().getEntityFromHandle(cs2::CEntityHandle{watchedHandle})) {
                    if (watched != povEntity) {
                        povEntity = static_cast<cs2::C_BaseEntity*>(watched);
                        spectatingOthers = true;
                    }
                }
            }
        }

        auto&& entitySystem = hookContext.template make<EntitySystem>();
        auto&& localController = hookContext.localPlayerController();
        entitySystem.forEachNetworkableEntityIdentity([&](const auto& identity) {
            if (count >= spectator_list::kMaxNames)
                return;
            const auto entityTypeInfo = hookContext.entityClassifier().classifyEntity(identity.entityClass);
            if (!entityTypeInfo.template is<cs2::CCSPlayerController>())
                return;

            auto* const controller = static_cast<cs2::CCSPlayerController*>(identity.entity);
            auto&& wrapped = hookContext.template make<PlayerController>(controller);
            if (wrapped == localController)
                return;

            const auto* const controllerBytes = reinterpret_cast<const std::byte*>(controller);

            // Alive players never spectate.
            bool pawnIsAlive{};
            std::memcpy(&pawnIsAlive, controllerBytes + *aliveOffset, sizeof(pawnIsAlive));
            if (pawnIsAlive)
                return;

            // The observer pawn the game assigned for the spectate camera.
            std::uint32_t observerPawnHandle{};
            std::memcpy(&observerPawnHandle, controllerBytes + *observerPawnOffset, sizeof(observerPawnHandle));
            auto* const observerPawn = static_cast<const cs2::C_BaseEntity*>(
                static_cast<cs2::C_BaseEntity*>(entitySystem.getEntityFromHandle(cs2::CEntityHandle{observerPawnHandle})));
            if (!observerPawn)
                return;

            void* observerServices{};
            std::memcpy(&observerServices, reinterpret_cast<const std::byte*>(observerPawn) + *servicesOffset, sizeof(observerServices));
            if (!observerServices)
                return;

            std::uint32_t targetHandle{};
            std::memcpy(&targetHandle, reinterpret_cast<const std::byte*>(observerServices) + *targetOffset, sizeof(targetHandle));
            auto* const target = static_cast<const cs2::C_BaseEntity*>(
                static_cast<cs2::C_BaseEntity*>(entitySystem.getEntityFromHandle(cs2::CEntityHandle{targetHandle})));
            if (target != povEntity)
                return;

            const char* name = "?";
            const auto controllerName = reinterpret_cast<const char*>(controllerBytes + *nameOffset);
            if (looksLikeName(controllerName))
                name = controllerName;

            std::size_t i = 0;
            for (; name[i] != '\0' && i < sizeof(names[0]) - 1; ++i)
                names[count][i] = name[i];
            names[count][i] = '\0';
            ++count;
        });
    }

    [[nodiscard]] static bool looksLikeName(const char* name) noexcept
    {
        if (!name || name[0] == '\0')
            return false;
        for (int i = 0; i < 32 && name[i] != '\0'; ++i) {
            const auto c = static_cast<unsigned char>(name[i]);
            if (c < 0x20 && c != '\t')
                return false;
        }
        return true;
    }

    HookContext& hookContext;
};
