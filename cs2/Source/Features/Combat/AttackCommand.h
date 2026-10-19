#pragma once

#include <CS2/Classes/CCSGOInput.h>
#include <CS2/Classes/CUserCmd.h>
#include <GameClient/UserCmd.h>

template <typename HookContext>
class AttackCommand {
public:
    explicit AttackCommand(HookContext& hookContext) noexcept
        : hookContext{hookContext}
    {
    }

    // Retained for feature lifecycle callers; presses no longer share a global toggle.
    static void reset() noexcept {}

    bool press(cs2::CUserCmd* cmd) const noexcept
    {
        const UserCmd userCmd{cmd};
        if (!userCmd || !userCmd.pressButtonsBothBanks(cs2::CCSGOInput::Buttons::kAttack))
            return false;
        // attack1_start_history_index is mandatory: a sent attack without it
        // triggers the server "no attack information present" warning and
        // degrades lag comp to the fallback path. the history size read can
        // miss on the frame that ends up sent - refuse then; callers run per
        // frame and fire a frame later with a complete attack record.
        const auto historySize = userCmd.inputHistorySize();
        if (!historySize.hasValue())
            return false;
        static_cast<void>(userCmd.setAttack1StartHistoryIndex(historySize.value() - 1));
        return true;
    }

private:
    HookContext& hookContext;
};
