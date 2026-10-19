#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <CS2/Classes/Entities/C_BaseEntity.h>
#include <CS2/Classes/PlayerRankingData.h>
#include <Features/Game/FakePremierConfigVariables.h>
#include <GameClient/Entities/PlayerController.h>
#include <HookContext/HookContextMacros.h>
#include <MemoryPatterns/PatternTypes/ClientPatternTypes.h>
















template <typename HookContext>
class FakePremier {
public:
    explicit FakePremier(HookContext& hookContext) noexcept
        : hookContext{hookContext}
    {
    }

    void run() const noexcept
    {
        const bool premier = GET_CONFIG_VAR(FakePremierEnabled);
        const bool wingman = GET_CONFIG_VAR(FakeWingmanEnabled);
        // controller/block carry one current-mode slot (premier wins when both are on);
        // the tree below carries every mode's node independently
        const bool controllerWingman = wingman && !premier;

        applyRankingBlock(premier, controllerWingman);
        applyRankTree(premier, wingman);

        auto* const controllerEntity = static_cast<cs2::C_BaseEntity*>(hookContext.localPlayerController().baseEntity());
        if (!controllerEntity)
            return;

        const auto rankingOffset = hookContext.schemaSystem().getFieldOffset("CCSPlayerController", "m_iCompetitiveRanking");
        const auto rankTypeOffset = hookContext.schemaSystem().getFieldOffset("CCSPlayerController", "m_iCompetitiveRankType");
        const auto winsOffset = hookContext.schemaSystem().getFieldOffset("CCSPlayerController", "m_iCompetitiveWins");
        if (!rankingOffset.has_value() || *rankingOffset <= 0 || !rankTypeOffset.has_value() || *rankTypeOffset <= 0 || !winsOffset.has_value() || *winsOffset <= 0)
            return;

        cachedController = controllerEntity;
        cachedRankingOffset = *rankingOffset;
        cachedRankTypeOffset = *rankTypeOffset;
        cachedWinsOffset = *winsOffset;

        if (premier || controllerWingman) {
            captureOriginal(controllerEntity, *rankingOffset, *rankTypeOffset, *winsOffset);
            apply(controllerEntity, *rankingOffset, *rankTypeOffset, *winsOffset, premier, controllerWingman);
        } else if (hasOriginal) {
            restore(controllerEntity, *rankingOffset, *rankTypeOffset, *winsOffset);
        }
    }

    void onUnload() const noexcept
    {
        if (hasOriginal && cachedController && cachedRankingOffset > 0 && cachedRankTypeOffset > 0 && cachedWinsOffset > 0)
            restore(cachedController, cachedRankingOffset, cachedRankTypeOffset, cachedWinsOffset);
        restoreRankingBlock();
    }

private:
    
    
    
    
    
    
    
    void applyRankingBlock(bool premier, bool wingmanMode) const noexcept
    {
        auto* const block = static_cast<std::byte*>(hookContext.patternSearchResults().template get<PlayerRankingDataPointer>());
        if (!block)
            return;

        void* sub{};
        std::memcpy(&sub, block + cs2::PlayerRankingData::kRankingSubstructOffset, sizeof(sub));
        cachedRankingSub = sub;

        if (premier || wingmanMode) {
            if (!sub)
                return; 
            auto* const subBytes = static_cast<std::byte*>(sub);
            const auto rankingBytes = subBytes + cs2::PlayerRankingData::kRankingSubstructRankingOffset;
            const auto winsBytes = subBytes + cs2::PlayerRankingData::kRankingSubstructWinsOffset;
            std::int32_t currentRanking{};
            std::memcpy(&currentRanking, rankingBytes, sizeof(currentRanking));
            std::int32_t currentWins{};
            std::memcpy(&currentWins, winsBytes, sizeof(currentWins));
            const std::int32_t score = wingmanMode
                ? static_cast<std::int32_t>(GET_CONFIG_VAR(WingmanRank))
                : static_cast<std::int32_t>(GET_CONFIG_VAR(FakePremierScore));
            // premier: satisfy the >= 10 competitive-wins gate or the UI hides the skill group
            const std::int32_t wins = premier && GET_CONFIG_VAR(FakePremierWins)
                ? static_cast<std::int32_t>(GET_CONFIG_VAR(PremierWins))
                : currentWins;
            
            if (!(hasBlockOriginal && currentRanking == score && (!premier || wins == currentWins))) {
                std::uint32_t flags{};
                std::memcpy(&flags, block + cs2::PlayerRankingData::kFlagsOffset, sizeof(flags));
                blockOriginalHadFlag = (flags & cs2::PlayerRankingData::kRankingPresentFlag) != 0;
                blockOriginalRanking = currentRanking;
                blockOriginalWins = currentWins;
                hasBlockOriginal = true;
            }
            std::memcpy(rankingBytes, &score, sizeof(score));
            if (premier)
                std::memcpy(winsBytes, &wins, sizeof(wins));
            std::uint32_t flags{};
            std::memcpy(&flags, block + cs2::PlayerRankingData::kFlagsOffset, sizeof(flags));
            flags |= cs2::PlayerRankingData::kRankingPresentFlag;
            std::memcpy(block + cs2::PlayerRankingData::kFlagsOffset, &flags, sizeof(flags));
        } else if (hasBlockOriginal && cachedRankingSub) {
            auto* const subBytes = static_cast<std::byte*>(cachedRankingSub);
            const auto rankingBytes = subBytes + cs2::PlayerRankingData::kRankingSubstructRankingOffset;
            const auto winsBytes = subBytes + cs2::PlayerRankingData::kRankingSubstructWinsOffset;
            std::int32_t currentRanking{};
            std::memcpy(&currentRanking, rankingBytes, sizeof(currentRanking));
            std::int32_t currentWins{};
            std::memcpy(&currentWins, winsBytes, sizeof(currentWins));
            if (currentRanking == static_cast<std::int32_t>(GET_CONFIG_VAR(FakePremierScore))
                || currentRanking == static_cast<std::int32_t>(GET_CONFIG_VAR(WingmanRank))) {
                std::memcpy(rankingBytes, &blockOriginalRanking, sizeof(blockOriginalRanking));
                std::memcpy(winsBytes, &blockOriginalWins, sizeof(blockOriginalWins));
            }
            if (!blockOriginalHadFlag) {
                std::uint32_t flags{};
                std::memcpy(&flags, block + cs2::PlayerRankingData::kFlagsOffset, sizeof(flags));
                flags &= ~cs2::PlayerRankingData::kRankingPresentFlag;
                std::memcpy(block + cs2::PlayerRankingData::kFlagsOffset, &flags, sizeof(flags));
            }
            hasBlockOriginal = false;
        }
    }

    // GC rank cache tree (cs2::GcRankCacheTree): what the display reads.
    // carries only keys the GC sent.
    void applyRankTree(bool premier, bool wingmanMode) const noexcept
    {
        using Tree = cs2::GcRankCacheTree;
        auto&& ps = hookContext.patternSearchResults();
        auto* const baseSlot = static_cast<std::byte*>(ps.template get<RankCacheBasePointer>());
        auto* const rootSlot = static_cast<std::byte*>(ps.template get<RankCacheRootPointer>());
        auto* const flagSlot = static_cast<std::byte*>(ps.template get<RankCacheFlagPointer>());
        if (!baseSlot || !rootSlot || !flagSlot)
            return;

        void* base{};
        std::memcpy(&base, baseSlot, sizeof(base));
        std::uint32_t flag{};
        std::memcpy(&flag, flagSlot, sizeof(flag));
        if (!base || (flag & Tree::kFlagMask) == 0)
            return;
        auto* const nodes = static_cast<std::byte*>(base);
        std::int32_t rootIdx{};
        std::memcpy(&rootIdx, rootSlot, sizeof(rootIdx));
        if (rootIdx == Tree::kInvalidIndex)
            return;

        const bool spoofWins = GET_CONFIG_VAR(FakePremierWins);
        const std::int32_t winsValue = static_cast<std::int32_t>(GET_CONFIG_VAR(PremierWins));

        // toggles are independent: premier -> key 11, wingman -> key 7.
        // NO INSERT: findTreeNode only walks root-reachable slots; the allocation
        // holds only ~4 slots, "detached slots 4..7" are an adjacent GC-heap object
        // (40-byte stride) - writing them corrupts its child indexes ->
        // SIGSEGV libclient+0x1ec4eca.
        // GC-shared heap, nodes move between snapshots: dirty-checked writes only
        // (unconditional 64 Hz writes race the GC into freed/reused allocations);
        // the spoof still re-applies when the GC reverts it.
        const auto writeIfChanged = [](std::byte* node, int offset, std::int32_t value) noexcept {
            std::int32_t current{};
            std::memcpy(&current, node + offset, sizeof(current));
            if (current != value)
                std::memcpy(node + offset, &value, sizeof(value));
        };
        if (premier) {
            auto* node = findTreeNode(nodes, rootIdx, Tree::kRankTypePremier);
            if (node) {
                const std::int32_t score = static_cast<std::int32_t>(GET_CONFIG_VAR(FakePremierScore));
                writeIfChanged(node, Tree::kNodeRatingOffset, score);
                if (spoofWins)
                    writeIfChanged(node, Tree::kNodeWinsOffset, winsValue);
            }
        }
        if (wingmanMode) {
            auto* node = findTreeNode(nodes, rootIdx, Tree::kRankTypeWingman);
            if (node) {
                const std::int32_t rank = static_cast<std::int32_t>(GET_CONFIG_VAR(WingmanRank));
                writeIfChanged(node, Tree::kNodeRatingOffset, rank);
                if (spoofWins)
                    writeIfChanged(node, Tree::kNodeWinsOffset, winsValue);
            }
        }

        // the >= 10 wins gate reads the competitive node's (key 12) wins field;
        // satisfy it without touching the classic comp rating
        if (spoofWins) {
            if (auto* compNode = findTreeNode(nodes, rootIdx, Tree::kRankTypeCompetitive))
                writeIfChanged(compNode, Tree::kNodeWinsOffset, winsValue);
        }
    }

    // returns the NODE BASE; add GcRankCacheTree offsets at the use site (136-byte nodes)
    [[nodiscard]] static std::byte* findTreeNode(std::byte* nodes, std::int32_t idx, std::int32_t wanted) noexcept
    {
        using Tree = cs2::GcRankCacheTree;
        for (int step = 0; step < Tree::kMaxWalkSteps; ++step) {
            auto* const node = nodes + Tree::kNodeStride * idx;
            std::int32_t key{};
            std::memcpy(&key, node + Tree::kNodeKeyOffset, sizeof(key));
            if (key == wanted)
                return node;
            std::int32_t next{};
            std::memcpy(&next, node + (wanted > key ? Tree::kNodeRightOffset : Tree::kNodeLeftOffset), sizeof(next));
            if (next == Tree::kInvalidIndex)
                return nullptr;
            idx = next;
        }
        return nullptr;
    }

    void restoreRankingBlock() const noexcept
    {
        auto* const block = static_cast<std::byte*>(hookContext.patternSearchResults().template get<PlayerRankingDataPointer>());
        if (!block)
            return;
        if (hasBlockOriginal && cachedRankingSub) {
            auto* const subBytes = static_cast<std::byte*>(cachedRankingSub);
            std::memcpy(subBytes + cs2::PlayerRankingData::kRankingSubstructRankingOffset, &blockOriginalRanking, sizeof(blockOriginalRanking));
            std::memcpy(subBytes + cs2::PlayerRankingData::kRankingSubstructWinsOffset, &blockOriginalWins, sizeof(blockOriginalWins));
        }
        if (hasBlockOriginal && !blockOriginalHadFlag) {
            std::uint32_t flags{};
            std::memcpy(&flags, block + cs2::PlayerRankingData::kFlagsOffset, sizeof(flags));
            flags &= ~cs2::PlayerRankingData::kRankingPresentFlag;
            std::memcpy(block + cs2::PlayerRankingData::kFlagsOffset, &flags, sizeof(flags));
        }
        hasBlockOriginal = false;
    }

    void apply(cs2::C_BaseEntity* controllerEntity, int rankingOffset, int rankTypeOffset, int winsOffset, bool premier, bool wingmanMode) const noexcept
    {
        const auto bytes = reinterpret_cast<std::byte*>(controllerEntity);
        if (wingmanMode) {
            const std::int32_t rank = static_cast<std::int32_t>(GET_CONFIG_VAR(WingmanRank));
            std::memcpy(bytes + rankingOffset, &rank, sizeof(rank));
            const std::int8_t rankType = kWingmanRankType;
            std::memcpy(bytes + rankTypeOffset, &rankType, sizeof(rankType));
            return;
        }
        const std::int32_t score = GET_CONFIG_VAR(FakePremierScore);
        std::memcpy(bytes + rankingOffset, &score, sizeof(score));
        const std::int8_t premierRankType = kPremierRankType;
        std::memcpy(bytes + rankTypeOffset, &premierRankType, sizeof(premierRankType));
        // controller-side wins gate (m_iCompetitiveWins): keep the real value unless spoofing
        std::int32_t wins{};
        std::memcpy(&wins, bytes + winsOffset, sizeof(wins));
        const std::int32_t spoofedWins = GET_CONFIG_VAR(FakePremierWins)
            ? static_cast<std::int32_t>(GET_CONFIG_VAR(PremierWins))
            : wins;
        std::memcpy(bytes + winsOffset, &spoofedWins, sizeof(spoofedWins));
    }

    
    
    
    void captureOriginal(cs2::C_BaseEntity* controllerEntity, int rankingOffset, int rankTypeOffset, int winsOffset) const noexcept
    {
        const auto bytes = reinterpret_cast<const std::byte*>(controllerEntity);
        std::int32_t ranking{};
        std::memcpy(&ranking, bytes + rankingOffset, sizeof(ranking));
        std::int8_t rankType{};
        std::memcpy(&rankType, bytes + rankTypeOffset, sizeof(rankType));
        std::int32_t wins{};
        std::memcpy(&wins, bytes + winsOffset, sizeof(wins));
        if (hasOriginal && rankType == kPremierRankType && ranking == static_cast<std::int32_t>(GET_CONFIG_VAR(FakePremierScore)))
            return;
        if (hasOriginal && rankType == kWingmanRankType && ranking == static_cast<std::int32_t>(GET_CONFIG_VAR(WingmanRank)))
            return;

        originalRanking = ranking;
        originalRankType = rankType;
        originalWins = wins;
        hasOriginal = true;
    }

    void restore(cs2::C_BaseEntity* controllerEntity, int rankingOffset, int rankTypeOffset, int winsOffset) const noexcept
    {
        const auto bytes = reinterpret_cast<std::byte*>(controllerEntity);
        std::memcpy(bytes + rankingOffset, &originalRanking, sizeof(originalRanking));
        std::memcpy(bytes + rankTypeOffset, &originalRankType, sizeof(originalRankType));
        std::memcpy(bytes + winsOffset, &originalWins, sizeof(originalWins));
        hasOriginal = false;
    }

    static constexpr std::int8_t kPremierRankType = 11; 
    // The game's own rankType jump table (0x1EE5540 on 11106093): 7=Wingman, 8=ScrimComp2v2,
    // 9=ScrimComp5v5, 10=DangerZone, 11=Premier, 12=Competitive.
    static constexpr std::int8_t kWingmanRankType = 7;
    // premier skill group is gated behind >= 10 competitive wins
    static constexpr std::int32_t kMinPremierWins = 10;

    inline static cs2::C_BaseEntity* cachedController{nullptr};
    inline static int cachedRankingOffset{0};
    inline static int cachedRankTypeOffset{0};
    inline static int cachedWinsOffset{0};
    inline static std::int32_t originalRanking{};
    inline static std::int8_t originalRankType{};
    inline static std::int32_t originalWins{};
    inline static bool hasOriginal{false};

    inline static void* cachedRankingSub{nullptr};
    inline static std::int32_t blockOriginalRanking{};
    inline static std::int32_t blockOriginalWins{};
    inline static bool blockOriginalHadFlag{false};
    inline static bool hasBlockOriginal{false};


    HookContext& hookContext;
};
