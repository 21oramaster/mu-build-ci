#include "stdafx.h"

#include "doctest.h"

#include "App/Control/ControlCommands.h"
#include "App/Control/ControlProtocol.h"
#include "App/Control/ControlState.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace
{
using Command = std::string (*)(const App::Control::Request&, std::unique_ptr<App::Control::Act>&);

std::string Run(Command command, const std::string& raw)
{
    std::unique_ptr<App::Control::Act> act;
    return command(App::Control::Request::Parse(raw), act);
}

bool IsBadRequest(const std::string& response)
{
    return response.find(R"("error":"bad_request")") != std::string::npos;
}
} // namespace

TEST_CASE("repair-tooltip refuses a value its item's bytes can't hold [core][control]")
{
    const auto repair = [](const std::string& fields)
    {
        return Run(&App::Control::Commands::RepairTooltip,
                   R"({"cmd":"repair-tooltip","group":0,"number":0,"level":0,)" + fields + "}");
    };
    // A durability is a byte, the excellent options six bits: what would wrap around is refused.
    CHECK(IsBadRequest(repair(R"("durability":-1)")));
    CHECK(IsBadRequest(repair(R"("durability":256)")));
    CHECK(IsBadRequest(repair(R"("durability":"full")")));
    CHECK(IsBadRequest(repair(R"("excellent":-1)")));
    CHECK(IsBadRequest(repair(R"("excellent":64)")));
    CHECK(IsBadRequest(repair(R"("excellent":256)")));
}

TEST_CASE("item-name-snapshot fails when it can't write its file [core][control]")
{
    // A folder can't be opened as the file.
    const std::filesystem::path folder = std::filesystem::temp_directory_path();
    const std::string response = Run(&App::Control::Commands::ItemNameSnapshot,
                                     R"({"cmd":"item-name-snapshot","out":")" + folder.generic_string() + R"("})");
    CHECK(response.find(R"("error":"failed")") != std::string::npos);
}

TEST_CASE("item-name and repair-tooltip refuse an item number no item uses [core][control]")
{
    // The tests load no item data, so no item exists.
    const std::pair<Command, const char*> commands[] = {{&App::Control::Commands::ItemName, "item-name"},
                                                        {&App::Control::Commands::RepairTooltip, "repair-tooltip"}};
    for (const auto& [command, name] : commands)
    {
        INFO(name);
        const std::string response = Run(command, std::string(R"({"cmd":")") + name + R"(","group":15,"number":511})");
        CHECK(IsBadRequest(response));
        CHECK(response.find("no item has group 15 and number 511") != std::string::npos);
    }
}

TEST_CASE("item-name-snapshot replies the absolute path it wrote [core][control]")
{
    const std::filesystem::path previous = std::filesystem::current_path();
    std::filesystem::current_path(std::filesystem::temp_directory_path());
    const std::string response = Run(&App::Control::Commands::ItemNameSnapshot,
                                     R"({"cmd":"item-name-snapshot","out":"mu_item_name_snapshot_relative.txt"})");
    const std::filesystem::path written = std::filesystem::absolute("mu_item_name_snapshot_relative.txt");
    std::filesystem::current_path(previous);
    CHECK(std::filesystem::exists(written));
    CHECK(response.find("mu_item_name_snapshot_relative.txt") != std::string::npos);
    CHECK(response.find("\"out\":\"mu_item_name_snapshot_relative.txt\"") == std::string::npos);
    std::filesystem::remove(written);
}

TEST_CASE("item identity fields read the excellent, ancient, option, luck, skill, harmony and socket bytes [core][control]")
{
    ITEM item{};
    item.ExcellentFlags = 0x05;
    item.AncientDiscriminator = 3;
    item.AncientBonusOption = 4;
    item.OptionLevel = 2;
    item.OptionType = 1;
    item.HasLuck = true;
    item.HasSkill = true;
    item.Jewel_Of_Harmony_Option = 7;
    item.Jewel_Of_Harmony_OptionLevel = 9;
    item.SocketSeedSetOption = 2;
    item.SocketCount = 2;
    item.bySocketOption[0] = 11;
    item.SocketSeedID[0] = 12;
    item.SocketSphereLv[0] = 1;
    item.bySocketOption[1] = 0xFF;
    item.SpecialNum = 1;
    item.Special[0] = 40;
    item.SpecialValue[0] = 300;

    const nlohmann::json fields = App::Control::ItemIdentityFields(item);
    CHECK(fields["excellent"] == 5);
    CHECK(fields["ancient"] == 3);
    CHECK(fields["ancient_bonus"] == 4);
    CHECK(fields["option"] == 2);
    CHECK(fields["option_type"] == 1);
    CHECK(fields["luck"] == true);
    CHECK(fields["skill"] == true);
    CHECK(fields["harmony"] == 7);
    CHECK(fields["harmony_level"] == 9);
    CHECK(fields["socket_seed_set_option"] == 2);
    REQUIRE(fields["sockets"].size() == 2);
    CHECK(fields["sockets"][0]["option"] == 11);
    CHECK(fields["sockets"][0]["seed_id"] == 12);
    CHECK(fields["sockets"][0]["sphere_level"] == 1);
    CHECK(fields["sockets"][1]["option"] == 255);
    REQUIRE(fields["option_slots"].size() == 1);
    CHECK(fields["option_slots"][0]["slot"] == 0);
    CHECK(fields["option_slots"][0]["effect"] == 40);
    CHECK(fields["option_slots"][0]["value"] == 300);
}

TEST_CASE("item identity fields of a plain item are empty and its socket count stops at the socket slots [core][control]")
{
    ITEM item{};
    nlohmann::json fields = App::Control::ItemIdentityFields(item);
    CHECK(fields["excellent"] == 0);
    CHECK(fields["luck"] == false);
    CHECK(fields["sockets"].empty());
    CHECK(fields["option_slots"].empty());

    // A count above the socket slots (a corrupt packet) is held to the slots.
    item.SocketCount = 255;
    fields = App::Control::ItemIdentityFields(item);
    CHECK(fields["sockets"].size() == static_cast<std::size_t>(MAX_SOCKETS));
TEST_CASE("vault commands refuse bad arguments before reading the vault [core][control]")
{
    CHECK(IsBadRequest(Run(&App::Control::Commands::VaultMove, R"({"cmd":"vault-move"})")));
    CHECK(IsBadRequest(Run(&App::Control::Commands::VaultMove, R"({"cmd":"vault-move","from":"chest","slot":0})")));
    CHECK(IsBadRequest(Run(&App::Control::Commands::VaultMove, R"({"cmd":"vault-move","from":"vault","slot":-1})")));
    CHECK(IsBadRequest(
        Run(&App::Control::Commands::VaultMove, R"({"cmd":"vault-move","from":"vault","slot":0,"target_slot":-2})")));
    CHECK(IsBadRequest(
        Run(&App::Control::Commands::VaultZen, R"({"cmd":"vault-zen","direction":"take","amount":5})")));
    CHECK(IsBadRequest(
        Run(&App::Control::Commands::VaultZen, R"({"cmd":"vault-zen","direction":"deposit","amount":0})")));
    CHECK(IsBadRequest(Run(&App::Control::Commands::VaultZen, R"({"cmd":"vault-zen","direction":"deposit"})")));
}

TEST_CASE("vault commands answer not_open while the vault window is closed [core][control]")
{
    CHECK(Run(&App::Control::Commands::VaultState, R"({"cmd":"vault-state"})").find(R"("vault":null)") !=
          std::string::npos);
    CHECK(Run(&App::Control::Commands::VaultMove, R"({"cmd":"vault-move","from":"vault","slot":0})")
              .find(R"("error":"not_open")") != std::string::npos);
    CHECK(Run(&App::Control::Commands::VaultZen, R"({"cmd":"vault-zen","direction":"withdraw","amount":1})")
              .find(R"("error":"not_open")") != std::string::npos);
}
