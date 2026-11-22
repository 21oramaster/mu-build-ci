#include "stdafx.h"

#include "doctest.h"

#include "App/Control/ControlCommands.h"
#include "App/Control/ControlProtocol.h"

#include <memory>
#include <string>

namespace
{
using Command = std::string (*)(const App::Control::Request&, std::unique_ptr<App::Control::Act>&);

std::string Run(Command command, const std::string& raw)
{
    std::unique_ptr<App::Control::Act> act;
    return command(App::Control::Request::Parse(raw), act);
}
} // namespace

TEST_CASE("npc-list answers an npcs array when no object is in view [core][control]")
{
    // The tests run no world: nothing is alive, so no NPC is listed.
    const std::string response = Run(&App::Control::Commands::NpcList, R"({"cmd":"npc-list"})");
    CHECK(response.find(R"("npcs":[])") != std::string::npos);
}

TEST_CASE("shop-state answers null while no NPC shop is open [core][control]")
{
    const std::string response = Run(&App::Control::Commands::ShopState, R"({"cmd":"shop-state"})");
    CHECK(response.find(R"("npc_shop":null)") != std::string::npos);
}

TEST_CASE("shop-close refuses with not_open while no NPC shop is open [core][control]")
{
    const std::string response = Run(&App::Control::Commands::ShopClose, R"({"cmd":"shop-close"})");
    CHECK(response.find(R"("error":"not_open")") != std::string::npos);
}

TEST_CASE("npc-talk needs a target and refuses one not in view [core][control]")
{
    const std::string missing = Run(&App::Control::Commands::NpcTalk, R"({"cmd":"npc-talk"})");
    CHECK(missing.find(R"("error":"bad_request")") != std::string::npos);

    // The tests run no world: no object has this id.
    const std::string absent = Run(&App::Control::Commands::NpcTalk, R"({"cmd":"npc-talk","target":4242})");
    CHECK(absent.find(R"("error":"not_in_view")") != std::string::npos);
}

TEST_CASE("shop-sell validates its inventory slot and confirm before it looks at the shop [core][control]")
{
    const auto sell = [](const std::string& fields)
    {
        return Run(&App::Control::Commands::ShopSell, R"({"cmd":"shop-sell",)" + fields + "}");
    };
    CHECK(sell(R"("inventory_slot":-1)").find(R"("error":"bad_request")") != std::string::npos);
    CHECK(sell(R"("inventory_slot":0)").find(R"("error":"bad_request")") != std::string::npos);
    CHECK(sell(R"("inventory_slot":"12")").find(R"("error":"bad_request")") != std::string::npos);
    CHECK(sell(R"("inventory_slot":12,"confirm":"yes")").find(R"("error":"bad_request")") != std::string::npos);
    CHECK(Run(&App::Control::Commands::ShopSell, R"({"cmd":"shop-sell"})").find(R"("error":"bad_request")") !=
          std::string::npos);
    // A valid slot with no shop open: nothing is sent.
    CHECK(sell(R"("inventory_slot":12)").find(R"("error":"not_open")") != std::string::npos);
}

TEST_CASE("repair validates its target and mode before it looks at the place [core][control]")
{
    const auto repair = [](const std::string& fields)
    {
        return Run(&App::Control::Commands::Repair, R"({"cmd":"repair",)" + fields + "}");
    };
    CHECK(Run(&App::Control::Commands::Repair, R"({"cmd":"repair"})").find(R"("error":"bad_request")") !=
          std::string::npos);
    CHECK(repair(R"("slot":-1)").find(R"("error":"bad_request")") != std::string::npos);
    CHECK(repair(R"("all":"yes")").find(R"("error":"bad_request")") != std::string::npos);
    CHECK(repair(R"("all":true,"slot":12)").find(R"("error":"bad_request")") != std::string::npos);
    CHECK(repair(R"("slot":12,"mode":"both")").find(R"("error":"bad_request")") != std::string::npos);
    CHECK(repair(R"("all":true,"mode":"self")").find(R"("error":"bad_request")") != std::string::npos);
    // No repair place is open: nothing is sent.
    CHECK(repair(R"("all":true)").find(R"("error":"not_open")") != std::string::npos);
    // The tests load no inventory, so the slot is empty.
    CHECK(repair(R"("slot":12)").find(R"("error":"empty_slot")") != std::string::npos);
}

TEST_CASE("stat-add validates its stat and points before it looks at the points [core][control]")
{
    const auto add = [](const std::string& fields)
    {
        return Run(&App::Control::Commands::StatAdd, R"({"cmd":"stat-add",)" + fields + "}");
    };
    CHECK(Run(&App::Control::Commands::StatAdd, R"({"cmd":"stat-add"})").find(R"("error":"bad_request")") !=
          std::string::npos);
    CHECK(add(R"("stat":"wisdom")").find(R"("error":"bad_request")") != std::string::npos);
    CHECK(add(R"("stat":"strength","points":0)").find(R"("error":"bad_request")") != std::string::npos);
    CHECK(add(R"("stat":"strength","points":1.5)").find(R"("error":"bad_request")") != std::string::npos);
    // The tests run no world: no character is there to add to.
    CHECK(add(R"("stat":"strength")").find(R"("error":"wrong_scene")") != std::string::npos);
}

TEST_CASE("shop-buy validates its goods slot before it looks at the shop [core][control]")
{
    const auto buy = [](const std::string& fields)
    {
        return Run(&App::Control::Commands::ShopBuy, R"({"cmd":"shop-buy",)" + fields + "}");
    };
    CHECK(buy(R"("slot":-1)").find(R"("error":"bad_request")") != std::string::npos);
    CHECK(buy(R"("slot":"0")").find(R"("error":"bad_request")") != std::string::npos);
    CHECK(buy(R"("slot":1.5)").find(R"("error":"bad_request")") != std::string::npos);
    CHECK(Run(&App::Control::Commands::ShopBuy, R"({"cmd":"shop-buy"})").find(R"("error":"bad_request")") !=
          std::string::npos);
    // A valid slot with no shop open: nothing is sent.
    CHECK(buy(R"("slot":0)").find(R"("error":"not_open")") != std::string::npos);
}
