#pragma once

#include <CS2/Classes/ViewSetup.h>
#include <cstdio>
#include <Features/Visuals/Removals/RemovalsConfigVariables.h>
#include <GameClient/ConVars/CvarSystem.h>
#include <GameClient/Entities/PlayerPawn.h>
#include <GameClient/Panorama/PanoramaUiEngine.h>
#include <HookContext/HookContextMacros.h>
#include <Hooks/SceneRenderHooks.h>














template <typename HookContext>
class Removals {
public:
    explicit Removals(HookContext& hookContext) noexcept
        : hookContext{hookContext}
    {
    }

    
    
    
    
    
    
    void overrideView(ViewSetup* viewSetup) const noexcept
    {
        if (!viewSetup || !GET_CONFIG_VAR(RemoveViewPunch))
            return;

        auto&& localPawn = hookContext.activeLocalPlayerPawn();
        if (!localPawn)
            return;

        const auto punch = localPawn.aimPunchAngle();
        if (!punch.hasValue())
            return;
        if (!looksLikeAngles(ViewSetup::viewAngles(viewSetup)) || !looksLikeAngles(punch.value()))
            return;

        auto& viewAngles = ViewSetup::viewAngles(viewSetup);
        viewAngles.x -= punch.value().x;
        viewAngles.y -= punch.value().y;
    }

    
    void run() const noexcept
    {
        updateLegsCvar();
        scene_render_hooks::setFlashOverlayRemoved(GET_CONFIG_VAR(RemoveFlashOverlay));
        updateMenuAds();
    }

    void onUnload() const noexcept
    {
        restoreLegsCvar();
        scene_render_hooks::setFlashOverlayRemoved(false);
        if (adsHidden)
            (void)hideMenuAds(false);
    }

private:
    // First-person legs removal via the game's own cl_firstperson_legs cvar
    // (the old FirstpersonLegsPrepass byte-patch is build-fragile: that
    // function is now a ~32KB inlined block). Console-queue once + direct
    // memory write; original cached on first enable, restored on the
    // disable edge.
    void updateLegsCvar() const noexcept
    {
        const bool removed = GET_CONFIG_VAR(RemoveLegs);
        if (!removed) {
            restoreLegsCvar();
            return;
        }
        if (!legsOriginalValid) {
            if (const auto original = cvarSystem().readIntConVar(kLegsCvarName); original.has_value()) {
                legsOriginal = original.value();
                legsOriginalValid = true;
            }
        }
        if (legsOriginalValid && !legsForced) {
            char command[64];
            std::snprintf(command, sizeof(command), "%s 0", kLegsCvarName);
            hookContext.template make<EngineCommandExecutor>().execute(command);
            static_cast<void>(cvarSystem().forceIntConVar(kLegsCvarName, 0));
            legsForced = true;
        }
    }

    void restoreLegsCvar() const noexcept
    {
        if (!legsForced)
            return;
        char command[64];
        std::snprintf(command, sizeof(command), "%s %d", kLegsCvarName, legsOriginal);
        hookContext.template make<EngineCommandExecutor>().execute(command);
        static_cast<void>(cvarSystem().forceIntConVar(kLegsCvarName, legsOriginal));
        legsForced = false;
    }

    [[nodiscard]] CvarSystem<HookContext> cvarSystem() const noexcept
    {
        return hookContext.template make<CvarSystem>();
    }
    void updateMenuAds() const noexcept
    {
        const bool wantHidden = GET_CONFIG_VAR(RemoveMenuAds);
        
        
        if (wantHidden == adsHidden && !wantHidden)
            return;
        ++frameCounter;
        if (wantHidden == adsHidden && frameCounter % kReissueIntervalFrames != 0)
            return;

        if (hideMenuAds(wantHidden))
            adsHidden = wantHidden;
    }

    [[nodiscard]] bool hideMenuAds(bool hidden) const noexcept
    {
        const auto mainMenu = hookContext.patternSearchResults().template get<MainMenuPanelPointer>();
        if (!mainMenu || !*mainMenu)
            return false;

        const char* const script = hidden
            ? "(function(){ var p = $.GetContextPanel().FindChildTraverse('JsLeftColumn'); if (p) p.SetHasClass('hidden', true); })();"
            : "(function(){ var p = $.GetContextPanel().FindChildTraverse('JsLeftColumn'); if (p) p.SetHasClass('hidden', false); })();";
        hookContext.template make<PanoramaUiEngine>().runScript((*mainMenu)->uiPanel, script);
        return true;
    }

    [[nodiscard]] static bool looksLikeAngles(const cs2::Vector& angles) noexcept
    {
        constexpr float kMaxPitch = 90.1f;
        constexpr float kMaxYaw = 180.1f;
        return angles.x >= -kMaxPitch && angles.x <= kMaxPitch
            && angles.y >= -kMaxYaw && angles.y <= kMaxYaw
            && angles.z >= -kMaxPitch && angles.z <= kMaxPitch;
    }

    static constexpr int kReissueIntervalFrames = 300;

    static constexpr const char* kLegsCvarName = "cl_firstperson_legs";

    inline static bool legsOriginalValid = false;
    inline static bool legsForced = false;
    inline static int legsOriginal = 1;


    inline static bool adsHidden{false};
    inline static int frameCounter{0};

    HookContext& hookContext;
};
