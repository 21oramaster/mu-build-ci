// The vault (warehouse) family: `vault-state` reads it, `vault-move` puts an item in or takes one out, and
// `vault-zen` moves zen. Each sends the packet the client's own storage window sends. A locked vault is refused:
// its PIN is typed by the player in the client, and the control socket never sends or sees it.
#include "stdafx.h"
#include "App/Control/ControlCommands.h"

#include "App/Control/ControlRules.h"
#include "App/Control/ControlState.h"
#include "Engine/Object/ZzzInfomation.h"
#include "Engine/Object/ZzzInventory.h"
#include "GameLogic/Items/InventoryUtils.h"
#include "GameLogic/Items/TradeRestrictions.h"
#include "GameLogic/Items/ZenAmount.h"
#include "Network/Server/WSclient.h"
#include "UI/Core/WindowSystem.h"
#include "UI/Inventory/InventoryCtrl.h"
#include "UI/Inventory/MyInventory.h"
#include "UI/Inventory/StorageInventory.h"

#include "json.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace App::Control::Commands
{
namespace
{
using App::Control::Act;
using App::Control::ErrorCode;
using App::Control::Request;
using mu::ui::window::CInventoryCtrl;
using nlohmann::json;

// The zen move has no reply of its own: the server answers with the new vault zen (ReceiveStorageGold).
// A refused move changes nothing, so it ends in `timeout`.
constexpr std::chrono::milliseconds ZenDeadline{5000};

// Refusal for a command that needs the vault open and unlocked for this session, or empty when it may go on.
std::string VaultRefusal(const Request& request)
{
    if (!g_pNewUISystem->IsVisible(mu::ui::window::INTERFACE_STORAGE))
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotOpen,
                           "the vault is not open; talk to its keeper with mu_npc_talk first");
    }
    if (g_pStorageInventory->IsStorageLocked() && !g_pStorageInventory->IsCorrectPassword())
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                           "the vault is locked; the player unlocks it with its PIN in the client");
    }
    return {};
}

class VaultZenAct : public Act
{
public:
    VaultZenAct(bool deposit, int amount) : m_deposit(deposit), m_amount(amount) {}

    [[nodiscard]] std::string_view Name() const override
    {
        return "vault-zen";
    }
    [[nodiscard]] std::optional<std::chrono::milliseconds> Deadline() const override
    {
        return ZenDeadline;
    }

    [[nodiscard]] std::string ProgressObject() const override
    {
        json progress;
        progress["direction"] = m_deposit ? "deposit" : "withdraw";
        progress["amount"] = m_amount;
        progress["sent"] = m_sent;
        return progress.dump();
    }

    [[nodiscard]] Status Tick(std::string& response) override
    {
        if (!m_sent)
        {
            m_beforeVault = CharacterMachine->StorageGold;
            SocketClient->ToGameServer()->SendVaultMoveMoneyRequest(
                m_deposit ? VaultMoneyMoveDirection::InventoryToVault : VaultMoneyMoveDirection::VaultToInventory,
                static_cast<uint32_t>(m_amount));
            m_sent = true;
            return Status::Running;
        }
        if (CharacterMachine->StorageGold == m_beforeVault)
        {
            return Status::Running;
        }

        json moved;
        moved["direction"] = m_deposit ? "deposit" : "withdraw";
        moved["amount"] = m_amount;
        moved["zen"] = static_cast<int64_t>(CharacterMachine->Gold);
        moved["vault_zen"] = CharacterMachine->StorageGold;
        response = App::Control::EncodeResult(EncodedId(), moved.dump());
        return Status::Finished;
    }

private:
    bool m_deposit;
    int m_amount;
    bool m_sent = false;
    int m_beforeVault = 0;
};
} // namespace

std::string VaultState(const Request& request, std::unique_ptr<Act>&)
{
    json result;
    result["vault"] = App::Control::VaultObject();
    return EncodeResult(request.EncodedId(), result.dump());
}

// vault-move: `from` is "inventory" (main grid, `slot` as `state` numbers it) or "vault" (`slot` is the vault
// grid's linear position). The destination is `target_slot` when given, else the first free square.
// Answers as `equip` does: once the request is sent. Read `vault-state` to see it land.
std::string VaultMove(const Request& request, std::unique_ptr<Act>&)
{
    std::string from;
    int slot = -1;
    if (!request.GetString("from", from) || (from != "inventory" && from != "vault") || !request.GetInt("slot", slot) ||
        slot < 0)
    {
        return EncodeError(request.EncodedId(), ErrorCode::BadRequest,
                           "`vault-move` needs `from` (inventory or vault) and a `slot`, a whole number from 0 up");
    }
    int targetSlot = -1;
    const bool hasTarget = request.Has("target_slot");
    if (hasTarget && (!request.GetInt("target_slot", targetSlot) || targetSlot < 0))
    {
        return EncodeError(request.EncodedId(), ErrorCode::BadRequest,
                           "`target_slot` must be a whole number from 0 up");
    }

    if (std::string refusal = VaultRefusal(request); !refusal.empty())
    {
        return refusal;
    }
    if (CInventoryCtrl::GetPickedItem() != nullptr)
    {
        return EncodeError(request.EncodedId(), ErrorCode::Busy, "an item is already being moved");
    }

    const bool fromVault = from == "vault";
    if (!fromVault && !GameLogic::Items::IsMainInventorySlot(slot))
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                           "`vault-move` takes items from the main inventory, not the extension slots");
    }
    CInventoryCtrl* sourceCtrl = fromVault ? g_pStorageInventory->GetInventoryCtrl()
                                           : (g_pMyInventory != nullptr ? g_pMyInventory->GetInventoryCtrl() : nullptr);
    ITEM* moving = nullptr;
    if (sourceCtrl != nullptr)
    {
        moving = fromVault ? sourceCtrl->FindItem(slot) : g_pMyInventory->FindItem(slot);
    }
    if (moving == nullptr || moving->Type < 0)
    {
        return EncodeError(request.EncodedId(), ErrorCode::EmptySlot,
                           "`" + from + "` slot " + std::to_string(slot) + " is empty");
    }
    if (sourceCtrl->IsLocked())
    {
        return EncodeError(request.EncodedId(), ErrorCode::Busy, "the " + from + " is locked");
    }
    if (!fromVault && GameLogic::Items::IsStoreBan(moving))
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed, "this item cannot be stored in the vault");
    }

    // Lifted before the destination is asked, as `equip` does: a move onto the item's own squares is then not
    // refused for colliding with itself. Restored when the move cannot be sent.
    if (!CInventoryCtrl::CreatePickedItem(sourceCtrl, moving, true))
    {
        CInventoryCtrl::DeletePickedItem();
        return EncodeError(request.EncodedId(), ErrorCode::Failed, "the item could not be picked up");
    }
    sourceCtrl->RemoveItem(moving);
    mu::ui::window::CPickedItem* picked = CInventoryCtrl::GetPickedItem();
    ITEM* lifted = picked != nullptr ? picked->GetItem() : nullptr;
    auto restore = [&]
    {
        if (lifted != nullptr)
        {
            sourceCtrl->AddItem(lifted->x, lifted->y, lifted);
        }
        CInventoryCtrl::DeletePickedItem();
    };
    if (lifted == nullptr)
    {
        restore();
        return EncodeError(request.EncodedId(), ErrorCode::Failed, "the item could not be picked up");
    }

    CInventoryCtrl* destCtrl = fromVault ? (g_pMyInventory != nullptr ? g_pMyInventory->GetInventoryCtrl() : nullptr)
                                         : g_pStorageInventory->GetInventoryCtrl();
    if (destCtrl == nullptr)
    {
        restore();
        return EncodeError(request.EncodedId(), ErrorCode::Failed, "the destination is not available");
    }
    int destSlot = targetSlot;
    if (!hasTarget)
    {
        destSlot = fromVault ? g_pMyInventory->FindEmptySlot(lifted)
                             : destCtrl->FindEmptySlot(ItemAttribute[lifted->Type].Width, ItemAttribute[lifted->Type].Height);
    }
    // The main inventory numbers its squares from MAX_EQUIPMENT_INDEX, which CanMove takes as the slot; the vault
    // numbers its squares from 0.
    const bool inRange = fromVault
        ? (destSlot >= MAX_EQUIPMENT_INDEX && GameLogic::Items::IsMainInventorySlot(destSlot))
        : (destSlot >= 0 && destSlot < destCtrl->GetNumberOfColumn() * destCtrl->GetNumberOfRow());
    if (!inRange || !destCtrl->CanMove(destSlot, lifted))
    {
        restore();
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                           "the item does not fit at slot " + std::to_string(destSlot));
    }

    // The client's own pair of packets: the reply handler completes the move from the lifted item.
    const bool sent = fromVault
        ? SendRequestEquipmentItem(STORAGE_TYPE::VAULT, slot, lifted, STORAGE_TYPE::INVENTORY, destSlot)
        : SendRequestEquipmentItem(STORAGE_TYPE::INVENTORY, slot, lifted, STORAGE_TYPE::VAULT, destSlot);
    if (!sent)
    {
        restore();
        return EncodeError(request.EncodedId(), ErrorCode::Busy, "another item move has not been answered yet");
    }

    json result;
    result["from"] = from;
    result["slot"] = slot;
    result["target_slot"] = destSlot;
    return EncodeResult(request.EncodedId(), result.dump());
}

// vault-zen: `direction` is "deposit" (inventory to vault) or "withdraw"; `amount` is limited the way the client's
// zen dialogs limit it (GameLogic::Items::MaxVaultDeposit / MaxVaultWithdraw).
std::string VaultZen(const Request& request, std::unique_ptr<Act>& act)
{
    std::string direction;
    int amount = 0;
    if (!request.GetString("direction", direction) || (direction != "deposit" && direction != "withdraw") ||
        !request.GetInt("amount", amount) || amount < 1)
    {
        return EncodeError(request.EncodedId(), ErrorCode::BadRequest,
                           "`vault-zen` needs `direction` (deposit or withdraw) and an `amount` of 1 or more");
    }
    if (std::string refusal = VaultRefusal(request); !refusal.empty())
    {
        return refusal;
    }

    const bool deposit = direction == "deposit";
    const int64_t gold = CharacterMachine->Gold;
    const int64_t vaultZen = CharacterMachine->StorageGold;
    const int64_t most = deposit ? GameLogic::Items::MaxVaultDeposit(gold, vaultZen)
                                 : GameLogic::Items::MaxVaultWithdraw(vaultZen, gold);
    if (amount > most)
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                           "at most " + std::to_string(most) + " zen can be " +
                               (deposit ? "deposited" : "withdrawn") + " now");
    }

    act = std::make_unique<VaultZenAct>(deposit, amount);
    return {};
}
} // namespace App::Control::Commands
