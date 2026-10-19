#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <CS2/Constants/DllNames.h>
#include <Config/RangeConstrainedVariableParams.h>
#include <Platform/DynamicLibrary.h>
#include <Platform/Linux/LinuxPlatformApi.h>
#include <Utils/NsStr.h>
#include <Utils/StatusReport.h>

// Runtime-created real vmat materials for player chams.
// Verified against the archived 14191 binaries (offsets from disasm):
//  - libmaterialsystem2 CreateInterface("VMaterialSystem2_001") returns the
//    CMaterialSystem2 singleton; CreateMaterial is vtable slot 28
//    (name, KV3 doc, manager flag, init flag) -> material.
//  - libtier0 LoadKV3(doc, errorString, text, kv3Id, filename, flags). The doc
//    blob must be zero-initialized header + node space; no SetType call on Linux.
//  - Every texture reference below exists in the current game VPKs.
// Primitive binding: material pointers at prim+0x20 AND prim+0x28, color RGBA
// at prim+0x50.
// doc blob = fixed 0x2900-byte zeroed heap block (0x100 header + 0x2800 node
// space); the KV3 parser bump-allocates nodes inside it. Ceiling: very large
// vmats could exhaust it; upgrade path is the CKV3Arena ctor (materialsystem2
// 0x118610). Materials are created once per session and never released (the
// resource system owns them); a session leaks ~14 * 0x2900 bytes.

namespace chams_materials
{

enum class Type : std::uint8_t {
    Flat = 0,
    Metallic,
    Ghost,
    Latex,
    Glow,
    Glass,
    Wireframe,
};

inline constexpr auto kMaterialTypeCount = 7;
inline constexpr auto kMaterialTypeParams = RangeConstrainedVariableParams<std::uint8_t>{.min = 0, .max = 6, .def = 0};

inline constexpr const char* kMaterialTypeNames[kMaterialTypeCount] = {
    "Flat", "Metallic", "Ghost", "Latex", "Glow", "Glass", "Wireframe",
};

namespace detail
{

using LoadKV3Fn = bool (*)(void* doc, void* errorString, const char* text, const void* kv3Id, const char* filename, unsigned int flags);
using CreateMaterialFn = void* (*)(void* materialSystem, const char* name, void* kv3Doc, unsigned int managerFlag, std::uint8_t initFlag);
using GetNameFn = const char* (*)(void* material);

// The "generic" KV3 format id exactly as the game passes it.
struct KV3ID {
    const char* name;
    std::uint64_t hash1;
    std::uint64_t hash2;
};
inline constexpr KV3ID kGenericKV3ID{"generic", 0x469806E97412167C, 0xE73790B53EE6F2AF};

constexpr auto kDocHeaderSize = 0x100;
constexpr auto kDocNodeSpace = 0x2800;
constexpr auto kDocBlobSize = kDocHeaderSize + kDocNodeSpace;

inline constexpr auto kKV3Header =
    "<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->\n{";

inline LoadKV3Fn loadKV3 = nullptr;
inline CreateMaterialFn createMaterial = nullptr;
inline void* materialSystem = nullptr;
inline const std::byte* materialSystemCodeBegin = nullptr;
inline const std::byte* materialSystemCodeEnd = nullptr;
inline bool symbolsResolved = false;
// Cross-thread publication: created on the MAIN thread (CreateMove), consumed
// on the render thread (GeneratePrimitives hook). Visibility is ordered by the
// acquire/release on creationAttempted/creationFailed; never read the material
// pointers unless materialsReady() was observed true.
inline std::atomic<bool> creationAttempted{false};
inline std::atomic<bool> creationFailed{false};
inline char lastError[64] = "not created yet";

inline void setError(const char* message) noexcept
{
    std::snprintf(lastError, sizeof(lastError), "%s", message);
}

[[nodiscard]] inline bool resolveSymbols() noexcept
{
    if (symbolsResolved)
        return loadKV3 && createMaterial && materialSystem;
    symbolsResolved = true;

    const DynamicLibrary tier0{cs2::TIER0_DLL};
    loadKV3 = tier0.getFunctionAddress("_Z7LoadKV3P10KeyValues3P10CUtlStringPKcRK7KV3ID_tS4_j").as<LoadKV3Fn>();

    const DynamicLibrary materialSystemLibrary{cs2::MATERIALSYSTEM2_DLL};
    const auto createInterface = materialSystemLibrary.getFunctionAddress("CreateInterface").as<void* (*)(const char*, int*)>();
    if (createInterface)
        materialSystem = createInterface("VMaterialSystem2_001", nullptr);

    if (materialSystem) {
        const auto codeSection = materialSystemLibrary.getCodeSection();
        materialSystemCodeBegin = codeSection.raw().data();
        materialSystemCodeEnd = codeSection.raw().data() + codeSection.raw().size();
        void* vtable = nullptr;
        if (LinuxPlatformApi::safeRead(materialSystem, &vtable, sizeof(vtable)) && vtable)
            createMaterial = reinterpret_cast<CreateMaterialFn>(reinterpret_cast<void* const*>(vtable)[28]);
    }

    if (!loadKV3 || !createMaterial || !materialSystem) {
        setError("interface resolve failed");
        return false;
    }
    return true;
}

// Accepts CMaterial2* directly (current ABI) or one pointer indirection (older
// handle-style result), but only if vtable slot 0 is a function inside
// libmaterialsystem2's code section AND returns exactly our material name.
// Never calls anything that did not pass the module-code check. Fail-closed.
[[nodiscard]] inline void* verifyMaterialPointer(void* candidate, const char* expectedName) noexcept
{
    for (int dereference = 0; dereference < 2 && candidate; ++dereference) {
        void* vtable = nullptr;
        if (!LinuxPlatformApi::safeRead(candidate, &vtable, sizeof(vtable)) || !vtable)
            return nullptr;
        void* getNameFn = nullptr;
        if (!LinuxPlatformApi::safeRead(vtable, &getNameFn, sizeof(getNameFn)) || !getNameFn)
            return nullptr;
        const auto fnAddress = reinterpret_cast<const std::byte*>(getNameFn);
        if (materialSystemCodeBegin <= fnAddress && fnAddress < materialSystemCodeEnd) {
            const auto name = reinterpret_cast<GetNameFn>(getNameFn)(candidate);
            if (name && std::strcmp(name, expectedName) == 0)
                return candidate;
            return nullptr; // a real material object, but not ours - do not trust it
        }
        void* next = nullptr;
        if (!LinuxPlatformApi::safeRead(candidate, &next, sizeof(next)) || !next)
            return nullptr;
        candidate = next;
    }
    return nullptr;
}

[[nodiscard]] inline void* createMaterialFromText(const char* name, const char* vmatBody) noexcept
{
    auto* const blob = static_cast<unsigned char*>(std::calloc(1, kDocBlobSize));
    if (!blob) {
        setError("doc blob alloc failed");
        return nullptr;
    }
    char text[2048];
    const auto headerLength = std::strlen(kKV3Header);
    if (headerLength + std::strlen(vmatBody) + 1 > sizeof(text)) {
        setError("vmat text overflow");
        return nullptr;
    }
    std::memcpy(text, kKV3Header, headerLength);
    std::strcpy(text + headerLength, vmatBody);

    auto* const doc = reinterpret_cast<void*>(blob + kDocHeaderSize);
    if (!loadKV3(doc, nullptr, text, &kGenericKV3ID, "", 0)) {
        setError("LoadKV3 rejected vmat");
        return nullptr;
    }
    return verifyMaterialPointer(createMaterial(materialSystem, name, doc, 0, 1), name);
}

// All texture refs verified against the current pak01_dir.vpk tree.
inline constexpr auto kFlatVisible =
    "Shader = \"csgo_unlitgeneric.vfx\"\n"
    "F_IGNOREZ = 0\nF_DISABLE_Z_WRITE = 0\nF_DISABLE_Z_BUFFERING = 0\n"
    "F_BLEND_MODE = 1\nF_TRANSLUCENT = 1\nF_RENDER_BACKFACES = 1\nF_PAINT_VERTEX_COLORS = 1\n"
    "g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]\n"
    "g_tColor = resource:\"materials/vgui/white_color_tga_998b1b45.vtex\"\n"
    "g_tNormal = resource:\"materials/default/default_normal_tga_1b833b2a.vtex\"\n}";

inline constexpr auto kFlatOccluded =
    "Shader = \"csgo_unlitgeneric.vfx\"\n"
    "F_IGNOREZ = 1\nF_DISABLE_Z_WRITE = 1\nF_DISABLE_Z_BUFFERING = 1\n"
    "F_BLEND_MODE = 1\nF_TRANSLUCENT = 1\nF_RENDER_BACKFACES = 1\nF_PAINT_VERTEX_COLORS = 1\n"
    "g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]\n"
    "g_tColor = resource:\"materials/vgui/white_color_tga_998b1b45.vtex\"\n"
    "g_tNormal = resource:\"materials/default/default_normal_tga_1b833b2a.vtex\"\n}";

inline constexpr auto kMetallicVisible =
    "Shader = \"csgo_complex.vfx\"\n"
    "F_IGNOREZ = 0\nF_DISABLE_Z_WRITE = 0\nF_DISABLE_Z_BUFFERING = 0\n"
    "F_BLEND_MODE = 1\nF_TRANSLUCENT = 0\nF_RENDER_BACKFACES = 1\nF_PAINT_VERTEX_COLORS = 1\n"
    "g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]\n"
    "g_flMetalness = 1.000000\ng_flRoughness = 0.150000\n"
    "g_tColor = resource:\"materials/vgui/white_color_tga_998b1b45.vtex\"\n"
    "g_tNormal = resource:\"materials/default/default_normal_tga_1b833b2a.vtex\"\n}";

inline constexpr auto kMetallicOccluded =
    "Shader = \"csgo_complex.vfx\"\n"
    "F_IGNOREZ = 1\nF_DISABLE_Z_WRITE = 1\nF_DISABLE_Z_BUFFERING = 1\n"
    "F_BLEND_MODE = 1\nF_TRANSLUCENT = 0\nF_PAINT_VERTEX_COLORS = 1\n"
    "g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]\n"
    "g_flMetalness = 1.000000\ng_flRoughness = 0.150000\n"
    "g_tColor = resource:\"materials/vgui/white_color_tga_998b1b45.vtex\"\n"
    "g_tNormal = resource:\"materials/default/default_normal_tga_1b833b2a.vtex\"\n}";

inline constexpr auto kGhostVisible =
    "Shader = \"csgo_unlitgeneric.vfx\"\n"
    "F_IGNOREZ = 0\nF_DISABLE_Z_WRITE = 0\nF_DISABLE_Z_BUFFERING = 0\n"
    "F_BLEND_MODE = 2\nF_TRANSLUCENT = 1\nF_ADDITIVE_BLEND = 1\n"
    "F_RENDER_BACKFACES = 1\nF_PAINT_VERTEX_COLORS = 1\n"
    "g_vColorTint = [1.000000, 1.000000, 1.000000, 0.550000]\n"
    "g_tColor = resource:\"materials/vgui/white_color_tga_998b1b45.vtex\"\n"
    "g_tNormal = resource:\"materials/default/default_normal_tga_1b833b2a.vtex\"\n}";

inline constexpr auto kGhostOccluded = kFlatOccluded;

inline constexpr auto kLatexVisible =
    "Shader = \"csgo_character.vfx\"\n"
    "F_IGNOREZ = 0\nF_DISABLE_Z_WRITE = 0\nF_DISABLE_Z_BUFFERING = 0\n"
    "F_BLEND_MODE = 1\nF_RENDER_BACKFACES = 1\nF_PAINT_VERTEX_COLORS = 1\n"
    "g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]\n"
    "g_bFogEnabled = 0\ng_flMetalness = 0.000000\n"
    "g_tMetalness = resource:\"materials/default/default_metal_tga_8fbc2820.vtex\"\n"
    "g_tColor = resource:\"materials/vgui/white_color_tga_998b1b45.vtex\"\n"
    "g_tNormal = resource:\"materials/default/default_normal_tga_1b833b2a.vtex\"\n}";

inline constexpr auto kLatexOccluded =
    "Shader = \"csgo_character.vfx\"\n"
    "F_IGNOREZ = 1\nF_DISABLE_Z_WRITE = 1\nF_DISABLE_Z_BUFFERING = 1\n"
    "F_BLEND_MODE = 1\nF_RENDER_BACKFACES = 1\nF_PAINT_VERTEX_COLORS = 1\n"
    "g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]\n"
    "g_bFogEnabled = 0\ng_flMetalness = 0.000000\n"
    "g_tColor = resource:\"materials/vgui/white_color_tga_998b1b45.vtex\"\n"
    "g_tNormal = resource:\"materials/default/default_normal_tga_1b833b2a.vtex\"\n"
    "g_tMetalness = resource:\"materials/default/default_metal_tga_8fbc2820.vtex\"\n}";

inline constexpr auto kGlowVisible =
    "Shader = \"csgo_effects.vfx\"\n"
    "F_RENDER_BACKFACES = 1\nF_PAINT_VERTEX_COLORS = 1\n"
    "g_flFresnelExponent = 7.0\ng_flFresnelFalloff = 10.0\ng_flFresnelMax = 0.1\ng_flFresnelMin = 1.0\n"
    "g_flToolsVisCubemapReflectionRoughness = 1.0\ng_flBeginMixingRoughness = 1.0\n"
    "g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]\n"
    "F_IGNOREZ = 0\nF_DISABLE_Z_WRITE = 0\nF_DISABLE_Z_BUFFERING = 0\n"
    "g_tColor = resource:\"materials/vgui/white_color_tga_998b1b45.vtex\"\n"
    "g_tMask1 = resource:\"materials/default/default_mask_tga_fde710a5.vtex\"\n"
    "g_tMask2 = resource:\"materials/default/default_mask_tga_fde710a5.vtex\"\n"
    "g_tMask3 = resource:\"materials/default/default_mask_tga_fde710a5.vtex\"\n"
    "g_tSceneDepth = resource:\"materials/default/default_mask_tga_fde710a5.vtex\"\n}";

inline constexpr auto kGlowOccluded =
    "Shader = \"csgo_effects.vfx\"\n"
    "F_RENDER_BACKFACES = 1\nF_PAINT_VERTEX_COLORS = 1\n"
    "g_flFresnelExponent = 7.0\ng_flFresnelFalloff = 10.0\ng_flFresnelMax = 0.1\ng_flFresnelMin = 1.0\n"
    "g_flToolsVisCubemapReflectionRoughness = 1.0\ng_flBeginMixingRoughness = 1.0\n"
    "g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]\n"
    "F_IGNOREZ = 1\nF_DISABLE_Z_WRITE = 1\nF_DISABLE_Z_BUFFERING = 1\n"
    "g_tColor = resource:\"materials/vgui/white_color_tga_998b1b45.vtex\"\n"
    "g_tMask1 = resource:\"materials/default/default_mask_tga_fde710a5.vtex\"\n"
    "g_tMask2 = resource:\"materials/default/default_mask_tga_fde710a5.vtex\"\n"
    "g_tMask3 = resource:\"materials/default/default_mask_tga_fde710a5.vtex\"\n"
    "g_tSceneDepth = resource:\"materials/default/default_mask_tga_fde710a5.vtex\"\n}";

inline constexpr auto kGlassVisible =
    "Shader = \"csgo_effects.vfx\"\n"
    "F_RENDER_BACKFACES = 1\nF_PAINT_VERTEX_COLORS = 1\n"
    "g_flOpacityScale = 0.45\ng_flFresnelExponent = 0.75\ng_flFresnelFalloff = 1\n"
    "g_flFresnelMax = 0.0\ng_flFresnelMin = 1\n"
    "F_ADDITIVE_BLEND = 1\nF_BLEND_MODE = 1\nF_TRANSLUCENT = 1\n"
    "F_IGNOREZ = 0\nF_DISABLE_Z_WRITE = 0\nF_DISABLE_Z_BUFFERING = 0\n"
    "g_vColorTint = [1.000000, 1.000000, 1.000000, 0.0]\n"
    "g_tColor = resource:\"materials/vgui/white_color_tga_998b1b45.vtex\"\n"
    "g_tNormal = resource:\"materials/default/default_normal_tga_1b833b2a.vtex\"\n"
    "g_tMask1 = resource:\"materials/default/default_mask_tga_344101f8.vtex\"\n"
    "g_tMask2 = resource:\"materials/default/default_mask_tga_344101f8.vtex\"\n"
    "g_tMask3 = resource:\"materials/default/default_mask_tga_344101f8.vtex\"\n}";

inline constexpr auto kGlassOccluded =
    "Shader = \"csgo_effects.vfx\"\n"
    "F_RENDER_BACKFACES = 1\nF_PAINT_VERTEX_COLORS = 1\n"
    "g_flOpacityScale = 0.45\ng_flFresnelExponent = 0.75\ng_flFresnelFalloff = 1\n"
    "g_flFresnelMax = 0.0\ng_flFresnelMin = 1\n"
    "F_ADDITIVE_BLEND = 1\nF_BLEND_MODE = 1\nF_TRANSLUCENT = 1\n"
    "F_IGNOREZ = 1\nF_DISABLE_Z_WRITE = 0\nF_DISABLE_Z_BUFFERING = 0\n"
    "g_vColorTint = [1.000000, 1.000000, 1.000000, 0.0]\n"
    "g_tColor = resource:\"materials/vgui/white_color_tga_998b1b45.vtex\"\n"
    "g_tNormal = resource:\"materials/default/default_normal_tga_1b833b2a.vtex\"\n"
    "g_tMask1 = resource:\"materials/default/default_mask_tga_344101f8.vtex\"\n"
    "g_tMask2 = resource:\"materials/default/default_mask_tga_344101f8.vtex\"\n"
    "g_tMask3 = resource:\"materials/default/default_mask_tga_344101f8.vtex\"\n}";

inline constexpr auto kWireframeVisible =
    "Shader = \"tools_wireframe.vfx\"\n"
    "F_RENDER_BACKFACES = 1\nF_DISABLE_Z_BUFFERING = 0\nF_DISABLE_Z_PREPASS = 0\nF_DISABLE_Z_WRITE = 0\n"
    "F_UNLIT = 1\nF_WIREFRAME = 1\n"
    "g_DepthBiasAmount = 0.0\ng_LineThickness = 0.25\n"
    "g_OverrideColorFactor = 1.0\ng_vOverrideColor = [1.0, 1.0, 1.0, 1.0]\n}";

inline constexpr auto kWireframeOccluded =
    "Shader = \"tools_wireframe.vfx\"\n"
    "F_RENDER_BACKFACES = 1\nF_DISABLE_Z_BUFFERING = 1\nF_DISABLE_Z_PREPASS = 1\nF_DISABLE_Z_WRITE = 1\n"
    "F_UNLIT = 1\nF_WIREFRAME = 1\n"
    "g_DepthBiasAmount = 0.0\ng_LineThickness = 0.25\n"
    "g_OverrideColorFactor = 1.0\ng_vOverrideColor = [1.0, 1.0, 1.0, 1.0]\n}";

struct MaterialDef {
    const char* suffix;
    const char* visibleText;
    const char* occludedText;
};

inline constexpr MaterialDef kMaterialDefs[kMaterialTypeCount] = {
    {"flat", kFlatVisible, kFlatOccluded},
    {"metallic", kMetallicVisible, kMetallicOccluded},
    {"ghost", kGhostVisible, kGhostOccluded},
    {"latex", kLatexVisible, kLatexOccluded},
    {"glow", kGlowVisible, kGlowOccluded},
    {"glass", kGlassVisible, kGlassOccluded},
    {"wireframe", kWireframeVisible, kWireframeOccluded},
};

inline void* visibleMaterials[kMaterialTypeCount]{};
inline void* occludedMaterials[kMaterialTypeCount]{};

} // namespace detail

// Render-thread safe read: true only after the main thread published the full material set.
[[nodiscard]] inline bool materialsReady() noexcept
{
    return detail::creationAttempted.load(std::memory_order_acquire)
        && !detail::creationFailed.load(std::memory_order_acquire);
}

[[nodiscard]] inline const char* lastError() noexcept
{
    return detail::lastError;
}

// MAIN thread only (CreateMove); creating from inside GeneratePrimitives on the
// render thread corrupts thread state. Creates the whole set once; after one
// attempt the verdict is final for the session.
[[nodiscard]] inline bool ensureMaterialsCreated() noexcept
{
    if (detail::creationAttempted.load(std::memory_order_acquire))
        return !detail::creationFailed.load(std::memory_order_acquire);
    detail::creationAttempted.store(true, std::memory_order_relaxed);

    if (!detail::resolveSymbols()) {
        detail::creationFailed.store(true, std::memory_order_release);
        StatusReport::record("Chams material system resolve failed (color-only fallback)", false);
        return false;
    }

    NS_STR(prefix, "ns_chams/");
    char name[48];
    int created = 0;
    const int total = kMaterialTypeCount * 2;
    for (int i = 0; i < kMaterialTypeCount; ++i) {
        const auto& def = detail::kMaterialDefs[i];
        std::snprintf(name, sizeof(name), "%s%s_vis", prefix.c_str(), def.suffix);
        detail::visibleMaterials[i] = detail::createMaterialFromText(name, def.visibleText);
        if (detail::visibleMaterials[i])
            ++created;
        std::snprintf(name, sizeof(name), "%s%s_occ", prefix.c_str(), def.suffix);
        detail::occludedMaterials[i] = detail::createMaterialFromText(name, def.occludedText);
        if (detail::occludedMaterials[i])
            ++created;
    }

    if (created != total) {
        detail::creationFailed.store(true, std::memory_order_release);
        StatusReport::record("Chams material creation incomplete (color-only fallback)", false);
        return false;
    }

    StatusReport::record("Chams runtime materials created", true);
    detail::creationFailed.store(false, std::memory_order_release); // publishes the material pointers
    return true;
}

// True until the one-shot creation has run; the main thread (CreateMove) uses
// this to decide whether to create; the render-thread hook only ever reads
// materialsReady().
[[nodiscard]] inline bool needsCreation() noexcept
{
    return !detail::creationAttempted.load(std::memory_order_acquire);
}

[[nodiscard]] inline void* materialFor(Type type, bool occluded) noexcept
{
    const auto index = static_cast<int>(type);
    if (index < 0 || index >= kMaterialTypeCount)
        return nullptr;
    return occluded ? detail::occludedMaterials[index] : detail::visibleMaterials[index];
}

}
