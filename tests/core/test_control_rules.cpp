// doctest unit tests for the control socket's pure rules: the purchase latch's
// price range, the shop's tax, what a purchase or sale changed, and the stat
// points range. They need no game state.
//
// Run: ctest --test-dir <build directory> --build-config Release -R "\[core\]\[control\]"

#include "doctest.h"

#include "App/Control/ControlRules.h"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

using namespace App::Control::Rules;

TEST_CASE("A purchase price the latch can hold is from 1 to the int maximum [core][control]")
{
    CHECK_FALSE(IsLatchablePrice(0));
    CHECK_FALSE(IsLatchablePrice(-1));
    CHECK(IsLatchablePrice(1));
    CHECK(IsLatchablePrice(std::numeric_limits<int>::max()));
    CHECK_FALSE(IsLatchablePrice(static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 1));
}

TEST_CASE("The shop's tax rounds the price down like its tooltip [core][control]")
{
    CHECK(ShownPrice(100, 10) == 110);
    CHECK(ShownPrice(99, 5) == 103);  // 99 * 5 / 100 is 4.95, kept as 4
    CHECK(ShownPrice(1, 50) == 1);    // 0.5 tax is nothing
    CHECK(ShownPrice(0, 10) == 0);
    CHECK(ShownPrice(100, 0) == 100);
}

TEST_CASE("A stacked purchase counts as bought: its slot changes its durability [core][control]")
{
    const std::vector<SlotState> before = {{20, 7, 3}, {30, 9, 1}};

    // A potion merges into the stack at slot 20: same slot, count (durability) one up.
    const std::vector<SlotState> stacked = {{20, 7, 4}, {30, 9, 1}};
    CHECK(ChangedSlot(before, stacked) == 20);

    // A new item lands in an empty slot.
    const std::vector<SlotState> added = {{20, 7, 3}, {30, 9, 1}, {31, 2, 0}};
    CHECK(ChangedSlot(before, added) == 31);

    // Nothing changed: no purchase.
    CHECK_FALSE(ChangedSlot(before, before).has_value());

    // An item that left is not a purchase.
    const std::vector<SlotState> left = {{20, 7, 3}};
    CHECK_FALSE(ChangedSlot(before, left).has_value());
}

TEST_CASE("stat-add takes whole points from 1 up to the points left [core][control]")
{
    CHECK(PointsWithin(1, 1));
    CHECK(PointsWithin(5, 5));
    CHECK_FALSE(PointsWithin(2, 1));
    CHECK_FALSE(PointsWithin(0, 5));
    CHECK_FALSE(PointsWithin(-3, 5));
    CHECK_FALSE(PointsWithin(1, 0));
}

TEST_CASE("sit needs a living character standing still [core][control]")
{
    CHECK(CanSit(true, false));
    CHECK_FALSE(CanSit(true, true));
    CHECK_FALSE(CanSit(false, false));
}

TEST_CASE("npc-talk reaches an NPC within the talk reach on both axes [core][control]")
{
    CHECK(WithinTalkReach(1, 1, 1));
    CHECK(WithinTalkReach(-1, 0, 1));
    CHECK(WithinTalkReach(0, 0, 1));
    CHECK_FALSE(WithinTalkReach(2, 0, 1));
    CHECK(WithinTalkReach(-2, 2, 2));  // NPC behind a counter: nearest reachable tile
    CHECK_FALSE(WithinTalkReach(3, 0, 2));
    CHECK_FALSE(WithinTalkReach(0, -3, 2));
}

TEST_CASE("A new character's name is 4 to 10 ASCII letters or digits [core][control]")
{
    CHECK(IsCharacterNameValid("Hero"));
    CHECK(IsCharacterNameValid("Hero1234ab"));  // 10
    CHECK_FALSE(IsCharacterNameValid("Her"));
    CHECK_FALSE(IsCharacterNameValid("Hero12345ab"));  // 11
    CHECK_FALSE(IsCharacterNameValid("He ro"));
    CHECK_FALSE(IsCharacterNameValid("Hero!"));
    CHECK_FALSE(IsCharacterNameValid(""));
}

TEST_CASE("A class resolves from its code name or short name or number ignoring case [core][control]")
{
    CHECK(ResolveCreatableClass("DarkKnight") == 1);
    CHECK(ResolveCreatableClass("darkknight") == 1);
    CHECK(ResolveCreatableClass("DK") == 1);
    CHECK(ResolveCreatableClass("dl") == 4);
    CHECK(ResolveCreatableClass("6") == 6);
    CHECK(ResolveCreatableClass("RageFighter") == 6);
    CHECK_FALSE(ResolveCreatableClass("7").has_value());  // Soul Master is a second class
    CHECK_FALSE(ResolveCreatableClass("SoulMaster").has_value());
    CHECK_FALSE(ResolveCreatableClass("-1").has_value());
    CHECK_FALSE(ResolveCreatableClass("").has_value());
    CHECK(ClassCodeName(0) == "DarkWizard");
    CHECK(ClassCodeName(17) == "TempleKnight");
    CHECK(ClassCodeName(18).empty());
}

TEST_CASE("Dark and Dark Lord and Summoner need their class card - the rest are always open [core][control]")
{
    const std::array<bool, ClassCardCount> none{false, false, false};
    const std::array<bool, ClassCardCount> all{true, true, true};
    CHECK(ClassCreatable(0, none));
    CHECK(ClassCreatable(1, none));
    CHECK(ClassCreatable(2, none));
    CHECK(ClassCreatable(6, none));
    CHECK_FALSE(ClassCreatable(3, none));
    CHECK_FALSE(ClassCreatable(4, none));
    CHECK_FALSE(ClassCreatable(5, none));
    CHECK(ClassCreatable(3, all));
    CHECK(ClassCreatable(5, all));
    CHECK_FALSE(ClassCreatable(7, all));  // second classes are not created
    const std::array<bool, ClassCardCount> darkLordOnly{false, true, false};
    CHECK(ClassCreatable(4, darkLordOnly));
    CHECK_FALSE(ClassCreatable(5, darkLordOnly));
}

TEST_CASE("char-create checks its name class card and free slot in that order [core][control]")
{
    const std::array<bool, ClassCardCount> none{false, false, false};
    CHECK(CheckCreate("Hero", 1, none, true) == CreateRefusal::None);
    CHECK(CheckCreate("Hr", 1, none, true) == CreateRefusal::InvalidName);
    CHECK(CheckCreate("Hero", std::nullopt, none, true) == CreateRefusal::UnknownClass);
    CHECK(CheckCreate("Hero", 3, none, true) == CreateRefusal::ClassNotAllowed);
    CHECK(CheckCreate("Hero", 1, none, false) == CreateRefusal::NoFreeSlot);
    CHECK(CheckCreate("Hr", std::nullopt, none, false) == CreateRefusal::InvalidName);
}

TEST_CASE("char-delete refuses a guild member's or an item-blocked character [core][control]")
{
    CHECK(CanDeleteCharacter(false, false));
    CHECK_FALSE(CanDeleteCharacter(true, false));
    CHECK_FALSE(CanDeleteCharacter(false, true));
}

TEST_CASE("npc-talk picks the reachable tile nearest the NPC with shorter path first [core][control]")
{
    // NPC at (10, 10); the tiles behind a counter are unreachable or two away.
    const std::vector<TalkTile> tiles = {{12, 10, 2}, {11, 10, 5}, {9, 9, 3}, {10, 8, 1}};
    const std::optional<TalkTile> pick = PickTalkTile(10, 10, tiles, 2);
    REQUIRE(pick.has_value());
    CHECK(pick->x == 9);
    CHECK(pick->y == 9);
    CHECK(pick->steps == 3);

    const std::vector<TalkTile> tie = {{11, 10, 5}, {9, 9, 3}};
    const std::vector<TalkTile> tieReversed = {{9, 9, 3}, {11, 10, 5}};
    CHECK(PickTalkTile(10, 10, tie, 2)->steps == 3);
    CHECK(PickTalkTile(10, 10, tieReversed, 2)->steps == 3);

    const std::vector<TalkTile> beyond = {{12, 10, 1}};
    CHECK(PickTalkTile(10, 10, beyond, 2).has_value());
    CHECK_FALSE(PickTalkTile(10, 10, beyond, 1).has_value());
    const std::vector<TalkTile> farther = {{13, 10, 1}};
    CHECK_FALSE(PickTalkTile(10, 10, farther, 2).has_value());
}

TEST_CASE("npc-talk skips tiles without a path to them [core][control]")
{
    const std::vector<TalkTile> blocked = {{10, 11, -1}};
    CHECK_FALSE(PickTalkTile(10, 10, blocked, 2).has_value());

    const std::vector<TalkTile> nearBlocked = {{11, 11, -1}, {12, 12, 4}};
    const std::optional<TalkTile> pick = PickTalkTile(10, 10, nearBlocked, 2);
    REQUIRE(pick.has_value());
    CHECK(pick->x == 12);
    CHECK(pick->steps == 4);

    const std::vector<TalkTile> none;
    CHECK_FALSE(PickTalkTile(10, 10, none, 2).has_value());
}

TEST_CASE("state reports the equipped arrow and bolt counts with null for a kind not equipped [core][control]")
{
    const AmmoCounts none = CountEquippedAmmo({});
    CHECK_FALSE(none.arrow.has_value());
    CHECK_FALSE(none.bolt.has_value());

    const AmmoCounts arrows = CountEquippedAmmo({{false, 120}});
    CHECK(arrows.arrow == 120);
    CHECK_FALSE(arrows.bolt.has_value());

    const AmmoCounts both = CountEquippedAmmo({{true, 7}, {false, 33}});
    CHECK(both.arrow == 33);
    CHECK(both.bolt == 7);

    const AmmoCounts first = CountEquippedAmmo({{false, 5}, {false, 9}});
    CHECK(first.arrow == 5);
}

TEST_CASE("ui names the first open window of its table and empty when none is open [core][control]")
{
    const NamedWindow<int> table[] = {{"storage", 8}, {"storage_extension", 9}, {"mix", 10}};
    CHECK(FirstOpenWindow(table, [](int key) { return key == 10; }) == "mix");
    CHECK(FirstOpenWindow(table, [](int key) { return key == 9 || key == 8; }) == "storage");
    CHECK(FirstOpenWindow(table, [](int) { return false; }).empty());
}

TEST_CASE("pickup reports the one new square of the drop's type and level [core][control]")
{
    const InventoryEntry potion{12, 5, 0, 1};
    const InventoryEntry sword{20, 9, 3, 30};
    const std::vector<InventoryEntry> before = {potion, sword};

    const std::vector<InventoryEntry> landed = {potion, sword, {40, 5, 0, 1}};
    CHECK(PlacedItemSlot(before, landed, 5, 0) == 40);
    CHECK_FALSE(PlacedItemSlot(before, landed, 9, 3).has_value());

    const std::vector<InventoryEntry> stacked = {{12, 5, 0, 2}, sword};
    CHECK(PlacedItemSlot(before, stacked, 5, 0) == 12);

    const std::vector<InventoryEntry> twoNew = {potion, sword, {40, 5, 0, 1}, {41, 5, 0, 1}};
    CHECK_FALSE(PlacedItemSlot(before, twoNew, 5, 0).has_value());
    CHECK_FALSE(PlacedItemSlot(before, before, 5, 0).has_value());
}
