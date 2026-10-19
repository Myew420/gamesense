#pragma once

#include <cstdint>

#include <Features/Combat/AttackCommand.h>
#include <Features/Combat/Triggerbot/Triggerbot.h>
#include <Features/Game/PanicKeyConfigVariables.h>
#include <GameClient/Bind.h>
#include <HookContext/HookContextMacros.h>
#include <Utils/VerifyConsole.h>

// combat panic key: a toggle bind. while engaged, combat/movement features
// short-circuit at the entry-point level (EntryPoints.h); visuals, glow, skin
// changer, sound and radio keep running.
// static state polled once per user command; rising edge toggles exactly once.
template <typename HookContext>
class PanicKey {
public:
    explicit PanicKey(HookContext& hookContext) noexcept
        : hookContext{hookContext}
    {
    }

    // Polled from CreateMove (every tick, game thread): updates the toggle state.
    void run() const noexcept
    {
        const int bindValue = GET_CONFIG_VAR(panic_vars::Bind);
        if (bindValue <= Bind::kOff || bindValue > Bind::kLast) {
            // an engaged panic with the key unbound is otherwise unrecoverable:
            // no pressable key = release the latch
            if (panicActive) {
                panicActive = false;
                VerifyConsole::write(0.0f, "panic", "bind removed - combat features re-enabled");
            }
            return;
        }

        const bool down = Bind::isDown(bindValue);

        // rising edge only; rebinding mid-hold causes at most one extra edge
        if (down && !lastKeyWasDown) {
            panicActive = !panicActive;
            if (panicActive)
                engage();
            VerifyConsole::write(0.0f, "panic", "%s - combat features %s",
                panicActive ? "ENGAGED" : "released",
                panicActive ? "disabled" : "enabled");
        }
        lastKeyWasDown = down;
    }

    [[nodiscard]] static bool isActive() noexcept
    {
        return panicActive;
    }

private:
    // engaging disarms cross-command state so a later release cannot fire a stale shot
    void engage() const noexcept
    {
        hookContext.template make<Triggerbot>().disarmStatics();
        hookContext.template make<AttackCommand>().reset();
    }

    inline static bool panicActive{false};
    inline static bool lastKeyWasDown{false};

    HookContext& hookContext;
};
