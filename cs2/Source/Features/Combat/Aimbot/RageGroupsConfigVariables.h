#pragma once

#include <cstdint>

#include <Utils/InRange.h>
#include <GameClient/Bind.h>
#include <Features/Combat/Aimbot/RageWeaponDefs.h>

// per-weapon-group rage config. one type per (group, field) so every value is
// a distinct ConfigVariable with its own storage/load/save/bind entry; the
// Field tag types keep the aliases distinct (else Enabled<G> and HitHead<G>
// collapse into one type and one slot).
//
// group comes from the active weapon's VData (m_WeaponType): pistol=1 smg=2
// rifle=3 shotgun=4 sniper=5 lmg=6. non-gun weapons resolve to no group and
// the ragebot stands down.

namespace rage_groups
{

struct Pistol {};
struct Smg {};
struct Rifle {};
struct Shotgun {};
struct Sniper {};
struct Lmg {};

struct EnabledField {};
// specific-weapon override: when set, the weapon's own config (RageWeaponDefs.h)
// replaces its weapon-type group's for the active resolution.
struct OverrideEnabledField {};
struct MultipointField {};
struct PointScaleField {};
struct DynamicPointscaleField {};
struct BodyAimField {};
struct ForceShotField {};
struct ForceShotAirField {};
struct HitchanceField {};
struct MinDamageField {};
struct MinDamageOverrideKeyField {};
struct MinDamageOverrideValueField {};
struct HitHeadField {};
struct HitChestField {};
struct HitStomachField {};
struct HitArmsField {};
struct HitLegsField {};
struct DoubletapField {};

template <typename Group, typename Field, bool defaultValue>
struct GroupBoolVariable {
    using ValueType = bool;
    static constexpr ValueType kDefaultValue{defaultValue};
};

template <typename Group, typename Field, typename T, T min, T max, T defaultValue>
struct GroupRangeVariable {
    using ValueType = InRange<T, min, max>;
    static constexpr ValueType kDefaultValue{defaultValue};
};

template <typename Group>
using Enabled = GroupBoolVariable<Group, EnabledField, true>;

template <typename Group>
using OverrideEnabled = GroupBoolVariable<Group, OverrideEnabledField, false>;

// defaults ON; off-by-default locked every group to the single hitbox center.
template <typename Group>
using Multipoint = GroupBoolVariable<Group, MultipointField, true>;

template <typename Group>
using PointScale = GroupRangeVariable<Group, PointScaleField, std::uint8_t, 0, 100, 85>;

template <typename Group>
using DynamicPointscale = GroupBoolVariable<Group, DynamicPointscaleField, true>;

template <typename Group>
using BodyAim = GroupBoolVariable<Group, BodyAimField, false>;

template <typename Group>
using ForceShot = GroupBoolVariable<Group, ForceShotField, false>;

template <typename Group>
using ForceShotAir = GroupBoolVariable<Group, ForceShotAirField, false>;

template <typename Group>
using Hitchance = GroupRangeVariable<Group, HitchanceField, std::uint8_t, 0, 100, 0>;

template <typename Group>
using MinDamage = GroupRangeVariable<Group, MinDamageField, std::uint8_t, 0, 200, 0>;

// hold-bind: while held the group's min damage becomes MinDamageOverrideValue
// (default 11).
template <typename Group>
using MinDamageOverrideKey = GroupRangeVariable<Group, MinDamageOverrideKeyField, std::uint8_t, 0, static_cast<std::uint8_t>(Bind::kLast), Bind::kOff>;

template <typename Group>
using MinDamageOverrideValue = GroupRangeVariable<Group, MinDamageOverrideValueField, std::uint8_t, 0, 200, 11>;

template <typename Group>
using HitHead = GroupBoolVariable<Group, HitHeadField, true>;

template <typename Group>
using HitChest = GroupBoolVariable<Group, HitChestField, false>;

template <typename Group>
using HitStomach = GroupBoolVariable<Group, HitStomachField, false>;

template <typename Group>
using HitArms = GroupBoolVariable<Group, HitArmsField, false>;

template <typename Group>
using HitLegs = GroupBoolVariable<Group, HitLegsField, false>;

template <typename Group>
using Doubletap = GroupBoolVariable<Group, DoubletapField, false>;

}
