#include <gtest/gtest.h>

#include <Features/Visuals/Chams/ChamsMaterials.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{

std::vector<std::string> extractResources(std::string_view text)
{
    std::vector<std::string> resources;
    std::size_t pos = 0;
    while ((pos = text.find("resource:\"", pos)) != std::string_view::npos) {
        const auto start = pos + 10;
        const auto end = text.find('"', start);
        if (end == std::string_view::npos)
            break;
        resources.emplace_back(text.substr(start, end - start));
        pos = end;
    }
    return resources;
}

// Every texture the shipped vmats may reference. Verified to exist as a compiled
// resource in the current game pak01_dir.vpk tree (2026-10-17 offline VPK audit).
// If a new vmat needs a new texture, verify it against the VPKs first, then add it here.
const std::vector<std::string>& verifiedResources()
{
    static const std::vector<std::string> resources = {
        "materials/vgui/white_color_tga_998b1b45.vtex",
        "materials/default/default_normal_tga_1b833b2a.vtex",
        "materials/default/default_metal_tga_8fbc2820.vtex",
        "materials/default/default_ao_tga_79a2e0d0.vtex",
        "materials/default/default_mask_tga_fde710a5.vtex",
        "materials/default/default_mask_tga_344101f8.vtex",
    };
    return resources;
}

}

TEST(ChamsMaterialsTest, MaterialTypeParamsCoverTheWholeEnum)
{
    static_assert(chams_materials::kMaterialTypeParams.max == chams_materials::kMaterialTypeCount - 1);
    static_assert(chams_materials::kMaterialTypeParams.min == 0);
    static_assert(chams_materials::kMaterialTypeParams.def == 0);
    EXPECT_EQ(static_cast<int>(chams_materials::Type::Wireframe), chams_materials::kMaterialTypeCount - 1);
}

TEST(ChamsMaterialsTest, MaterialNamesAreComplete)
{
    for (const auto* name : chams_materials::kMaterialTypeNames)
        ASSERT_NE(name, nullptr);
    for (int i = 0; i < chams_materials::kMaterialTypeCount; ++i)
        for (int j = i + 1; j < chams_materials::kMaterialTypeCount; ++j)
            EXPECT_STRNE(chams_materials::kMaterialTypeNames[i], chams_materials::kMaterialTypeNames[j]);
}

TEST(ChamsMaterialsTest, EveryTypeHasAMaterialDef)
{
    for (const auto& def : chams_materials::detail::kMaterialDefs) {
        EXPECT_NE(def.suffix, nullptr);
        ASSERT_NE(def.visibleText, nullptr);
        ASSERT_NE(def.occludedText, nullptr);
        EXPECT_GT(std::strlen(def.visibleText), 20);
        EXPECT_GT(std::strlen(def.occludedText), 20);
    }
    EXPECT_EQ(std::size(chams_materials::detail::kMaterialDefs), chams_materials::kMaterialTypeCount);
}

TEST(ChamsMaterialsTest, VmatBodiesAreWellFormed)
{
    for (const auto& def : chams_materials::detail::kMaterialDefs) {
        for (const char* const text : {def.visibleText, def.occludedText}) {
            const std::string_view body{text};
            ASSERT_FALSE(body.empty());
            EXPECT_EQ(body.front(), 'S'); // starts with the Shader line, header opens the brace
            EXPECT_EQ(body.back(), '}'); // closes the document the header opens
            EXPECT_NE(body.find("Shader = \""), std::string_view::npos);
            EXPECT_EQ(std::count(body.begin(), body.end(), '{'), 0); // only the header opens blocks
            EXPECT_EQ(std::count(body.begin(), body.end(), '}'), 1);
        }
    }
}

TEST(ChamsMaterialsTest, VmatTexturesAreFromTheVerifiedSet)
{
    for (const auto& def : chams_materials::detail::kMaterialDefs) {
        for (const char* const text : {def.visibleText, def.occludedText}) {
            for (const auto& resource : extractResources(text)) {
                EXPECT_TRUE(std::ranges::any_of(verifiedResources(),
                    [&](const std::string& verified) { return verified == resource; }))
                    << "unverified resource reference: " << resource;
            }
        }
    }
}

TEST(ChamsMaterialsTest, TintableMaterialsPaintVertexColors)
{
    for (int i = 0; i < chams_materials::kMaterialTypeCount; ++i) {
        const auto& def = chams_materials::detail::kMaterialDefs[i];
        const auto type = static_cast<chams_materials::Type>(i);
        for (const char* const text : {def.visibleText, def.occludedText}) {
            const std::string_view body{text};
            const bool paintsVertexColors = body.find("F_PAINT_VERTEX_COLORS = 1") != std::string_view::npos;
            if (type == chams_materials::Type::Wireframe)
                EXPECT_FALSE(paintsVertexColors); // wireframe tints via g_vOverrideColor instead
            else
                EXPECT_TRUE(paintsVertexColors) << "type " << i << " must take the tint from the primitive color";
        }
    }
}

TEST(ChamsMaterialsTest, OccludedMaterialsDisableDepth)
{
    for (const auto& def : chams_materials::detail::kMaterialDefs) {
        const std::string_view occluded{def.occludedText};
        const bool throughWalls = occluded.find("F_IGNOREZ = 1") != std::string_view::npos
            || occluded.find("F_DISABLE_Z_BUFFERING = 1") != std::string_view::npos;
        EXPECT_TRUE(throughWalls) << def.suffix << " occluded variant must draw through geometry";
    }
}

TEST(ChamsMaterialsTest, MaterialLookupFailsClosed)
{
    EXPECT_FALSE(chams_materials::materialsReady());
    EXPECT_EQ(chams_materials::materialFor(chams_materials::Type::Flat, false), nullptr);
    EXPECT_EQ(chams_materials::materialFor(chams_materials::Type::Wireframe, true), nullptr);
}

TEST(ChamsMaterialsTest, PointerVerificationFailsClosedWithoutModuleCodeRange)
{
    int dummy = 0;
    EXPECT_EQ(chams_materials::detail::verifyMaterialPointer(&dummy, "anything"), nullptr);
    EXPECT_EQ(chams_materials::detail::verifyMaterialPointer(nullptr, "anything"), nullptr);
}

TEST(ChamsMaterialsTest, GenericKv3IdMatchesTheGamesOwnStaticTable)
{
    // From the 14191 disasm: the id the game itself passes to LoadKV3 for vmat text
    // (materialsystem2 static tables at 0x1b0480/0x1b0ec0, GUID 7412167c-06e9-4698-aff2-e63eb59037e7).
    EXPECT_STREQ(chams_materials::detail::kGenericKV3ID.name, "generic");
    EXPECT_EQ(chams_materials::detail::kGenericKV3ID.hash1, 0x469806E97412167CULL);
    EXPECT_EQ(chams_materials::detail::kGenericKV3ID.hash2, 0xE73790B53EE6F2AFULL);
}
