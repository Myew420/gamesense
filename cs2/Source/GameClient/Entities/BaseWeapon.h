#pragma once

#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <CS2/Classes/CAttributeList.h>
#include <CS2/Classes/CEconItemAttribute.h>
#include <CS2/Classes/CModelState.h>
#include <CS2/Classes/Entities/C_CSWeaponBase.h>
#include <CS2/Classes/CCSWeaponBaseVData.h>
#include <CS2/Classes/Vector.h>
#include <MemoryPatterns/PatternTypes/WeaponPatternTypes.h>
#include <MemoryPatterns/PatternTypes/WeaponVDataPatternTypes.h>
#include <Utils/RetAddrSpoofer.h>
#include "BaseEntity.h"
#include "EntityClassifier.h"

template <typename HookContext>
class BaseWeapon {
public:
    BaseWeapon(HookContext& hookContext, cs2::C_CSWeaponBase* baseWeapon) noexcept
        : hookContext{hookContext}
        , baseWeapon{baseWeapon}
    {
    }

    using RawType = cs2::C_CSWeaponBase;

    template <template <typename...> typename EntityType>
    [[nodiscard]] bool is() const noexcept
    {
        return baseEntity().template is<EntityType>();
    }

    template <template <typename...> typename EntityType>
    [[nodiscard]] decltype(auto) cast() const noexcept
    {
        return baseEntity().template cast<EntityType>();
    }

    [[nodiscard]] decltype(auto) baseEntity() const noexcept
    {
        return hookContext.template make<BaseEntity>(baseWeapon);
    }

    [[nodiscard]] bool isSniperRifle() const noexcept
    {
        switch (baseEntity().classify().typeIndex) {
        case EntityTypeInfo::indexOf<cs2::C_WeaponSSG08>():
        case EntityTypeInfo::indexOf<cs2::C_WeaponAWP>():
        case EntityTypeInfo::indexOf<cs2::C_WeaponG3SG1>():
        case EntityTypeInfo::indexOf<cs2::C_WeaponSCAR20>(): return true;
        default: return false;
        }
    }

    [[nodiscard]] auto bulletInaccuracy() const noexcept
    {
        return inaccuracy() + spread();
    }

    [[nodiscard]] auto getName() const noexcept
    {
        const auto vData = static_cast<cs2::CCSWeaponBaseVData*>(hookContext.template make<BaseEntity>(baseWeapon).vData().valueOr(nullptr));
        return hookContext.patternSearchResults().template get<OffsetToWeaponName>().of(vData).valueOr(nullptr);
    }
    [[nodiscard]] Optional<bool> isReadyToFire(int tick) const noexcept
    {
        if (!baseWeapon || tick <= 0) return {};
        auto&& schema = hookContext.schemaSystem();
        const auto nextOffset = schema.getFieldOffset("C_BasePlayerWeapon", "m_nNextPrimaryAttackTick");
        const auto fractionOffset = schema.getFieldOffset("C_BasePlayerWeapon", "m_flNextPrimaryAttackTickRatio");
        const auto reloadOffset = schema.getFieldOffset("C_CSWeaponBase", "m_bInReload");
        if (!nextOffset.has_value() || *nextOffset<=0 || !fractionOffset.has_value() || *fractionOffset<=0
            || !reloadOffset.has_value() || *reloadOffset<=0) return {};
        const auto* bytes = reinterpret_cast<const std::byte*>(baseWeapon);
        int next{}; float fraction{}; bool reload{};
        std::memcpy(&next, bytes+*nextOffset, sizeof(next));
        std::memcpy(&fraction, bytes+*fractionOffset, sizeof(fraction));
        std::memcpy(&reload, bytes+*reloadOffset, sizeof(reload));
        if (!std::isfinite(fraction) || fraction<0 || fraction>1) return {};
        return !reload && (next<tick || (next==tick && fraction==0));
    }


    [[nodiscard]] auto clipAmmo() const noexcept
    {
        return hookContext.patternSearchResults().template get<OffsetToClipAmmo>().of(baseWeapon).toOptional();
    }

    [[nodiscard]] auto paintKit() const noexcept
    {
        return hookContext.econEntityOffsets().fallbackPaintKit.of(baseWeapon).toOptional();
    }

    void setPaintKit(int paintKit) const noexcept
    {
        hookContext.econEntityOffsets().fallbackPaintKit.of(baseWeapon) = paintKit;
    }

    [[nodiscard]] auto seed() const noexcept
    {
        return hookContext.econEntityOffsets().fallbackSeed.of(baseWeapon).toOptional();
    }

    void setSeed(int seed) const noexcept
    {
        hookContext.econEntityOffsets().fallbackSeed.of(baseWeapon) = seed;
    }

    [[nodiscard]] auto wear() const noexcept
    {
        return hookContext.econEntityOffsets().fallbackWear.of(baseWeapon).toOptional();
    }

    void setWear(float wear) const noexcept
    {
        hookContext.econEntityOffsets().fallbackWear.of(baseWeapon) = wear;
    }

    [[nodiscard]] auto statTrak() const noexcept
    {
        return hookContext.econEntityOffsets().fallbackStatTrak.of(baseWeapon).toOptional();
    }

    void setStatTrak(int statTrak) const noexcept
    {
        hookContext.econEntityOffsets().fallbackStatTrak.of(baseWeapon) = statTrak;
    }

    // currently-equipped item definition index; same offset chain as setSkin().
    [[nodiscard]] auto itemDefinitionIndex() const noexcept
    {
        const auto& offsets = hookContext.econItemAttributeOffsets();
        const auto attributeContainer = offsets.attributeManager.of(baseWeapon).get();
        const auto item = offsets.item.of(attributeContainer).get();
        return offsets.itemDefinitionIndex.of(item).toOptional();
    }

    // raw write to the same schema-resolved m_iItemDefinitionIndex field the getter reads;
    // bypasses SetAttributeValueByName bookkeeping entirely, so riskier than the attribute path.
    void setItemDefinitionIndex(int defIndex) const noexcept
    {
        const auto& offsets = hookContext.econItemAttributeOffsets();
        const auto attributeContainer = offsets.attributeManager.of(baseWeapon).get();
        const auto item = offsets.item.of(attributeContainer).get();
        if (!item)
            return;
        offsets.itemDefinitionIndex.of(item) = defIndex;
    }

    // reference sets quality 3 (QUALITY_UNUSUAL) for knives, 0 for guns. setSkin() writes 0
    // to the same field, so call AFTER setSkin() or it is overwritten.
    void setEntityQuality(int quality) const noexcept
    {
        const auto& offsets = hookContext.econItemAttributeOffsets();
        const auto attributeContainer = offsets.attributeManager.of(baseWeapon).get();
        const auto item = offsets.item.of(attributeContainer).get();
        if (!item)
            return;
        offsets.entityQuality.of(item) = quality;
    }

    // raw write to C_BaseEntity::m_nSubclassID (CUtlStringToken hash, uint32).
    // reference sets this then calls UpdateSubclass().
    void setSubclassID(std::uint32_t hash) const noexcept
    {
        hookContext.entitySubclassOffsets().subclassID.of(baseWeapon) = hash;
    }

    // reads back the field setSubclassID() writes; lets the skin changer skip re-applying.
    [[nodiscard]] auto subclassID() const noexcept
    {
        return hookContext.entitySubclassOffsets().subclassID.of(baseWeapon).toOptional();
    }

    // refreshes the entity's cached subclass data; call after setSubclassID().
    void resolveSubclassData() const noexcept
    {
        if (const auto resolve = hookContext.patternSearchResults().template get<PointerToResolveSubclassData>(); resolve && baseWeapon)
            resolve(baseWeapon);
    }

    // re-derives and applies the weapon's currently-desired model regardless of changeType.
    // changeType=1 also destroys a stale hudmodel sub-entity (after a subclass swap);
    // changeType=0 skips that but still runs the model-refresh check.
    void updateWeaponData(unsigned int changeType) const noexcept
    {
        if (const auto update = hookContext.patternSearchResults().template get<PointerToUpdateWeaponData>(); update && baseWeapon)
            update(baseWeapon, changeType);
    }

    // the real UpdateSubclass(); nullptr spawn-data arg is safe because the setSubclassID()
    // write already skips the path that would dereference it (hash computation).
    void updateSubclass() const noexcept
    {
        if (const auto update = hookContext.patternSearchResults().template get<PointerToUpdateSubclass>(); update && baseWeapon) {
            // a mis-anchored pointer can land on 0xCC padding: executing it SIGTRAPs.
            // skip the call instead.
            if (*static_cast<const unsigned char*>(update.rawAddress()) == 0xCC)
                return;
            update(baseWeapon, nullptr);
        }
    }

    // writes CModelState::m_MeshGroupMask (1 = default, 2 = legacy paint kit) via the
    // schema-resolved chain weapon -> m_CBodyComponent -> m_skeletonInstance -> m_modelState.
    // deliberately no vtable call: the old vtable+112 slot was reshuffled by the 5GB update
    // and the write through it clobbered the animgraph registry (rip=2 crashes).
    void setMeshGroupMask(std::uint64_t mask) const noexcept
    {
        const auto& offsets = hookContext.modelStateOffsets();
        const auto bodyComponent = offsets.bodyComponent.of(baseWeapon).valueOr(nullptr);
        if (!bodyComponent)
            return;

        // weapon body component is a CBodyComponentSkeletonInstance.
        const auto skeletonInstance = offsets.skeletonInstance.of(reinterpret_cast<cs2::CBodyComponentSkeletonInstance*>(bodyComponent)).get();
        if (!skeletonInstance)
            return;
        const auto modelState = offsets.modelState.of(skeletonInstance).get();
        if (!modelState)
            return;
        offsets.meshGroupMask.of(modelState) = mask;
    }

    // reads CModelState::m_hModel (u64 resource handle, +0xA0) via the same chain as
    // setMeshGroupMask(); lets callers skip SetModel when the model is already set
    // (every SetModel rebuilds model state, a race window against the anim workers).
    [[nodiscard]] std::uint64_t modelHandle() const noexcept
    {
        const auto& offsets = hookContext.modelStateOffsets();
        const auto bodyComponent = offsets.bodyComponent.of(baseWeapon).valueOr(nullptr);
        if (!bodyComponent)
            return 0;

        // same schema chain as setMeshGroupMask (old vtable+112 slot no longer stable).
        const auto skeletonInstance = offsets.skeletonInstance.of(reinterpret_cast<cs2::CBodyComponentSkeletonInstance*>(bodyComponent)).get();
        if (!skeletonInstance)
            return 0;
        const auto modelState = offsets.modelState.of(skeletonInstance).get();
        if (!modelState)
            return 0;

        std::uint64_t handle = 0;
        return offsets.modelHandle.of(modelState).valueOr(handle);
    }

    // resolves the equipped paint-kit definition via the game's own getter on the item view,
    // then reads its use_legacy_model bool at offset 174. call AFTER setSkin() - it reads
    // whatever paint kit is currently attached to the item.
    [[nodiscard]] bool isEquippedPaintKitLegacy() const noexcept
    {
        const auto& offsets = hookContext.econItemAttributeOffsets();
        const auto attributeContainer = offsets.attributeManager.of(baseWeapon).get();
        const auto item = offsets.item.of(attributeContainer).get();
        if (!item)
            return false;

        const auto getPaintKitDefinition = hookContext.patternSearchResults().template get<PointerToGetPaintKitDefinition>();
        if (!getPaintKitDefinition)
            return false;

        const auto paintKitDefinition = getPaintKitDefinition(item);
        if (!paintKitDefinition)
            return false;

        return *(reinterpret_cast<const unsigned char*>(paintKitDefinition) + 174) != 0;
    }

    // candidate SetModel().
    void setModel(const char* modelPath) const noexcept
    {
        if (const auto setModelFn = hookContext.patternSearchResults().template get<PointerToSetModel>(); setModelFn && baseWeapon && modelPath)
            setModelFn(baseWeapon, modelPath);
    }

    // attribute writes go through the game's own C_EconItemView::SetAttributeValueByName, not
    // a raw m_Attributes array write (raw write skips bookkeeping/cache invalidation and is the
    // leading suspect for a delayed near-null crash in the scene system's LOD selection).
    //
    // split from regenerateSkin() so callers can set the mesh group mask between the two:
    // composite material rebuild must happen AFTER setMeshGroupMask(), or legacy-only submeshes
    // get rebuilt against the default mesh group and end up with no bound material.
    bool applySkinAttributes(int paintKit, int seed, float wear) const noexcept
    {
        const auto& offsets = hookContext.econItemAttributeOffsets();
        const auto attributeContainer = offsets.attributeManager.of(baseWeapon).get();
        const auto item = offsets.item.of(attributeContainer).get();

        const auto setAttribute = hookContext.patternSearchResults().template get<PointerToSetAttributeValueByName>();
        if (!setAttribute || !item)
            return false;

        // ItemIDHigh = 0xFFFFFFFF (no real SOC item; makes the resolver promote the
        // weapon-fallback paint/seed/wear into view attributes), plus AccountID, DisallowSOC,
        // RestoreCustomMaterialAfterPrecache. do not stamp ItemIDLow/m_bInitialized/
        // EntityQuality or a faux item id here - the resolver would chase it instead of the
        // fallbacks and the HUD row state gets polluted.
        offsets.itemIDHigh.of(item) = 0xFFFFFFFFu;
        offsets.accountID.of(item) = offsets.originalOwnerXuidLow.of(baseWeapon).valueOr(0u);
        offsets.disallowSOC.of(item) = true;
        offsets.restoreCustomMaterialAfterPrecache.of(item) = true;

        setAttribute(item, "set item texture prefab", static_cast<float>(paintKit));
        setAttribute(item, "set item texture wear", wear);
        setAttribute(item, "set item texture seed", static_cast<float>(seed));

        // write the real paint kit/wear/seed into the fallback fields, not -1 (matches the
        // reference; -1 only matched an older revision).
        hookContext.econEntityOffsets().fallbackPaintKit.of(baseWeapon) = paintKit;
        hookContext.econEntityOffsets().fallbackSeed.of(baseWeapon) = seed;
        hookContext.econEntityOffsets().fallbackWear.of(baseWeapon) = wear;
        return true;
    }

    // the real UpdateCompositeMaterial: drains the embedded CCompositeMaterialOwner
    // (pending jobs owner+0x2b8/+0x2c0, refcounted handles owner+0x2a0/+0x2a8); the bool
    // only gates a trailing deferred owner->vt[4] dispatch.
    // call order: drain BEFORE stamping the item view, dispatch off - running the pending
    // jobs after the stamp wipes it (HUD skin name disappears).
    void updateCompositeMaterial(bool dispatchOwnerRebuild = false) const noexcept
    {
        const auto update = hookContext.patternSearchResults().template get<PointerToUpdateCompositeMaterial>();
        if (!update || !baseWeapon)
            return;

        update(reinterpret_cast<unsigned char*>(baseWeapon) + cs2::C_CSWeaponBase::kCompositeMaterialOwnerOffset, dispatchOwnerRebuild);
    }

    // the real UpdateSkin: resets the CACHED skin attribution block (weapon+0x2910..0x2928,
    // cached paint kit -> -1). the mesh path reads this cache, not the item view - without
    // the reset skins render HUD-only (HUD reads the view, model reads the cache).
    void updateSkin(int changeFlag) const noexcept
    {
        if (const auto update = hookContext.patternSearchResults().template get<PointerToUpdateSkin>(); update && baseWeapon)
            update(baseWeapon, changeFlag);
    }

    // the real UpdateCompositeMaterialSet: merges a source composite-material set into the
    // dest owner (dest must already hold entries). port of the reference's self-merge
    // (dispatch off) right after UpdateCompositeMaterial.
    void updateCompositeMaterialSet() const noexcept
    {
        const auto updateSet = hookContext.patternSearchResults().template get<PointerToUpdateCompositeMaterialSet>();
        if (!updateSet || !baseWeapon)
            return;
        auto* const weapon = reinterpret_cast<unsigned char*>(baseWeapon);
        const auto owner = weapon + cs2::C_CSWeaponBase::kCompositeMaterialOwnerOffset;
        updateSet(owner, owner, false);
    }

    // read int + write int+1 on C_CSWeaponBase.m_nCustomEconReloadEventId; the bump makes
    // the render side treat the rebuild as an econ reload event and rebind, otherwise the
    // scene object keeps the stock materials.
    void bumpEconReloadEventId() const noexcept
    {
        if (!baseWeapon)
            return;
        const auto offset = hookContext.schemaSystem().getFieldOffset("C_CSWeaponBase", "m_nCustomEconReloadEventId");
        if (!offset.has_value() || *offset <= 0)
            return;
        auto* const eventId = reinterpret_cast<int32_t*>(reinterpret_cast<unsigned char*>(baseWeapon) + *offset);
        *eventId = *eventId + 1;
    }

    // composite material rebuild (RegenerateWeaponSkin). call AFTER applySkinAttributes(),
    // setMeshGroupMask() and updateCompositeMaterial().
    //
    // returns false when skipped: RegenerateWeaponSkin unconditionally reads
    // *(m_pSubclassVData + 1312) (m_pSubclassVData = offset 1272) near its start, null on a
    // freshly spawned weapon -> segfault. callers must not mark the weapon applied on false
    // so the next frame retries.
    bool regenerateSkin() const noexcept
    {
        if (!baseEntity().vData().valueOr(nullptr))
            return false;

        // ABI: (weapon, outSkinData*) - out is a 32-byte output buffer written with movaps
        // (never read); must be 16-byte aligned. a null out-pointer crashes (movaps [r12]).
        alignas(16) std::array<std::byte, 32> regenOut{};
        if (const auto regenerate = hookContext.patternSearchResults().template get<PointerToRegenerateWeaponSkin>(); regenerate && baseWeapon)
            regenerate(baseWeapon, regenOut.data());

        return true;
    }

    [[nodiscard]] auto getSceneObjectUpdater() const noexcept
    {
        return reinterpret_cast<std::uint64_t(*)(cs2::C_CSWeaponBase*, void*, bool)>(sceneObjectUpdaterHandle() ? sceneObjectUpdaterHandle()->updaterFunction : nullptr);
    }

    void setSceneObjectUpdater(auto x) const noexcept
    {
        if (sceneObjectUpdaterHandle())
            sceneObjectUpdaterHandle()->updaterFunction = reinterpret_cast<std::uint64_t(*)(void*, void*, bool)>(x);
    }

    // forces accuracy penalty and recoil index up to the current tick (CS2 decays both
    // lazily). call before reading inaccuracy()/spread()/recoilIndex() or you get stale
    // values. idempotent within a tick.
    void updateAccuracyPenalty() const noexcept
    {
        if (const auto updateFn = hookContext.patternSearchResults().template get<PointerToUpdateAccuracyPenaltyFunction>(); updateFn && baseWeapon)
            updateFn(baseWeapon);
    }

    // live per-shot inaccuracy and base spread via the game's own GetInaccuracy/GetSpread
    // (matches what FireBullet uses). SpreadSolver needs them separately; call
    // updateAccuracyPenalty() first for a non-stale read.
    [[nodiscard]] Optional<float> inaccuracy() const noexcept
    {
        const auto getInaccuracyFn = hookContext.patternSearchResults().template get<PointerToGetInaccuracyFunction>();
        if (baseWeapon && getInaccuracyFn)
            return getInaccuracyFn(baseWeapon, nullptr, nullptr);
        return {};
    }

    // inaccuracy the weapon would have if `pawn` moved at `velocity` (velocity-cs2's
    // get_inaccuracy_at_velocity). backs up the weapon's accuracy-state block and the pawn's
    // velocity/eflags, writes the hypothetical velocity, clears EFL_DIRTY_ABSVELOCITY
    // (0x1000 in m_iEFlags, else the engine recomputes and ignores the write), runs
    // UpdateAccuracyPenalty + GetInaccuracy, restores everything. {} = cannot predict.
    [[nodiscard]] Optional<float> inaccuracyAtVelocity(cs2::C_BaseEntity* pawn, const cs2::Vector& velocity) const noexcept
    {
        const auto getInaccuracyFn = hookContext.patternSearchResults().template get<PointerToGetInaccuracyFunction>();
        const auto updateFn = hookContext.patternSearchResults().template get<PointerToUpdateAccuracyPenaltyFunction>();
        if (!baseWeapon || !pawn || !getInaccuracyFn || !updateFn)
            return {};

        auto&& schema = hookContext.schemaSystem();
        // accuracy state spans m_flTurningInaccuracyDelta..m_flRecoilIndex (velocity
        // hardcodes 40 bytes); resolved via schema so layout shifts are handled.
        const auto turningDeltaOffset = schema.getFieldOffset("C_CSWeaponBase", "m_flTurningInaccuracyDelta");
        const auto recoilIndexOffset = schema.getFieldOffset("C_CSWeaponBase", "m_flRecoilIndex");
        const auto velocityOffset = schema.getFieldOffset("C_BaseEntity", "m_vecAbsVelocity");
        const auto eflagsOffset = schema.getFieldOffset("C_BaseEntity", "m_iEFlags");
        if (!turningDeltaOffset.has_value() || !recoilIndexOffset.has_value() || !velocityOffset.has_value() || !eflagsOffset.has_value())
            return {};
        if (*turningDeltaOffset <= 0 || *recoilIndexOffset < *turningDeltaOffset || *velocityOffset <= 0 || *eflagsOffset <= 0)
            return {};

        const std::size_t accuracyStateSize = static_cast<std::size_t>(*recoilIndexOffset - *turningDeltaOffset) + sizeof(float);
        if (accuracyStateSize > kMaxAccuracyStateSize)
            return {};

        auto* const weaponBytes = reinterpret_cast<std::byte*>(baseWeapon);
        auto* const pawnBytes = reinterpret_cast<std::byte*>(pawn);

        std::byte backup[kMaxAccuracyStateSize];
        std::memcpy(backup, weaponBytes + *turningDeltaOffset, accuracyStateSize);

        cs2::Vector oldVelocity{};
        std::memcpy(&oldVelocity, pawnBytes + *velocityOffset, sizeof(oldVelocity));
        std::uint32_t oldEflags{};
        std::memcpy(&oldEflags, pawnBytes + *eflagsOffset, sizeof(oldEflags));

        const std::uint32_t newEflags = oldEflags & ~0x1000u;
        std::memcpy(pawnBytes + *eflagsOffset, &newEflags, sizeof(newEflags));
        std::memcpy(pawnBytes + *velocityOffset, &velocity, sizeof(velocity));

        updateFn(baseWeapon);
        const float predicted = getInaccuracyFn(baseWeapon, nullptr, nullptr);

        std::memcpy(pawnBytes + *velocityOffset, &oldVelocity, sizeof(oldVelocity));
        std::memcpy(pawnBytes + *eflagsOffset, &oldEflags, sizeof(oldEflags));
        std::memcpy(weaponBytes + *turningDeltaOffset, backup, accuracyStateSize);

        return predicted;
    }

    [[nodiscard]] Optional<float> spread() const noexcept
    {
        const auto getSpreadFn = hookContext.patternSearchResults().template get<PointerToGetSpreadFunction>();
        if (baseWeapon && getSpreadFn)
            return getSpreadFn(baseWeapon);
        return {};
    }

    // pellets per shot (VData m_nNumBullets, 0x730; old 0x738 is stale after a layout shift).
    // the cone generator advances the RNG once per bullet. {} if VData is unresolved.
    [[nodiscard]] Optional<int> numBullets() const noexcept
    {
        const auto vData = baseEntity().vData().valueOr(nullptr);
        if (!vData)
            return {};
        int value{};
        std::memcpy(&value, reinterpret_cast<const std::byte*>(vData) + kNumBulletsOffset, sizeof(value));
        return value;
    }

    // A float field on this weapon's VData (CCSWeaponBaseVData), by schema name. {} if VData is not
    // resolved yet or the field name did not resolve. Used for the per-weapon accuracy constants
    // (m_flMaxSpeed, m_flInaccuracyJumpApex) the max-accuracy fire gate needs.
    [[nodiscard]] Optional<float> vDataFloat(const char* fieldName) const noexcept
    {
        const auto vData = baseEntity().vData().valueOr(nullptr);
        if (!vData)
            return {};
        const auto offset = hookContext.schemaSystem().getFieldOffset("CCSWeaponBaseVData", fieldName);
        if (!offset.has_value() || *offset <= 0)
            return {};
        float value{};
        std::memcpy(&value, reinterpret_cast<const std::byte*>(vData) + *offset, sizeof(value));
        return value;
    }

    // One value of a per-firing-mode VData field (CFiringModeFloat: two floats, [0] primary and
    // [1] secondary - the scoped mode on sniper rifles), e.g. m_flInaccuracyStand.
    [[nodiscard]] Optional<float> vDataFloatAt(const char* fieldName, int mode) const noexcept
    {
        const auto vData = baseEntity().vData().valueOr(nullptr);
        if (!vData || mode < 0 || mode > 1)
            return {};
        const auto offset = hookContext.schemaSystem().getFieldOffset("CCSWeaponBaseVData", fieldName);
        if (!offset.has_value() || *offset <= 0)
            return {};
        float value{};
        std::memcpy(&value, reinterpret_cast<const std::byte*>(vData) + *offset
            + static_cast<std::ptrdiff_t>(mode) * static_cast<std::ptrdiff_t>(sizeof(float)), sizeof(value));
        return value;
    }

    // Settled means within 10% of the weapon's inaccuracy floor (the penalty decays toward it
    // exponentially and never quite reaches it).
    static constexpr float kSettledInaccuracyTolerance = 1.10f;

    // Base run speed for this weapon (CCSWeaponBaseVData::m_flMaxSpeed). The ground max-accuracy gate is
    // speed <= maxSpeed * 0.34.
    [[nodiscard]] Optional<float> maxSpeed() const noexcept
    {
        return vDataFloat("m_flMaxSpeed");
    }

    // Speed multiplier while attacking with this weapon (CCSWeaponBaseVData::m_flAttackMovespeedFactor,
    // schema-confirmed at vdata +0x7E4; 1.0 for most guns, < 1 for heavies like the Negev/AUG). The
    // game's movement setup multiplies the max speed by this while IN_ATTACK is held.
    [[nodiscard]] Optional<float> attackMovespeedFactor() const noexcept
    {
        return vDataFloat("m_flAttackMovespeedFactor");
    }

    // The in-air inaccuracy floor at jump apex (CCSWeaponBaseVData::m_flInaccuracyJumpApex). The air
    // max-accuracy gate is inaccuracy <= accuracyPenalty + this.
    [[nodiscard]] Optional<float> inaccuracyJumpApex() const noexcept
    {
        return vDataFloat("m_flInaccuracyJumpApex");
    }

    // weapon type enum (VData m_WeaponType, resolved by schema name). knife=0 pistol=1 smg=2
    // rifle=3 shotgun=4 sniper=5 lmg=6 c4=7 taser=8 grenade=9 equipment=10 healthshot=11.
    [[nodiscard]] Optional<int> weaponType() const noexcept
    {
        const auto vData = baseEntity().vData().valueOr(nullptr);
        if (!vData)
            return {};
        const auto offset = hookContext.schemaSystem().getFieldOffset("CCSWeaponBaseVData", "m_WeaponType");
        if (!offset.has_value() || *offset <= 0)
            return {};
        int value{};
        std::memcpy(&value, reinterpret_cast<const std::byte*>(vData) + *offset, sizeof(value));
        return value;
    }

    // Bullet damage inputs from VData, for the min-damage estimate (velocity's penetration::prepare):
    // base damage (m_nDamage, an int), the per-500u range falloff multiplier (m_flRangeModifier), the
    // weapon's armor penetration ratio (m_flArmorRatio) and the headshot multiplier (m_flHeadshotMultiplier).
    [[nodiscard]] Optional<float> baseDamage() const noexcept
    {
        const auto vData = baseEntity().vData().valueOr(nullptr);
        if (!vData)
            return {};
        const auto offset = hookContext.schemaSystem().getFieldOffset("CCSWeaponBaseVData", "m_nDamage");
        if (!offset.has_value() || *offset <= 0)
            return {};
        int value{};
        std::memcpy(&value, reinterpret_cast<const std::byte*>(vData) + *offset, sizeof(value));
        return static_cast<float>(value);
    }

    [[nodiscard]] Optional<float> rangeModifier() const noexcept
    {
        return vDataFloat("m_flRangeModifier");
    }
    [[nodiscard]] Optional<float> maxRange() const noexcept
    {
        return vDataFloat("m_flRange");
    }

    // Penetration power (CCSWeaponBaseVData::m_flPenetration) - the autowall loss formula's
    // "penetrationPower" input (higher = less damage lost per wall layer; rifles ~1-3, awp high).
    [[nodiscard]] Optional<float> penetrationPower() const noexcept
    {
        return vDataFloat("m_flPenetration");
    }

    [[nodiscard]] Optional<float> armorRatio() const noexcept
    {
        return vDataFloat("m_flArmorRatio");
    }

    [[nodiscard]] Optional<float> headshotMultiplier() const noexcept
    {
        return vDataFloat("m_flHeadshotMultiplier");
    }

    // The weapon's live accumulated accuracy penalty (C_CSWeaponBase::m_fAccuracyPenalty). Call
    // updateAccuracyPenalty() first for a current read.
    [[nodiscard]] Optional<float> accuracyPenalty() const noexcept
    {
        if (!baseWeapon)
            return {};
        const auto offset = hookContext.schemaSystem().getFieldOffset("C_CSWeaponBase", "m_fAccuracyPenalty");
        if (!offset.has_value() || *offset <= 0)
            return {};
        float value{};
        std::memcpy(&value, reinterpret_cast<const std::byte*>(baseWeapon) + *offset, sizeof(value));
        return value;
    }

    // velocity-cs2's is_max_accuracy: would a shot right now be at minimum inaccuracy?
    // ground: snipers need scope + still/ducked, others speed <= maxSpeed*0.34; air:
    // inaccuracy decayed to the jump-apex floor. false = cannot confirm.
    [[nodiscard]] bool isMaxAccuracy(float inaccuracy, bool onGround, bool isDucking, bool isScoped, float speed2d) const noexcept
    {
        if (onGround) {
            if (isSniperRifle()) {
                if (!isScoped)
                    return false;

                if (!isDucking && speed2d > 0.1f)
                    return false;
                // floor-rounding test (floor(x*170)/170 < x) is true for nearly
                // every value; compare against the VData scoped stand/crouch
                // floor instead. unknown floor = cannot confirm.
                const auto floor = vDataFloatAt(isDucking ? "m_flInaccuracyCrouch" : "m_flInaccuracyStand", 1);
                if (!floor.hasValue() || !(floor.value() > 0.0f))
                    return false;
                return inaccuracy <= floor.value() * kSettledInaccuracyTolerance;
            }

            const auto weaponMaxSpeed = maxSpeed();
            if (!weaponMaxSpeed.hasValue())
                return false;
            return speed2d <= weaponMaxSpeed.value() * 0.34f;
        }

        const auto jumpApex = inaccuracyJumpApex();
        const auto penalty = accuracyPenalty();
        if (!jumpApex.hasValue() || !penalty.hasValue())
            return false;
        constexpr float tolerance = 0.001f;
        return inaccuracy <= penalty.value() + jumpApex.value() + tolerance;
    }

    // current recoil index (m_flRecoilIndex, 0x28B8; old 0x2690 is stale after a layout
    // shift). the cone generator factors it in; pass the live value.
    [[nodiscard]] Optional<float> recoilIndex() const noexcept
    {
        if (!baseWeapon)
            return {};
        float value{};
        std::memcpy(&value, reinterpret_cast<const std::byte*>(baseWeapon) + kRecoilIndexOffset, sizeof(value));
        return value;
    }

private:
    // m_nNumBullets is on CCSWeaponBaseVData; m_flRecoilIndex is on C_CSWeaponBase.
    static constexpr std::ptrdiff_t kNumBulletsOffset = 0x730;
    static constexpr std::ptrdiff_t kRecoilIndexOffset = 0x28B8;

    // upper bound for the accuracy-state backup; real span is 40 bytes, actual size is
    // recomputed from schema each call and bounds-checked.
    static constexpr std::size_t kMaxAccuracyStateSize = 64;

    [[nodiscard]] auto sceneObjectUpdaterHandle() const noexcept
    {
        return hookContext.patternSearchResults().template get<OffsetToWeaponSceneObjectUpdaterHandle>().of(baseWeapon).valueOr(nullptr);
    }

    HookContext& hookContext;
    cs2::C_CSWeaponBase* baseWeapon;
};
