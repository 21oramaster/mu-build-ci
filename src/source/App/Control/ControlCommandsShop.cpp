#include "stdafx.h"
#include "App/Control/ControlCommands.h"
#include "App/Control/ControlRules.h"

#include "Engine/Object/ZzzInfomation.h"
#include "Engine/Object/ZzzInventory.h"
#include "GameLogic/Items/IInventoryActionContext.h"
#include "GameLogic/Items/InventoryUtils.h"
#include "GameLogic/Items/ItemCategories.h"
#include "GameLogic/Items/ShopRestrictions.h"
#include "GameLogic/Social/GambleSystem.h"
#include "Network/Server/WSclient.h"
#include "UI/Core/WindowSystem.h"
#include "UI/Inventory/InventoryContents.h"
#include "UI/Inventory/InventoryCtrl.h"
#include "UI/Inventory/MyInventory.h"
#include "UI/NPCs/NPCShop.h"

#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The client's latch for a purchase in flight (WSclient.cpp, ReceiveBuyExtended).
extern int BuyCost;

namespace
{
using App::Control::Act;
using App::Control::ErrorCode;
using App::Control::Request;
using nlohmann::json;
namespace Rules = App::Control::Rules;

// A purchase the server has not answered yet, from the send to the 0x32 reply.
constexpr std::chrono::milliseconds BuyDeadline{5000};
// A sale the server has not answered yet: the item leaves the hand on the answer.
constexpr std::chrono::milliseconds SellDeadline{5000};
// A repair the server has not confirmed (its answer brings the zen, a refusal none).
constexpr std::chrono::milliseconds RepairDeadline{5000};
// The slot the server reads as "every item" in a repair at an NPC.
constexpr BYTE RepairAllSlot = 0xFF;
// Room for the repair price's text, as ConvertRepairGold writes it.
constexpr std::size_t RepairTextLength = 100;

bool IsNpcShopOpen()
{
    return g_pNewUISystem->IsVisible(mu::ui::window::INTERFACE_NPCSHOP);
}

// The goods of the open shop at a slot (y * columns + x, NPCShop.cpp:157); null for an empty one.
ITEM* ShopGoods(int slot)
{
    mu::ui::window::CInventoryCtrl* grid = g_pNPCShop->GetInventoryCtrl();
    for (int i = 0; grid != nullptr && i < static_cast<int>(grid->GetNumberOfItems()); ++i)
    {
        ITEM* item = grid->GetItem(i);
        if (item != nullptr && item->y * grid->GetNumberOfColumn() + item->x == slot)
        {
            return item;
        }
    }
    return nullptr;
}

// The hero's zen; 0 while no character is in the world (the tests run none).
int64_t HeroZen()
{
    return CharacterMachine != nullptr ? static_cast<int64_t>(CharacterMachine->Gold) : 0;
}

// The player's inventory slots that hold an item.
std::vector<Rules::SlotState> InventorySnapshot()
{
    std::vector<Rules::SlotState> slots;
    for (int slot = MAX_EQUIPMENT_INDEX; slot < MAX_MY_INVENTORY_EX_INDEX; ++slot)
    {
        const ITEM* item = UI::Inventory::FindPlayerItem(slot);
        if (item != nullptr && item->Type >= 0)
        {
            slots.push_back({slot, item->Type, item->Durability});
        }
    }
    return slots;
}

// Whether the open NPC shop repairs.
bool IsNpcRepairOpen()
{
    return IsNpcShopOpen() && g_pNPCShop->IsRepairShop();
}

// repair: the repair packet of a click (InventoryActionController.cpp) or of the NPC's
// "repair all" (NPCShop.cpp). The answer (0x34) brings the zen only; the durability
// follows in the item update, so success is read from the zen.
class RepairAct : public Act
{
public:
    // `slot` is the inventory slot, or -1 for "repair all" at the NPC.
    RepairAct(int slot, bool selfRepair) : m_slot(slot), m_selfRepair(selfRepair) {}

    [[nodiscard]] std::string_view Name() const override
    {
        return "repair";
    }
    // The timeout is `failed`, not `timeout`, so the act keeps its own clock.
    [[nodiscard]] std::optional<std::chrono::milliseconds> Deadline() const override
    {
        return std::nullopt;
    }

    [[nodiscard]] std::string ProgressObject() const override
    {
        json progress;
        progress["slot"] = m_slot < 0 ? json(nullptr) : json(m_slot);
        progress["sent"] = m_sent;
        return progress.dump();
    }

    [[nodiscard]] Status Tick(std::string& response) override
    {
        if (!m_sent)
        {
            // Checked again: the shop or the repair mode may have changed since the command was taken.
            const bool ready = m_selfRepair ? g_pMyInventory->GetRepairMode() == SEASON3B::REPAIR_MODE_ON
                                            : IsNpcShopOpen() && g_pNPCShop->IsRepairShop();
            if (!ready)
            {
                response = App::Control::EncodeError(EncodedId(), ErrorCode::NotOpen,
                                                     "the repair place closed before the repair", ProgressObject());
                return Status::Finished;
            }

            m_zenBefore = static_cast<int64_t>(CharacterMachine->Gold);
            m_sentAt = std::chrono::steady_clock::now();
            const BYTE slot = m_slot < 0 ? RepairAllSlot : static_cast<BYTE>(m_slot);
            SocketClient->ToGameServer()->SendRepairItemRequest(slot, m_selfRepair ? 1 : 0);
            m_sent = true;
            return Status::Running;
        }

        const int64_t zen = static_cast<int64_t>(CharacterMachine->Gold);
        if (zen != m_zenBefore)
        {
            json repaired;
            if (m_slot < 0)
            {
                repaired["all"] = true;
            }
            else
            {
                repaired["inventory_slot"] = m_slot;
            }
            repaired["zen"] = zen;
            response = App::Control::EncodeResult(EncodedId(), repaired.dump());
            return Status::Finished;
        }

        // Gold 0 in the answer means nothing was repaired, and so does no answer at all.
        if (std::chrono::steady_clock::now() - m_sentAt < RepairDeadline)
        {
            return Status::Running;
        }
        response = App::Control::EncodeError(EncodedId(), ErrorCode::Failed,
                                             "no repair was confirmed by the server", ProgressObject());
        return Status::Finished;
    }

private:
    int m_slot;
    bool m_selfRepair;
    bool m_sent = false;
    int64_t m_zenBefore = 0;
    std::chrono::steady_clock::time_point m_sentAt{};
};

// shop-sell: sends an inventory slot to the open shop, as a click on the item does.
class ShopSellAct : public Act
{
public:
    explicit ShopSellAct(int inventorySlot) : m_inventorySlot(inventorySlot) {}

    [[nodiscard]] std::string_view Name() const override
    {
        return "shop-sell";
    }
    [[nodiscard]] std::optional<std::chrono::milliseconds> Deadline() const override
    {
        return SellDeadline;
    }

    [[nodiscard]] std::string ProgressObject() const override
    {
        json progress;
        progress["inventory_slot"] = m_inventorySlot;
        progress["sent"] = m_sent;
        return progress.dump();
    }

    [[nodiscard]] Status Tick(std::string& response) override
    {
        if (!m_sent)
        {
            if (!IsNpcShopOpen())
            {
                response = App::Control::EncodeError(EncodedId(), ErrorCode::NotOpen,
                                                     "the NPC shop closed before the sale", ProgressObject());
                return Status::Finished;
            }
            if (g_pNPCShop->IsSellingItem())
            {
                response = App::Control::EncodeError(EncodedId(), ErrorCode::Busy,
                                                     "a sale is still waiting for the server", ProgressObject());
                return Status::Finished;
            }

            // The client takes the item out of its grid before it sends the sale (InventoryActionController.cpp):
            // the answer then discards it, or restores it on a refusal.
            mu::ui::window::CInventoryCtrl* grid = g_pMyInventory->GetInventoryCtrl();
            ITEM* item = UI::Inventory::FindPlayerItem(m_inventorySlot);
            if (grid == nullptr || item == nullptr || !mu::ui::window::CInventoryCtrl::CreatePickedItem(grid, item))
            {
                response = App::Control::EncodeError(EncodedId(), ErrorCode::Failed,
                                                     "the item could not be lifted for the sale", ProgressObject());
                return Status::Finished;
            }
            grid->RemoveItem(item);
            if (auto* picked = mu::ui::window::CInventoryCtrl::GetPickedItem())
            {
                picked->HidePickedItem();
            }

            m_zenBefore = static_cast<int64_t>(CharacterMachine->Gold);
            SocketClient->ToGameServer()->SendSellItemToNpcRequest(static_cast<BYTE>(m_inventorySlot));
            g_pNPCShop->SetSellingItem(true);
            m_sent = true;
            return Status::Running;
        }

        // The answer (0x33, ReceiveSell) clears the sale latch, bought or refused alike.
        if (g_pNPCShop->IsSellingItem())
        {
            return Status::Running;
        }

        const int64_t zen = static_cast<int64_t>(CharacterMachine->Gold);
        if (zen > m_zenBefore)
        {
            json sold;
            sold["inventory_slot"] = m_inventorySlot;
            sold["zen"] = zen;
            response = App::Control::EncodeResult(EncodedId(), sold.dump());
            return Status::Finished;
        }

        // A refusal brings no zen. 0xFF restores the item already; 0xFE does not, so it is put back here.
        if (mu::ui::window::CInventoryCtrl::GetPickedItem() != nullptr)
        {
            UI::Inventory::RestorePickedItem();
        }
        response = App::Control::EncodeError(EncodedId(), ErrorCode::Failed, "the server refused the sale",
                                             ProgressObject());
        return Status::Finished;
    }

private:
    int m_inventorySlot;
    bool m_sent = false;
    int64_t m_zenBefore = 0;
};

// shop-buy: sends the purchase as a click on the goods does, under the BuyCost latch.
class ShopBuyAct : public Act
{
public:
    ShopBuyAct(int goodsSlot, int64_t cost) : m_goodsSlot(goodsSlot), m_cost(cost) {}

    [[nodiscard]] std::string_view Name() const override
    {
        return "shop-buy";
    }
    [[nodiscard]] std::optional<std::chrono::milliseconds> Deadline() const override
    {
        return BuyDeadline;
    }

    [[nodiscard]] std::string ProgressObject() const override
    {
        json progress;
        progress["slot"] = m_goodsSlot;
        progress["sent"] = m_sent;
        return progress.dump();
    }

    [[nodiscard]] Status Tick(std::string& response) override
    {
        if (!m_sent)
        {
            // Checked again: the shop or the latch may have changed since the command was taken.
            if (!IsNpcShopOpen())
            {
                response = App::Control::EncodeError(EncodedId(), ErrorCode::NotOpen,
                                                     "the NPC shop closed before the purchase", ProgressObject());
                return Status::Finished;
            }
            if (BuyCost != 0)
            {
                response = App::Control::EncodeError(EncodedId(), ErrorCode::Busy,
                                                     "a purchase is still waiting for the server", ProgressObject());
                return Status::Finished;
            }

            m_before = InventorySnapshot();
            SocketClient->ToGameServer()->SendBuyItemFromNpcRequest(static_cast<BYTE>(m_goodsSlot));
            BuyCost = static_cast<int>(m_cost);
            m_sent = true;
            return Status::Running;
        }

        // The answer (0x32, ReceiveBuyExtended) clears BuyCost, bought or refused alike.
        if (BuyCost != 0)
        {
            return Status::Running;
        }

        if (const std::optional<int> slot = Rules::ChangedSlot(m_before, InventorySnapshot()))
        {
            json bought;
            bought["slot"] = m_goodsSlot;
            bought["inventory_slot"] = *slot;
            bought["zen"] = static_cast<int64_t>(CharacterMachine->Gold);
            response = App::Control::EncodeResult(EncodedId(), bought.dump());
            return Status::Finished;
        }

        // No item arrived: the server refused (0xFE also closes the shop; 0xFF is silent).
        response = App::Control::EncodeError(EncodedId(), ErrorCode::Failed, "the server refused the purchase",
                                             ProgressObject());
        return Status::Finished;
    }

private:
    int m_goodsSlot;
    int64_t m_cost;
    bool m_sent = false;
    std::vector<Rules::SlotState> m_before;
};
} // namespace

namespace
{
// repair at an NPC that repairs: every item at once.
std::string RepairAllAtNpc(const Request& request, std::unique_ptr<Act>& act, const std::string& mode)
{
    if (mode == "self")
    {
        return App::Control::EncodeError(request.EncodedId(), ErrorCode::BadRequest, "`all` repairs at an NPC, not `self`");
    }
    if (!IsNpcRepairOpen())
    {
        return App::Control::EncodeError(request.EncodedId(), ErrorCode::NotOpen, "no NPC repair shop is open");
    }
    const int64_t price = AllRepairGold;
    if (price <= 0)
    {
        return App::Control::EncodeError(request.EncodedId(), ErrorCode::NotAllowed, "nothing needs repairing");
    }
    if (HeroZen() < price)
    {
        return App::Control::EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                           "not enough zen: repair all costs " + std::to_string(price) + ", the hero has " +
                               std::to_string(HeroZen()));
    }
    act = std::make_unique<RepairAct>(-1, false);
    return {};
}

// repair of one inventory slot, at the NPC or in the inventory's repair mode.
std::string RepairOne(const Request& request, std::unique_ptr<Act>& act, int slot, const std::string& mode)
{
    const ITEM* item = UI::Inventory::FindPlayerItem(slot);
    if (item == nullptr || item->Type < 0)
    {
        return App::Control::EncodeError(request.EncodedId(), ErrorCode::EmptySlot,
                           "inventory slot " + std::to_string(slot) + " is empty");
    }
    if (GameLogic::Items::IsRepairBan(item))
    {
        return App::Control::EncodeError(request.EncodedId(), ErrorCode::NotAllowed, "this item cannot be repaired");
    }
    const int maxDurability = CalcMaxDurability(item, &ItemAttribute[item->Type], item->Level);
    if (item->Durability >= maxDurability)
    {
        return App::Control::EncodeError(request.EncodedId(), ErrorCode::NotAllowed, "the item is at full durability");
    }
    const int64_t value = ItemValue(item, 2);
    if (value < 0)
    {
        return App::Control::EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                           "the item has no repair price yet (its data has not arrived)");
    }

    const bool selfRepair = mode == "self" || (mode.empty() && !IsNpcRepairOpen());
    if (selfRepair)
    {
        if (g_pMyInventory->GetRepairMode() != SEASON3B::REPAIR_MODE_ON)
        {
            return App::Control::EncodeError(request.EncodedId(), ErrorCode::NotOpen,
                               "the inventory is not in repair mode (the L key turns it on)");
        }
    }
    else if (!IsNpcRepairOpen())
    {
        return App::Control::EncodeError(request.EncodedId(), ErrorCode::NotOpen, "no NPC repair shop is open");
    }

    // The price the client's own tooltip shows for this place (ControlState.cpp, AddRepairPrice).
    wchar_t text[RepairTextLength] = {};
    const int64_t cost = ConvertRepairGold(value, item->Durability, maxDurability, item->Type, text);
    // A zero price repairs nothing and the answer carries no zen, so it would only time out.
    if (cost <= 0)
    {
        return App::Control::EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                                         "this repair costs nothing, so the client does not do it");
    }
    if (HeroZen() < cost)
    {
        return App::Control::EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                           "not enough zen: the repair costs " + std::to_string(cost) + ", the hero has " +
                               std::to_string(HeroZen()));
    }

    act = std::make_unique<RepairAct>(slot, selfRepair);
    return {};
}
} // namespace

namespace App::Control::Commands
{
// shop-buy: `slot` is a goods slot of the open NPC shop, as `state.npc_shop` numbers its items.
std::string ShopBuy(const Request& request, std::unique_ptr<Act>& act)
{
    int slot = -1;
    if (!request.GetInt("slot", slot) || slot < 0)
    {
        return EncodeError(request.EncodedId(), ErrorCode::BadRequest,
                           "`shop-buy` needs a goods `slot`, a whole number from 0 up");
    }

    if (!IsNpcShopOpen())
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotOpen, "no NPC shop is open");
    }

    // A gamble shop asks for a confirmation with its own item index (NPCShop.cpp:158-183): not driven here.
    if (GambleSystem::Instance().IsGambleShop())
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                           "this is a gamble shop; its purchase needs a confirmation the control socket does not give");
    }

    if (BuyCost != 0)
    {
        return EncodeError(request.EncodedId(), ErrorCode::Busy, "a purchase is still waiting for the server");
    }

    const ITEM* goods = ShopGoods(slot);
    if (goods == nullptr)
    {
        return EncodeError(request.EncodedId(), ErrorCode::EmptySlot,
                           "shop slot " + std::to_string(slot) + " has no goods");
    }

    const int64_t cost = ItemValue(goods, 0);
    if (!Rules::IsLatchablePrice(cost))
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                           "this item has no price the client's purchase can hold");
    }
    const int64_t price = Rules::ShownPrice(cost, g_pNPCShop->GetTaxRate());
    if (HeroZen() < price)
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                           "not enough zen: the item costs " + std::to_string(price) + ", the hero has " +
                               std::to_string(HeroZen()));
    }

    act = std::make_unique<ShopBuyAct>(slot, cost);
    return {};
}

// shop-sell: `inventory_slot` is the player's inventory slot of the item. A high-value item
// asks the player first in the client; `confirm: true` is the answer to that question.
std::string ShopSell(const Request& request, std::unique_ptr<Act>& act)
{
    int slot = -1;
    if (!request.GetInt("inventory_slot", slot) || slot < MAX_EQUIPMENT_INDEX || slot >= MAX_MY_INVENTORY_EX_INDEX)
    {
        return EncodeError(request.EncodedId(), ErrorCode::BadRequest,
                           "`shop-sell` needs an `inventory_slot` of the player's inventory");
    }

    bool confirm = false;
    if (request.Has("confirm") && !request.GetStrictBool("confirm", confirm))
    {
        return EncodeError(request.EncodedId(), ErrorCode::BadRequest, "`confirm` is true or false");
    }

    if (!IsNpcShopOpen())
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotOpen, "no NPC shop is open");
    }

    if (g_pNPCShop->IsSellingItem())
    {
        return EncodeError(request.EncodedId(), ErrorCode::Busy, "a sale is still waiting for the server");
    }

    // The sale's answer (ReceiveSell) discards whatever the hand holds: refuse while an item is held.
    if (mu::ui::window::CInventoryCtrl::GetPickedItem() != nullptr)
    {
        return EncodeError(request.EncodedId(), ErrorCode::Busy, "an item is held in the hand; put it down first");
    }

    const ITEM* item = UI::Inventory::FindPlayerItem(slot);
    if (item == nullptr || item->Type < 0)
    {
        return EncodeError(request.EncodedId(), ErrorCode::EmptySlot,
                           "inventory slot " + std::to_string(slot) + " is empty");
    }

    if (GameLogic::Items::IsSellingBan(item))
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed, "the shop does not buy this item");
    }

    // The client's pick-up of an extension slot is not reachable from here: only the main inventory sells.
    if (!IsMainInventorySlot(slot))
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                           "only the main inventory can be sold from by the control socket");
    }

    if (!confirm && GameLogic::Items::IsHighValueItem(item))
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                           "an expensive item: the client asks before it is sold; send `confirm: true` to sell it");
    }

    act = std::make_unique<ShopSellAct>(slot);
    return {};
}

// repair: `slot` (an inventory slot) or `all: true` (at an NPC that repairs). `mode` is "npc" or "self";
// by default the NPC when its repair shop is open, else the inventory's repair mode.
std::string Repair(const Request& request, std::unique_ptr<Act>& act)
{
    bool all = false;
    if (request.Has("all") && !request.GetStrictBool("all", all))
    {
        return EncodeError(request.EncodedId(), ErrorCode::BadRequest, "`all` is true or false");
    }

    int slot = -1;
    if (all)
    {
        if (request.Has("slot"))
        {
            return EncodeError(request.EncodedId(), ErrorCode::BadRequest, "give a `slot` or `all`, not both");
        }
    }
    else if (!request.GetInt("slot", slot) || slot < MAX_EQUIPMENT_INDEX || slot >= MAX_MY_INVENTORY_EX_INDEX)
    {
        return EncodeError(request.EncodedId(), ErrorCode::BadRequest,
                           "`repair` needs a `slot` of the player's inventory, or `all: true`");
    }

    std::string mode;
    if (request.Has("mode") && (!request.GetString("mode", mode) || (mode != "npc" && mode != "self")))
    {
        return EncodeError(request.EncodedId(), ErrorCode::BadRequest, "`mode` is \"npc\" or \"self\"");
    }

    return all ? RepairAllAtNpc(request, act, mode) : RepairOne(request, act, slot, mode);
}

} // namespace App::Control::Commands
