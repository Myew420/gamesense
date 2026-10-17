#pragma once

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <cstring>

#include <CS2/Classes/Entities/C_BaseEntity.h>
#include <CS2/Classes/Entities/C_CSPlayerPawn.h>
#include <CS2/Classes/CSceneObject.h>
#include <Features/Visuals/Chams/ChamsConfigVariables.h>
#include <GameClient/Entities/BaseEntity.h>
#include <GameClient/Entities/PlayerPawn.h>
#include <GameClient/EntitySystem/EntitySystem.h>
#include <HookContext/HookContextMacros.h>
#include <MemoryPatterns/PatternTypes/EntityPatternTypes.h>
#include <MemoryPatterns/PatternTypes/RenderComponentPatternTypes.h>
#include <MemoryPatterns/PatternTypes/SceneObjectPatternTypes.h>
#include <MemoryPatterns/PatternTypes/SceneObjectUpdaterPatternTypes.h>
#include <Platform/Linux/LinuxPlatformApi.h>
#include <GameClient/ClientBuildProfile.h>
#include <Utils/CrashLogger.h>

















template <typename HookContext>
class Chams {
public:
    explicit Chams(HookContext& hookContext) noexcept
        : hookContext{hookContext}
    {
    }

    // CGlowHelperSceneObjectDesc::GeneratePrimitives calls the attached mesh
    // descriptor at +0x20, returning to client +0x1907de1 on this build.
    // Preserve that nested generation while suppressing its ordinary mesh pass.
    [[nodiscard]] static bool isOutlineGeneration(std::uintptr_t returnAddress) noexcept
    {
        return hideModelsSupported() && returnAddress == CrashLogger::clientModule.base + 0x1907de1;
    }
    [[nodiscard]] static bool hideModelsSupported() noexcept
    {
        int status = outlineProfileStatus.load(std::memory_order_relaxed);
        if (!status) {
            status = client_build_profile::supported(CrashLogger::clientModule.base) ? 2 : 1;
            outlineProfileStatus.store(status, std::memory_order_relaxed);
        }
        return status == 2;
    }
    [[nodiscard]] static std::uint32_t primitiveCount(void* primitives) noexcept {
        std::uint32_t count{};
        if (primitives) std::memcpy(&count, static_cast<std::byte*>(primitives) + kPrimitiveCountOffset, sizeof(count));
        return count;
    }
    static void suppressGeneratedPrimitives(void* primitives, std::uint32_t previous) noexcept {
        if (primitives && primitiveCount(primitives) >= previous)
            std::memcpy(static_cast<std::byte*>(primitives) + kPrimitiveCountOffset, &previous, sizeof(previous));
    }

    static constexpr std::size_t kPrimitiveStride = 0x70;
    static constexpr int kMaxLivePawns = 32;
    static constexpr int kMaxSceneObjectEntries = 128;
    static constexpr int kMaxSceneObjectsPerPawn = 8;
    static constexpr int kSnapshotRefreshInterval = 64;
    static constexpr std::uint32_t kMaxOverridePrimitives = 4096;

    // Target state is double-buffered and atomically published: the DrawArray hook
    // fires on MULTIPLE render threads (live diag showed 6+ threads hitting it
    // concurrently) while the refresh rebuilds on a GeneratePrimitives thread.
    // Rebuilding in place made readers see half-cleared pawn/scene-object tables -
    // the gate matched every other frame = the 2026-10-17 flicker. One writer at a
    // time (refreshInFlight), publish by swapping the active index (release/acquire).
    struct SceneObjectEntry {
        const void* object;
        std::uint8_t pawnIndex;
        bool enemy;
    };

    struct Snapshot {
        cs2::C_BaseEntity* livePawnPointers[kMaxLivePawns]{};
        bool livePawnIsEnemy[kMaxLivePawns]{};
        int livePawnCount{0};
        cs2::C_BaseEntity* localPawnPointer{nullptr};
        SceneObjectEntry sceneObjectEntries[kMaxSceneObjectEntries]{};
        int sceneObjectEntryCount{0};
    };

    inline static Snapshot snapshots[2];
    inline static std::atomic<int> activeSnapshotIndex{0};
    inline static std::atomic<bool> refreshInFlight{false};

    [[nodiscard]] static const Snapshot& currentSnapshot() noexcept
    {
        return snapshots[activeSnapshotIndex.load(std::memory_order_acquire)];
    }

    
    
    void refreshLivePawnPointers() noexcept
    {
        bool expected = false;
        if (!refreshInFlight.compare_exchange_strong(expected, true))
            return; // another render thread is rebuilding; keep the current snapshot
        const int stagingIndex = 1 - activeSnapshotIndex.load(std::memory_order_relaxed);
        auto& snapshot = snapshots[stagingIndex];
        snapshot.livePawnCount = 0;
        snapshot.localPawnPointer = nullptr;
        snapshot.sceneObjectEntryCount = 0;
        hookContext.template make<EntitySystem>().forEachNetworkableEntityIdentity([&](const auto& identity) {
            if (snapshot.livePawnCount >= kMaxLivePawns)
                return;
            auto&& baseEntity = hookContext.template make<BaseEntity>(static_cast<cs2::C_BaseEntity*>(identity.entity));
            if (!baseEntity.classify().template is<cs2::C_CSPlayerPawn>())
                return;
            auto&& pawn = baseEntity.template as<PlayerPawn>();
            if (!pawn)
                return;
            if (pawn.isControlledByLocalPlayer()) {
                snapshot.localPawnPointer = static_cast<cs2::C_BaseEntity*>(identity.entity);
                return;
            }
            if (pawn.isAlive() != true)
                return;
            const bool enemy = pawn.isEnemy() == true;
            const bool teammate = pawn.isEnemy() == false;
            if (enemy || (teammate && GET_CONFIG_VAR(chams_vars::Teammates))) {
                snapshot.livePawnIsEnemy[snapshot.livePawnCount] = enemy;
                snapshot.livePawnPointers[snapshot.livePawnCount] = static_cast<cs2::C_BaseEntity*>(identity.entity);
                registerPawnSceneObjects(snapshot, static_cast<cs2::C_BaseEntity*>(identity.entity),
                    static_cast<std::uint8_t>(snapshot.livePawnCount), enemy);
                ++snapshot.livePawnCount;
            }
        });
        activeSnapshotIndex.store(stagingIndex, std::memory_order_release);
        refreshInFlight.store(false, std::memory_order_release);
    }

    
    
    
    
    
    
    
    
    
    
    
    // Chams targeting (2026-10-17 live round): the +0x100 chain is a REAL ownership
    // link only for CARRIED-WEAPON scene objects (the enemy knife chams prove it);
    // for PLAYER-BODY scene objects it resolves to a random pawn or null - bodies never
    // matched the gate, which is why "only the knife changed". Bodies are targeted via
    // a registry built from the ModelGlow-proven walk instead: pawn -> render component
    // -> scene-object-updater vector -> CSceneObject*, the same enumeration
    // applySpawnProtectionEffect/glow uses (live-verified: the objects from this walk
    // are exactly the ones CAnimatableSceneObjectDesc::DrawArray draws, and their
    // prim+0x20 carries a CMaterial2 - the material override DOES work on bodies).
    // Gate order: registry first (bodies), then the chain (weapons).
    void registerPawnSceneObjects(Snapshot& snapshot, cs2::C_BaseEntity* pawn, std::uint8_t pawnIndex, bool enemy) noexcept
    {
        // Every read safeRead-guarded: this runs on the render thread inside the hook
        // and must survive map transitions tearing the render components down.
        void* renderComponent = nullptr;
        if (!renderComponentOf(pawn, &renderComponent))
            return;
        if (reinterpret_cast<std::uintptr_t>(renderComponent) < 0x10000)
            return;
        void* updaters = nullptr; // address of the embedded CUtlVector
        if (!sceneObjectUpdatersOf(static_cast<cs2::CRenderComponent*>(renderComponent), &updaters))
            return;
        if (reinterpret_cast<std::uintptr_t>(updaters) < 0x10000)
            return;
        int size = 0;
        if (!LinuxPlatformApi::safeRead(updaters, &size, sizeof(size)) || size <= 0)
            return;
        if (size > kMaxSceneObjectsPerPawn)
            size = kMaxSceneObjectsPerPawn;
        void* memory = nullptr;
        if (!LinuxPlatformApi::safeRead(static_cast<std::byte*>(updaters) + sizeof(void*), &memory, sizeof(memory)))
            return;
        if (reinterpret_cast<std::uintptr_t>(memory) < 0x10000)
            return;
        for (int i = 0; i < size; ++i) {
            void* handle = nullptr;
            if (!LinuxPlatformApi::safeRead(static_cast<std::byte*>(memory) + i * sizeof(void*), &handle, sizeof(handle)))
                continue;
            if (reinterpret_cast<std::uintptr_t>(handle) < 0x10000)
                continue;
            void* sceneObject = nullptr;
            if (!sceneObjectOf(static_cast<cs2::SceneObjectUpdaterHandle_t*>(handle), &sceneObject))
                continue;
            if (reinterpret_cast<std::uintptr_t>(sceneObject) < 0x10000)
                continue;
            for (int e = 0; e < snapshot.sceneObjectEntryCount; ++e)
                if (snapshot.sceneObjectEntries[e].object == sceneObject)
                    goto next;
            if (snapshot.sceneObjectEntryCount >= kMaxSceneObjectEntries)
                return;
            snapshot.sceneObjectEntries[snapshot.sceneObjectEntryCount++] = SceneObjectEntry{sceneObject, pawnIndex, enemy};
        next:;
        }
    }

    [[nodiscard]] bool safeReadField(const void* fieldAddress, void** out) noexcept
    {
        return LinuxPlatformApi::safeRead(fieldAddress, out, sizeof(void*));
    }

    [[nodiscard]] bool renderComponentOf(cs2::C_BaseEntity* pawn, void** out) noexcept
    {
        const auto field = hookContext.patternSearchResults().template get<OffsetToRenderComponent>().of(pawn);
        return field.get() && safeReadField(field.get(), out);
    }

    // The updaters vector is EMBEDDED in the render component: OffsetToSceneObjectUpdaters
    // resolves to the address of the CUtlVector STRUCT ITSELF (rc+0x38), not a pointer
    // field - the 2026-10-17 empty-registry bug was exactly this one dereference too many
    // (diag: regEntries=0 while livePawns>0). ModelGlow's SceneObjectUpdaters reads it
    // the same way: updaters->size at +0, updaters->memory at +8.
    [[nodiscard]] bool sceneObjectUpdatersOf(cs2::CRenderComponent* renderComponent, void** updatersAddressOut) noexcept
    {
        const auto field = hookContext.patternSearchResults().template get<OffsetToSceneObjectUpdaters>().of(renderComponent);
        if (!field.get())
            return false;
        *updatersAddressOut = field.get();
        return true;
    }

    [[nodiscard]] bool sceneObjectOf(cs2::SceneObjectUpdaterHandle_t* handle, void** out) noexcept
    {
        const auto field = hookContext.patternSearchResults().template get<OffsetToSceneObject>().of(handle);
        return field.get() && safeReadField(field.get(), out);
    }

    // Own viewmodel (first-person hands + weapon) - the game's own renderable-flag
    // bit, same read the ModelGlow scene-object helpers use (SCENEOBJECTFLAG_VIEWMODEL_LAYER).
    [[nodiscard]] bool isPartOfViewmodel(const void* sceneObject) noexcept
    {
        if (!sceneObject)
            return false;
        const auto flags = hookContext.patternSearchResults()
            .template get<OffsetToSceneObjectRenderableFlags>()
            .of(static_cast<cs2::CSceneObject*>(const_cast<void*>(sceneObject))).toOptional();
        return (flags.valueOr(0) & cs2::SCENEOBJECTFLAG_VIEWMODEL_LAYER) != 0;
    }

    // The +0x100 chain - WORKS for carried-weapon scene objects, garbage for bodies.
    [[nodiscard]] static int overlayPassPawnIndex(const void* sceneObject) noexcept
    {
        if (!sceneObject)
            return -1;

        void* wrapper = nullptr;
        if (!LinuxPlatformApi::safeRead(static_cast<const std::byte*>(sceneObject) + kBodyComponentOffset, &wrapper, sizeof(wrapper)))
            return -1;
        if (reinterpret_cast<std::uintptr_t>(wrapper) < 0x10000)
            return -1;

        void* node = nullptr;
        if (!LinuxPlatformApi::safeRead(static_cast<std::byte*>(wrapper) - kSkeletonInstanceSlotDelta, &node, sizeof(node)))
            return -1;
        if (reinterpret_cast<std::uintptr_t>(node) < 0x10000)
            return -1;

        void* owner = nullptr;
        if (!LinuxPlatformApi::safeRead(static_cast<std::byte*>(node) + kNodeOwnerOffset, &owner, sizeof(owner)))
            return -1;
        const auto& snapshot = currentSnapshot();
        for (int i = 0; i < snapshot.livePawnCount; ++i) {
            if (static_cast<const void*>(snapshot.livePawnPointers[i]) == owner)
                return i;
        }
        return -1;
    }

    [[nodiscard]] static bool wantsOverlayPass(const void* sceneObject) noexcept
    {
        if (sceneObjectRegistryIndex(sceneObject) >= 0)
            return true;
        return overlayPassPawnIndex(sceneObject) >= 0;
    }

    [[nodiscard]] static bool overlayPassIsEnemy(const void* sceneObject) noexcept
    {
        const int registryIndex = sceneObjectRegistryIndex(sceneObject);
        if (registryIndex >= 0)
            return sceneObjectRegistryEntryIsEnemy(registryIndex);
        const int pawnIndex = overlayPassPawnIndex(sceneObject);
        return livePawnIsEnemyAtIndex(pawnIndex);
    }

    [[nodiscard]] static int sceneObjectRegistryIndex(const void* sceneObject) noexcept
    {
        const auto& snapshot = currentSnapshot();
        for (int i = 0; i < snapshot.sceneObjectEntryCount; ++i) {
            if (snapshot.sceneObjectEntries[i].object == sceneObject)
                return i;
        }
        return -1;
    }

    inline static std::atomic<int> outlineProfileStatus{0};

    
    
    static void clearGeneratedLayersFlag(void* sceneObject) noexcept
    {
        if (!sceneObject)
            return;
        auto* const flag = static_cast<std::byte*>(sceneObject) + kGeneratedLayersFlagOffset;
        std::uint32_t value{};
        std::memcpy(&value, flag, sizeof(value));
        value &= ~kGeneratedLayersBit;
        std::memcpy(flag, &value, sizeof(value));
    }

    
    
    
    
    static void applyOverlay(void* primitives, std::uint32_t prevCount, std::uint32_t colorPacked) noexcept
    {
        if (!primitives)
            return;

        void* elements = nullptr;
        std::memcpy(&elements, primitives, sizeof(elements));
        std::uint32_t count = 0;
        std::memcpy(&count, static_cast<std::byte*>(primitives) + kPrimitiveCountOffset, sizeof(count));
        if (!elements || count <= prevCount)
            return;

        for (std::uint32_t i = prevCount; i < count; ++i) {
            auto* const prim = static_cast<std::byte*>(elements) + static_cast<std::size_t>(i) * kPrimitiveStride;
            std::memcpy(prim + kPrimitiveColorOffset, &colorPacked, sizeof(colorPacked));
        }
    }

    
    
    
    
    // Primitive color at +0x50 is stored little-endian R,G,B,A BYTES (the game's own
    // packing fn assembles c0 | c1<<8 | c2<<16 | c3<<24 from a float[4]; the Windows ref
    // struct is {u8 r,g,b,a}). color::Rgba packs 0xRRGGBBAA, which is byte-REVERSED -
    // writing it raw made saturated colors shift channels and picker-grays go
    // semi-transparent (2026-10-17 in-game report). Always convert through here.
    [[nodiscard]] static std::uint32_t primitiveColor(color::Rgba rgba) noexcept
    {
        return (std::uint32_t{rgba.a()} << 24) | (std::uint32_t{rgba.b()} << 16)
            | (std::uint32_t{rgba.g()} << 8) | std::uint32_t{rgba.r()};
    }

    // Draw-side (DrawArray) helpers. In DrawArray the primitives argument is the RAW
    // 0x70-stride array base with the count as a separate argument (unlike the
    // GeneratePrimitives buffer struct). The game re-binds prim+0x20 to the entity's
    // own material between generation and the draw, so the material swap has to
    // happen here, right before the original DrawArray runs.
    [[nodiscard]] static const void* sceneObjectOfFirstPrimitive(const void* primitives) noexcept
    {
        if (!primitives)
            return nullptr;
        const void* sceneObject = nullptr;
        LinuxPlatformApi::safeRead(static_cast<const std::byte*>(primitives) + kPrimitiveSceneObjectOffset,
            &sceneObject, sizeof(sceneObject));
        return sceneObject;
    }

    static void applyMaterialOverrideRange(void* primitives, std::uint32_t primitiveCount,
                                           const void* material, std::uint32_t colorPacked) noexcept
    {
        if (!primitives || !material || primitiveCount == 0)
            return;
        if (primitiveCount > kMaxOverridePrimitives)
            primitiveCount = kMaxOverridePrimitives;

        for (std::uint32_t i = 0; i < primitiveCount; ++i) {
            auto* const prim = static_cast<std::byte*>(primitives) + static_cast<std::size_t>(i) * kPrimitiveStride;
            std::memcpy(prim + kPrimitiveMaterialOffset, &material, sizeof(material));
            std::memcpy(prim + kPrimitiveMaterial2Offset, &material, sizeof(material));
            std::memcpy(prim + kPrimitiveColorOffset, &colorPacked, sizeof(colorPacked));
        }
    }

    // Real-material override for the chams primitive ranges. The Linux scene primitive
    // (stride 0x70, verified from GeneratePrimitives/DrawArray disasm on 14191) carries
    // the material at +0x20 and +0x28 (the game writes the same material pointer into
    // both, the Windows ref calls them material/material2) and the paint-vertex-color
    // tint at +0x50. The occluded material's vmat flags (F_IGNOREZ/F_DISABLE_Z_*) handle
    // through-wall drawing by themselves, so the primitive depth bits are left alone.
    // The span cap mirrors the Windows reference's kMaxPrimitivesPerPass guard: a model
    // emitting more primitives than any sane mesh means the buffer state is not what we
    // think - skip rather than write.
    static void applyMaterialOverride(void* primitives, std::uint32_t prevCount, std::uint32_t newCount,
                                      const void* material, std::uint32_t colorPacked) noexcept
    {
        if (!primitives || !material || newCount <= prevCount)
            return;
        if (newCount - prevCount > kMaxOverridePrimitives)
            return;

        void* elements = nullptr;
        std::memcpy(&elements, primitives, sizeof(elements));
        if (!elements)
            return;

        for (std::uint32_t i = prevCount; i < newCount; ++i) {
            auto* const prim = static_cast<std::byte*>(elements) + static_cast<std::size_t>(i) * kPrimitiveStride;
            std::memcpy(prim + kPrimitiveMaterialOffset, &material, sizeof(material));
            std::memcpy(prim + kPrimitiveMaterial2Offset, &material, sizeof(material));
            std::memcpy(prim + kPrimitiveColorOffset, &colorPacked, sizeof(colorPacked));
        }
    }

private:
    static constexpr std::ptrdiff_t kBodyComponentOffset = 0x100;   
    static constexpr std::ptrdiff_t kSkeletonInstanceSlotDelta = 0x100; 
    static constexpr std::ptrdiff_t kNodeOwnerOffset = 0x30;        
    static constexpr std::ptrdiff_t kGeneratedLayersFlagOffset = 0x78;
    static constexpr std::uint32_t kGeneratedLayersBit = 1u << 3;
    static constexpr std::size_t kPrimitiveCountOffset = 0xC;
    static constexpr std::size_t kPrimitiveColorOffset = 0x50;
    static constexpr std::size_t kPrimitiveDepthOffset = 0x5E;
    static constexpr std::size_t kPrimitiveFlagsOffset = 0x62;
    static constexpr std::size_t kPrimitiveMaterialOffset = 0x20;
    static constexpr std::size_t kPrimitiveMaterial2Offset = 0x28;
    static constexpr std::size_t kPrimitiveSceneObjectOffset = 0x18;
    
    
    static constexpr std::uint16_t kPrimitiveNoDepthBits = 0x0020;



public:
    [[nodiscard]] static int livePawnCountForDiagnostics() noexcept { return currentSnapshot().livePawnCount; }
    [[nodiscard]] static int sceneObjectEntryCountForDiagnostics() noexcept { return currentSnapshot().sceneObjectEntryCount; }
    [[nodiscard]] static bool sceneObjectRegistryEntryIsEnemy(int index) noexcept
    {
        const auto& snapshot = currentSnapshot();
        return index >= 0 && index < snapshot.sceneObjectEntryCount && snapshot.sceneObjectEntries[index].enemy;
    }
    [[nodiscard]] static bool livePawnIsEnemyAtIndex(int index) noexcept
    {
        const auto& snapshot = currentSnapshot();
        return index >= 0 && index < snapshot.livePawnCount && snapshot.livePawnIsEnemy[index];
    }
    
    
    
    [[nodiscard]] bool ownsLocalPawn(const void* sceneObject) noexcept
    {
        if (!sceneObject)
            return false;

        void* wrapper = nullptr;
        if (!LinuxPlatformApi::safeRead(static_cast<const std::byte*>(sceneObject) + kBodyComponentOffset, &wrapper, sizeof(wrapper)))
            return false;
        if (reinterpret_cast<std::uintptr_t>(wrapper) < 0x10000)
            return false;

        void* node = nullptr;
        if (!LinuxPlatformApi::safeRead(static_cast<std::byte*>(wrapper) - kSkeletonInstanceSlotDelta, &node, sizeof(node)))
            return false;
        if (reinterpret_cast<std::uintptr_t>(node) < 0x10000)
            return false;

        void* owner = nullptr;
        if (!LinuxPlatformApi::safeRead(static_cast<std::byte*>(node) + kNodeOwnerOffset, &owner, sizeof(owner)))
            return false;
        const auto* localPawn = currentSnapshot().localPawnPointer;
        return localPawn && static_cast<const void*>(owner) == static_cast<const void*>(localPawn);
    }
    [[nodiscard]] static const void* livePawnPointerForDiagnostics(int index) noexcept
    {
        const auto& snapshot = currentSnapshot();
        return (index >= 0 && index < snapshot.livePawnCount) ? static_cast<const void*>(snapshot.livePawnPointers[index]) : nullptr;
    }
    [[nodiscard]] static bool isLivePawnPointer(const void* candidate) noexcept
    {
        if (!candidate)
            return false;
        const auto& snapshot = currentSnapshot();
        for (int i = 0; i < snapshot.livePawnCount; ++i) {
            if (static_cast<const void*>(snapshot.livePawnPointers[i]) == candidate)
                return true;
        }
        return false;
    }

private:

protected:
    HookContext& hookContext;
};
