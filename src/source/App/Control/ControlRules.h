// The control socket's pure rules: the arithmetic and comparisons the shop and
// stat commands decide with, free of game globals so the core tests can check them.
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace App::Control::Rules
{
// The client's purchase latch (BuyCost) is an int, and a price of 0 never arms it.
[[nodiscard]] inline bool IsLatchablePrice(std::int64_t cost)
{
    return cost > 0 && cost <= std::numeric_limits<int>::max();
}

// The price the shop shows: the tax on the price, rounded down as its tooltip does.
[[nodiscard]] inline std::int64_t ShownPrice(std::int64_t price, int taxRate)
{
    return price + price * taxRate / 100;
}

// An inventory slot as a purchase or a sale can change it: a stack that takes one more keeps its
// slot and changes its count, which the client keeps as the durability.
struct SlotState
{
    int slot;
    int type;
    int durability;
    bool operator==(const SlotState&) const = default;
};

// The first slot of `now` whose item is new or changed since `before`.
[[nodiscard]] inline std::optional<int> ChangedSlot(const std::vector<SlotState>& before,
                                                    const std::vector<SlotState>& now)
{
    for (const SlotState& current : now)
    {
        bool same = false;
        for (const SlotState& earlier : before)
        {
            same = same || (earlier.slot == current.slot && earlier == current);
        }
        if (!same)
        {
            return current.slot;
        }
    }
    return std::nullopt;
}

// stat-add: a whole number of points from 1 up to the level-up points left.
[[nodiscard]] inline bool PointsWithin(int points, int available)
{
    return points >= 1 && points <= available;
}

// npc-talk: the hero talks from within `reach` tiles of the NPC on both axes (dx, dy in tiles).
[[nodiscard]] inline bool WithinTalkReach(int dx, int dy, int reach)
{
    return std::abs(dx) <= reach && std::abs(dy) <= reach;
}

// The client sits only while the character is alive and still: a walk or a death
// ends the sitting pose, and the server stops the rest with it.
[[nodiscard]] inline bool CanSit(bool alive, bool moving)
{
    return alive && !moving;
}

// Character select (char-list, char-create, char-delete). The name limits are the client's
// (CCharMakeWin's kMinCharacterNameLength and kCharNameMaxLength).
inline constexpr std::size_t MinCharacterNameLength = 4;
inline constexpr std::size_t MaxCharacterNameLength = 10;
// The classes the client creates: CLASS_TYPE 0 to 6, the first class of each line.
inline constexpr int CreatableClassCount = 7;
// The class cards the server sends, read in the client's order: Dark (CLASS_DARK, 3), Dark Lord and Summoner.
inline constexpr int ClassCardCount = 3;

// Code name by CLASS_TYPE number, 0 (Dark Wizard) to 17 (Temple Knight).
inline constexpr std::array<std::string_view, 18> ClassCodeNames = {{
    "DarkWizard", "DarkKnight", "FairyElf", "MagicGladiator", "DarkLord", "Summoner", "RageFighter",
    "SoulMaster", "BladeKnight", "MuseElf", "BloodySummoner", "GrandMaster", "BladeMaster", "HighElf",
    "DuelMaster", "LordEmperor", "DimensionMaster", "TempleKnight",
}};

[[nodiscard]] inline bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

// The code name of a class number, or empty for a number no class has.
[[nodiscard]] inline std::string_view ClassCodeName(int classId)
{
    return classId >= 0 && classId < static_cast<int>(ClassCodeNames.size())
               ? ClassCodeNames[static_cast<std::size_t>(classId)]
               : std::string_view{};
}

// A creatable class from its code name, its short name or its number, ignoring case.
[[nodiscard]] inline std::optional<int> ResolveCreatableClass(std::string_view text)
{
    static constexpr std::array<std::string_view, CreatableClassCount> shortNames = {"DW", "DK", "ELF", "MG",
                                                                                     "DL", "SU", "RF"};
    if (!text.empty() && text.size() <= 2 &&
        std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
    {
        int number = 0;
        for (const char c : text)
        {
            number = number * 10 + (c - '0');
        }
        return number < CreatableClassCount ? std::optional<int>(number) : std::nullopt;
    }
    for (int classId = 0; classId < CreatableClassCount; ++classId)
    {
        const auto index = static_cast<std::size_t>(classId);
        if (EqualsIgnoreCase(text, ClassCodeNames[index]) || EqualsIgnoreCase(text, shortNames[index]))
        {
            return classId;
        }
    }
    return std::nullopt;
}

// Whether the account may create the class now. Dark (MG), Dark Lord and Summoner need their card
// (CCharMakeWin::UpdateDisplay's gate, which the build enables); the other four are always open.
[[nodiscard]] inline bool ClassCreatable(int classId, const std::array<bool, ClassCardCount>& cards)
{
    if (classId < 0 || classId >= CreatableClassCount)
    {
        return false;
    }
    constexpr int firstCarded = 3;
    if (classId >= firstCarded && classId < firstCarded + ClassCardCount)
    {
        return cards[static_cast<std::size_t>(classId - firstCarded)];
    }
    return true;
}

// A new name: 4 to 10 ASCII letters or digits. Stricter than the client, which only refuses spaces and symbols.
[[nodiscard]] inline bool IsCharacterNameValid(std::string_view name)
{
    if (name.size() < MinCharacterNameLength || name.size() > MaxCharacterNameLength)
    {
        return false;
    }
    return std::all_of(name.begin(), name.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
}

enum class CreateRefusal : std::uint8_t
{
    None,
    InvalidName,
    UnknownClass,
    ClassNotAllowed,
    NoFreeSlot,
};

// char-create's own checks, in the order they are reported.
[[nodiscard]] inline CreateRefusal CheckCreate(std::string_view name, std::optional<int> classId,
                                               const std::array<bool, ClassCardCount>& cards, bool hasFreeSlot)
{
    if (!IsCharacterNameValid(name))
    {
        return CreateRefusal::InvalidName;
    }
    if (!classId)
    {
        return CreateRefusal::UnknownClass;
    }
    if (!ClassCreatable(*classId, cards))
    {
        return CreateRefusal::ClassNotAllowed;
    }
    return hasFreeSlot ? CreateRefusal::None : CreateRefusal::NoFreeSlot;
}

// char-delete refuses a guild member's character and an item-blocked one, as the client's delete does.
[[nodiscard]] inline bool CanDeleteCharacter(bool guildMember, bool itemBlocked)
{
    return !guildMember && !itemBlocked;
}

// npc-talk: a tile the hero could talk from. `steps` is its path length, -1 when no path reaches it.
struct TalkTile
{
    int x;
    int y;
    int steps;
};

// npc-talk: the reachable candidate nearest the NPC (Chebyshev distance, at most `reach`); ties go to
// the shorter path. Empty when no candidate qualifies.
[[nodiscard]] inline std::optional<TalkTile> PickTalkTile(int npcX, int npcY, const std::vector<TalkTile>& candidates,
                                                          int reach)
{
    std::optional<TalkTile> best;
    int bestDistance = 0;
    for (const TalkTile& tile : candidates)
    {
        const int distance = std::max(std::abs(tile.x - npcX), std::abs(tile.y - npcY));
        if (tile.steps < 0 || distance > reach)
        {
            continue;
        }
        if (!best || distance < bestDistance || (distance == bestDistance && tile.steps < best->steps))
        {
            best = tile;
            bestDistance = distance;
        }
    }
    return best;
}
// ammo: an equipped arrow or bolt stack and its count (the durability of an arrow is its count).
struct EquippedAmmo
{
    bool bolt;
    int count;
};

struct AmmoCounts
{
    std::optional<int> arrow;
    std::optional<int> bolt;
};

// The count of each kind equipped; a kind that is not equipped is null. The first stack of a kind wins.
[[nodiscard]] inline AmmoCounts CountEquippedAmmo(const std::vector<EquippedAmmo>& equipped)
{
    AmmoCounts counts;
    for (const EquippedAmmo& ammo : equipped)
    {
        std::optional<int>& kind = ammo.bolt ? counts.bolt : counts.arrow;
        if (!kind)
        {
            kind = ammo.count;
        }
    }
    return counts;
}

// A window as `ui` and `npc-talk` name it.
template <typename Key>
struct NamedWindow
{
    std::string_view name;
    Key key;
};

// The name of the first window of `table` that `isOpen` reports open; empty when none is.
template <typename Key, std::size_t N, typename IsOpen>
[[nodiscard]] inline std::string_view FirstOpenWindow(const NamedWindow<Key> (&table)[N], IsOpen&& isOpen)
{
    for (const NamedWindow<Key>& window : table)
    {
        if (isOpen(window.key))
        {
            return window.name;
        }
    }
    return {};
}

// An inventory square's item, as pickup compares the squares before and after the pickup.
struct InventoryEntry
{
    int slot;
    int type;
    int level;
    int durability;

    bool operator==(const InventoryEntry&) const = default;
};

// pickup: the square a picked item landed in, the one new entry of the drop's type and level.
// Null when there is none or more than one, so the caller reports the slot as unknown.
[[nodiscard]] inline std::optional<int> PlacedItemSlot(const std::vector<InventoryEntry>& before,
                                                       const std::vector<InventoryEntry>& after, int type, int level)
{
    std::optional<int> placed;
    int matches = 0;
    for (const InventoryEntry& entry : after)
    {
        if (entry.type != type || entry.level != level ||
            std::find(before.begin(), before.end(), entry) != before.end())
        {
            continue;
        }
        placed = entry.slot;
        ++matches;
    }
    return matches == 1 ? placed : std::nullopt;
}

} // namespace App::Control::Rules
