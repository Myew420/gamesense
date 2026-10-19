#pragma once

#include <cstdint>

#include <CS2/Classes/SceneObjectUpdaterHandle_t.h>
#include "C_EconEntity.h"

namespace cs2
{

struct C_CSWeaponBase : C_EconEntity {
    using m_iClip1 = std::int32_t;
    using sceneObjectUpdaterHandle = SceneObjectUpdaterHandle_t*;
    using GetInaccuracy = float(C_CSWeaponBase* thisptr, float* movementInaccuracy, float* airSpeedInaccuracy);
    using GetSpread = float(C_CSWeaponBase* thisptr);
    
    
    
    // ABI: bool(weapon, outSkinData*). rsi = 32-byte output struct, only written on the
    // success path with two movaps stores ([out]/[out+0x10]) so the buffer MUST be 16-byte
    // aligned; never read, no initialization needed. a null out-pointer crashes.
    using RegenerateSkin = bool(C_CSWeaponBase* thisptr, void* outSkinData);
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    using ResolveSubclassData = std::int64_t(C_CSWeaponBase* thisptr);
    using UpdateSkin = void(C_CSWeaponBase* thisptr, int changeFlag);
    using UpdateCompositeMaterialSet = void(void* destOwner, void* srcOwner, bool dispatch);
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    using UpdateWeaponData = std::int64_t(C_CSWeaponBase* thisptr, unsigned int changeType);
    
    
    
    
    
    
    
    
    using UpdateSubclass = std::int64_t(C_CSWeaponBase* thisptr, void* subclassSpawnData);
    
    
    
    
    
    
    
    using SetModel = std::int64_t(C_CSWeaponBase* thisptr, const char* modelPath);
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    using UpdateCompositeMaterial = std::int64_t(void* compositeMaterialOwner, bool dispatchOwnerRebuild);

    
    
    
    
    
    
    static constexpr int kCompositeMaterialOwnerOffset = 1936;
};

}
