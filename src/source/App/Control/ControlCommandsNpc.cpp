#include "stdafx.h"
#include "App/Control/ControlCommands.h"

#include "App/Control/ControlObjects.h"
#include "App/Control/ControlRules.h"
#include "App/Control/ControlState.h"
#include "App/Control/ControlTargets.h"
#include "Engine/AI/ZzzAI.h"
#include "Engine/Object/ZzzCharacter.h"
#include "Engine/Object/ZzzInterface.h"
#include "GameLogic/Automation/Movement.h"
#include "UI/Core/WindowSystem.h"
#include "UI/Dialogs/GenericConfirmDialog.h"
#include "UI/Dialogs/MessageBox.h"

#include "json.hpp"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using App::Control::Act;
using App::Control::ErrorCode;
using App::Control::Request;
using App::Control::Targets::ResolveTarget;
using App::Control::Targets::TargetError;
using nlohmann::json;
namespace Automation = GameLogic::Automation;

// How close (in tiles, either way) the hero must stand to talk once it has walked there.
constexpr int TalkReach = 1;
// The click has no range: it talks where its walk ends (ZzzInterface.cpp, MOVEMENT_TALK, and
// Action(c, o, true) when PathFinding2 finds no step). An NPC behind a counter leaves the hero
// on the nearest reachable tile, two tiles away, so the same reach applies there.
constexpr int TalkNearestReach = 2;
// Shortest gap between two walk packets (the server's speed check, as `move`).
constexpr std::chrono::milliseconds WalkRepeatInterval{300};
// The talk's whole run: the walk, then the answer the window waits for.
constexpr std::chrono::milliseconds TalkDeadline{15000};
// How long after the talk a window may open before the answer is `none`.
constexpr std::chrono::milliseconds TalkReplyWindow{2000};

// The NPC the last `npc-talk` talked to; -1 when none. Cleared as soon as the
// shop is seen closed, so a shop opened by a click later reports `npc: null`.
int s_talkedNpcKey = -1;

// The character a key names, while the hero is in the world and it is alive.
CHARACTER* FindNpc(int key)
{
    const int index = FindCharacterIndex(key);
    if (Hero == nullptr || index < 0 || index >= MAX_CHARACTERS_CLIENT)
    {
        return nullptr;
    }
    CHARACTER& character = CharactersClient[index];
    return character.Object.Live ? &character : nullptr;
}

// The tiles within TalkNearestReach of the NPC, each with the hero's path length to it (-1 when none).
// Tiles farther out could never be picked: the talk rule refuses them.
std::vector<App::Control::Rules::TalkTile> TalkCandidates(const CHARACTER& npc)
{
    std::vector<App::Control::Rules::TalkTile> candidates;
    for (int x = npc.PositionX - TalkNearestReach; x <= npc.PositionX + TalkNearestReach; ++x)
    {
        for (int y = npc.PositionY - TalkNearestReach; y <= npc.PositionY + TalkNearestReach; ++y)
        {
            if (x < 0 || y < 0 || x >= TERRAIN_SIZE || y >= TERRAIN_SIZE)
            {
                continue;
            }
            PATH_t path;
            const bool reachable = PathFinding2(Hero->PositionX, Hero->PositionY, x, y, &path);
            candidates.push_back({x, y, reachable ? static_cast<int>(path.PathNum) : -1});
        }
    }
    return candidates;
}

// The windows a talk can open, named as `ui` names them. The inventory is not one: it is open all the time.
constexpr App::Control::Rules::NamedWindow<DWORD> TalkWindows[] = {
    {"npc_shop", mu::ui::window::INTERFACE_NPCSHOP},
    {"npc_quest", mu::ui::window::INTERFACE_NPCQUEST},
    {"storage", mu::ui::window::INTERFACE_STORAGE},
    {"storage_extension", mu::ui::window::INTERFACE_STORAGE_EXT},
    {"mix", mu::ui::window::INTERFACE_MIXINVENTORY},
    {"lucky_item", mu::ui::window::INTERFACE_LUCKYITEMWND},
};

// The window a talk answers in, named as `ui` names it; empty while none is open.
std::string OpenWindowName()
{
    const std::string_view window = App::Control::Rules::FirstOpenWindow(
        TalkWindows, [](DWORD key) { return g_pNewUISystem->IsVisible(key); });
    if (!window.empty())
    {
        return std::string(window);
    }
    if (!g_MessageBox->IsEmpty() ||
        (mu::ui::window::g_pGenericConfirmDialog != nullptr && mu::ui::window::g_pGenericConfirmDialog->IsVisible()))
    {
        return "message_box";
    }
    return {};
}

// npc-talk: walks up to an NPC, then talks to it through the client's own
// talk (GameLogic::Npcs::Talk), so the packet and its rules are the click's.
class NpcTalkAct : public Act
{
public:
    explicit NpcTalkAct(int npcKey) : m_npcKey(npcKey) {}

    [[nodiscard]] std::string_view Name() const override
    {
        return "npc-talk";
    }
    [[nodiscard]] std::optional<std::chrono::milliseconds> Deadline() const override
    {
        return TalkDeadline;
    }

    [[nodiscard]] std::string ProgressObject() const override
    {
        json progress;
        progress["npc"] = m_npcKey;
        progress["position"] =
            Hero != nullptr ? json::array({Hero->PositionX, Hero->PositionY}) : json::array({-1, -1});
        return progress.dump();
    }

    [[nodiscard]] Status Tick(std::string& response) override
    {
        if (m_talked)
        {
            return TickReply(response);
        }

        CHARACTER* npc = FindNpc(m_npcKey);
        if (npc == nullptr)
        {
            response = App::Control::EncodeError(EncodedId(), ErrorCode::NotInView, "the NPC is no longer in view",
                                                 ProgressObject());
            return Status::Finished;
        }

        if (Automation::IsWalkInProgress())
        {
            return Status::Running;
        }

        const int dx = npc->PositionX - Hero->PositionX;
        const int dy = npc->PositionY - Hero->PositionY;
        if (App::Control::Rules::WithinTalkReach(dx, dy, TalkReach))
        {
            return Talk(*npc, response);
        }

        const auto now = std::chrono::steady_clock::now();
        if (m_lastWalkAt.time_since_epoch().count() != 0 && now - m_lastWalkAt < WalkRepeatInterval)
        {
            return Status::Running;
        }
        m_lastWalkAt = now;

        if (m_hasTalkTile)
        {
            // The NPC has no path: the hero walks to the nearest reachable tile picked for it.
            const Automation::MoveResult result = Automation::WalkTo(m_talkTileX, m_talkTileY);
            if (result == Automation::MoveResult::Arrived)
            {
                return Talk(*npc, response);
            }
            if (result == Automation::MoveResult::NoPath)
            {
                response = App::Control::EncodeError(EncodedId(), ErrorCode::NoPath,
                                                     "no tile within talk reach of the NPC", ProgressObject());
                return Status::Finished;
            }
            return Status::Running;
        }

        if (Automation::WalkTo(npc->PositionX, npc->PositionY) == Automation::MoveResult::NoPath)
        {
            // The click's own "no path" branch: it talks from where the hero stands.
            if (App::Control::Rules::WithinTalkReach(dx, dy, TalkNearestReach))
            {
                return Talk(*npc, response);
            }
            const std::optional<App::Control::Rules::TalkTile> tile = App::Control::Rules::PickTalkTile(
                npc->PositionX, npc->PositionY, TalkCandidates(*npc), TalkNearestReach);
            if (!tile)
            {
                response = App::Control::EncodeError(EncodedId(), ErrorCode::NoPath,
                                                     "no tile within talk reach of the NPC", ProgressObject());
                return Status::Finished;
            }
            m_hasTalkTile = true;
            m_talkTileX = tile->x;
            m_talkTileY = tile->y;
        }
        return Status::Running;
    }

private:
    // Talks from where the hero stands; the client stops the hero first (MOVEMENT_TALK).
    Status Talk(CHARACTER& npc, std::string& response)
    {
        SetPlayerStop(Hero);
        Hero->Movement = false;
        if (!TalkToNpc(npc))
        {
            response = App::Control::EncodeError(EncodedId(), ErrorCode::NotAllowed,
                                                 "the NPC does not talk to the hero yet (its talk rules refuse)",
                                                 ProgressObject());
            return Status::Finished;
        }
        m_talked = true;
        m_talkedAt = std::chrono::steady_clock::now();
        s_talkedNpcKey = m_npcKey;
        return Status::Running;
    }

    // Answers once a window the talk opened is seen, or with `none` after the reply window.
    Status TickReply(std::string& response) const
    {
        const std::string window = OpenWindowName();
        if (window.empty() && std::chrono::steady_clock::now() - m_talkedAt < TalkReplyWindow)
        {
            return Status::Running;
        }

        // Only the NPC shop this talk opened is the talk's shop.
        if (window != "npc_shop")
        {
            App::Control::ForgetShopNpc();
        }

        json result;
        result["npc"] = m_npcKey;
        result["window"] = window.empty() ? std::string("none") : window;
        result["talk"] = "sent";
        response = App::Control::EncodeResult(EncodedId(), result.dump());
        return Status::Finished;
    }

    int m_npcKey;
    bool m_talked = false;
    bool m_hasTalkTile = false;
    int m_talkTileX = 0;
    int m_talkTileY = 0;
    std::chrono::steady_clock::time_point m_talkedAt{};
    std::chrono::steady_clock::time_point m_lastWalkAt{};
};
} // namespace

namespace App::Control
{
void ForgetShopNpc()
{
    s_talkedNpcKey = -1;
}

json ShopNpcObject()
{
    if (s_talkedNpcKey < 0 || !g_pNewUISystem->IsVisible(mu::ui::window::INTERFACE_NPCSHOP))
    {
        return nullptr;
    }

    const Events::ObjectDescription npc = DescribeGameObject(s_talkedNpcKey);
    if (!npc.known)
    {
        return nullptr;
    }

    json described;
    described["id"] = npc.id;
    described["name"] = npc.name;
    described["kind"] = npc.kind;
    described["monster"] = npc.monster ? json(*npc.monster) : json(nullptr);
    described["position"] = json::array({npc.x, npc.y});
    return described;
}
} // namespace App::Control

namespace App::Control::Commands
{
// npc-list: the NPCs among the objects the client sees, as `nearby` reports them.
std::string NpcList(const Request& request, std::unique_ptr<Act>&)
{
    json npcs = json::array();
    for (const json& object : NearbyArray())
    {
        if (object.value("kind", std::string()) == "npc")
        {
            npcs.push_back(object);
        }
    }

    json result;
    result["npcs"] = npcs;
    return EncodeResult(request.EncodedId(), result.dump());
}

// shop-state: the open NPC shop, as `state.npc_shop` has it; null while none is open.
std::string ShopState(const Request& request, std::unique_ptr<Act>&)
{
    json result;
    result["npc_shop"] = NpcShopObject();
    return EncodeResult(request.EncodedId(), result.dump());
}

// shop-close: closes the open NPC shop as its close button does. Hiding the
// window sends the close packet (CSystem::Hide, WindowSystem.cpp:1236).
std::string ShopClose(const Request& request, std::unique_ptr<Act>&)
{
    if (!g_pNewUISystem->IsVisible(mu::ui::window::INTERFACE_NPCSHOP))
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotOpen, "no NPC shop is open");
    }

    // The client keeps a sale's shop open until the server has answered it.
    if (g_pNPCShop->IsSellingItem())
    {
        return EncodeError(request.EncodedId(), ErrorCode::Busy, "a sale is still waiting for the server");
    }

    g_pNewUISystem->Hide(mu::ui::window::INTERFACE_NPCSHOP);
    s_talkedNpcKey = -1;

    json result;
    result["closed"] = true;
    return EncodeResult(request.EncodedId(), result.dump());
}

// npc-talk: `target` is an NPC in view. The answer names the window the talk opened.
std::string NpcTalk(const Request& request, std::unique_ptr<Act>& act)
{
    int npcKey = -1;
    const std::string targetFailure = TargetError(request, ResolveTarget(request, npcKey), "npc-talk", true);
    if (!targetFailure.empty())
    {
        return targetFailure;
    }

    const CHARACTER* npc = FindNpc(npcKey);
    if (npc == nullptr)
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotInView, "the NPC is not in view");
    }
    if (ObjectKindName(npc->Object.Kind) != "npc")
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed, "the target is not an NPC");
    }

    // A talk answers in a window; one already open would be taken for the answer.
    const std::string open = OpenWindowName();
    if (!open.empty())
    {
        return EncodeError(request.EncodedId(), ErrorCode::Busy,
                           "`" + open + "` is open; close it before talking to an NPC");
    }

    App::Control::ForgetShopNpc();
    act = std::make_unique<NpcTalkAct>(npcKey);
    return {};
}
} // namespace App::Control::Commands
