#pragma once

#include <cstdint>
#include <cstring>

#include <CS2/Constants/DllNames.h>
#include <MemorySearch/TypeinfoVtableResolver.h>
#include <Platform/DynamicLibrary.h>
#include <Utils/StatusReport.h>














namespace chams_hook
{


void onGeneratePrimitives(void* desc, void* sceneObject, void* sceneView, void* primitives) noexcept;
void onSceneObjectDrawArray(void* desc, void* renderContext, void* primitives, unsigned primitiveCount,
    void* sceneView, void* sceneLayer, void* perFrameStats) noexcept;

inline constexpr int kTargetCount = 5;
inline constexpr const char* kClassNames[kTargetCount] = {
    "CBaseSceneObjectDesc",
    "CMeshBuilderSceneObjectDesc",
    "CInstancedMeshSceneObjectDesc",
    "CAggregateSceneObjectDesc",
    "CAnimatableSceneObjectDesc",
};
inline constexpr std::size_t kGeneratePrimitivesSlotOffset = 0x20;

inline VTableSlotPatch patches[kTargetCount];
inline std::uintptr_t addressPoints[kTargetCount]{};

// DrawArray (vtable slot 1) patches for the ENEMY/TEAM chams passes. The material
// write has to happen HERE, not in GeneratePrimitives: the game re-binds prim+0x20
// back to the entity's own material between generation and the draw (2026-10-17
// live gdb: at DrawArray the body prim carried the game's skin material despite our
// post-GeneratePrimitives override; the +0x50 color survived on weapons only, which
// is why "only the weapons got colors"). Both public chams references do it this
// way - the xsip Windows chams hook the Render/draw call, FrameworkCS2 hooks this
// exact DrawArray and swaps MeshDrawPrimitive.material right before the trampoline.
// Slot 1 of CBaseSceneObjectDesc + CAggregateSceneObjectDesc belongs to the world
// recolor hooks (scene_render_hooks) - we patch only the other three descs.
inline constexpr int kDrawArrayTargetCount = 3;
inline constexpr int kDrawArrayTargetIndices[kDrawArrayTargetCount] = {1, 2, 4}; // MeshBuilder, InstancedMesh, Animatable
inline constexpr std::size_t kDrawArraySlotOffset = sizeof(void*);
inline VTableSlotPatch drawArrayPatches[kDrawArrayTargetCount];
inline std::uintptr_t drawArrayAddressPoints[kDrawArrayTargetCount]{};

[[nodiscard]] inline std::uintptr_t originalFor(const void* desc) noexcept
{
    if (!desc)
        return 0;
    std::uintptr_t vptr = 0;
    std::memcpy(&vptr, desc, sizeof(vptr));
    for (int i = 0; i < kTargetCount; ++i) {
        if (addressPoints[i] == vptr)
            return patches[i].original();
    }
    return 0;
}


[[nodiscard]] inline int classIndexFor(const void* desc) noexcept
{
    if (!desc)
        return -1;
    std::uintptr_t vptr = 0;
    std::memcpy(&vptr, desc, sizeof(vptr));
    for (int i = 0; i < kTargetCount; ++i) {
        if (addressPoints[i] == vptr)
            return i;
    }
    return -1;
}

[[nodiscard]] inline std::uintptr_t originalDrawArrayFor(const void* desc) noexcept
{
    if (!desc)
        return 0;
    std::uintptr_t vptr = 0;
    std::memcpy(&vptr, desc, sizeof(vptr));
    for (int i = 0; i < kDrawArrayTargetCount; ++i) {
        if (drawArrayAddressPoints[i] == vptr)
            return drawArrayPatches[i].original();
    }
    return 0;
}

inline void uninstall() noexcept
{
    for (int i = 0; i < kTargetCount; ++i)
        patches[i].restore();
    for (int i = 0; i < kDrawArrayTargetCount; ++i)
        drawArrayPatches[i].restore();
}

[[nodiscard]] inline bool install() noexcept
{
    const DynamicLibrary scenesystemLibrary{cs2::SCENESYSTEM_DLL};
    const auto params = scenesystemLibrary.getVmtFinderParams();
    const auto text = scenesystemLibrary.getCodeSection();
    bool allResolved = true;

    for (int i = 0; i < kTargetCount; ++i) {
        const auto vtable = typeinfo_vtable::findPrimaryVtable(params.rodataSection, params.dataRelRoSection, text, kClassNames[i]);
        if (!vtable) {
            StatusReport::record("Chams desc vtable not resolved (pass-through inactive)", false);
            allResolved = false;
            continue;
        }
        addressPoints[i] = vtable->addressPoint;
        if (patches[i].install(vtable->addressPoint + kGeneratePrimitivesSlotOffset, reinterpret_cast<std::uintptr_t>(&onGeneratePrimitives)))
            StatusReport::record("Chams GeneratePrimitives patch installed", true);
        else {
            StatusReport::record("Chams GeneratePrimitives patch failed", false);
            allResolved = false;
        }

        // DrawArray patch on the three chams-relevant descs (see the note above the
        // drawArray arrays for why CBase/CAggregate are excluded).
        for (int d = 0; d < kDrawArrayTargetCount; ++d) {
            if (kDrawArrayTargetIndices[d] != i)
                continue;
            drawArrayAddressPoints[d] = vtable->addressPoint;
            if (drawArrayPatches[d].install(vtable->addressPoint + kDrawArraySlotOffset, reinterpret_cast<std::uintptr_t>(&onSceneObjectDrawArray)))
                StatusReport::record("Chams DrawArray patch installed", true);
            else {
                StatusReport::record("Chams DrawArray patch failed", false);
                allResolved = false;
            }
        }
    }
    return allResolved;
}

}
