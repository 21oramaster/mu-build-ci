// What the control socket reports about the character and its surroundings.
//
// Reading the engine's tables lives here, apart from the command handlers, so
// `state`, `nearby` and the event recorders share one description of the world.
#pragma once

#include "App/Control/ControlRules.h"
#include "json.hpp"

#include <string>
#include <vector>

namespace App::Control
{
// Encoded JSON object with every field the `state` command promises for a
// character standing in the world.
[[nodiscard]] std::string WorldStateObject();

// The open NPC shop as `state.npc_shop` reports it; null while no shop is open.
[[nodiscard]] nlohmann::json NpcShopObject();

// The NPC the open shop was opened from by `npc-talk`, as {id, name, kind,
// monster, position}; null when unknown or no shop is open.
[[nodiscard]] nlohmann::json ShopNpcObject();
// Drops the remembered talk NPC: the shop it opened is closed, or another talk started.
void ForgetShopNpc();

// The inventory's items as pickup compares them before and after a pickup.
[[nodiscard]] std::vector<Rules::InventoryEntry> InventoryEntries();

// JSON array of the objects the client currently knows: players, monsters,
// NPCs and dropped items.
[[nodiscard]] nlohmann::json NearbyArray();
} // namespace App::Control
