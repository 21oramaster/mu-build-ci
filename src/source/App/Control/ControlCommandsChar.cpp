// Character select: char-list, char-create and char-delete. Each one sends the client's own
// packet (SendCreateCharacter, SendDeleteCharacter) the way its window does, and answers from the
// reply the client's handler records (ReceiveCreateCharacter, ReceiveDeleteCharacter), so the
// server's rules decide. The checks that need no game state are in ControlRules.h.
#include "stdafx.h"
#include "App/Control/ControlCommands.h"
#include "App/Control/ControlRules.h"

#include "Character/CharMakeWin.h"
#include "Character/CharacterManager.h"
#include "Core/Text/Utf8.h"
#include "Dotnet/PacketFunctions_CommonEnums.h"
#include "Engine/Object/ZzzCharacter.h"
#include "Guild/GuildTypes.h"
#include "Network/Server/WSclient.h"
#include "Scenes/CharacterScene.h"
#include "Scenes/SceneCommon.h"
#include "Scenes/SceneCore.h"
#include "UI/Core/SceneUICoordinator.h"
#include "UI/Windows/MsgWin.h"

#include "json.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace
{
using App::Control::Act;
using App::Control::EncodeError;
using App::Control::EncodeResult;
using App::Control::ErrorCode;
using App::Control::Request;
using nlohmann::json;
namespace Rules = App::Control::Rules;

// The server answers in a moment; a minute covers a slow stack, as for login.
constexpr std::chrono::milliseconds CharCreateDeadline{30000};
constexpr std::chrono::milliseconds CharDeleteDeadline{30000};

// A slot holds a character when the client has a live one there with a name.
bool Occupied(int slot)
{
    const CHARACTER& character = CharactersClient[slot];
    return character.Object.Live != 0 && character.ID[0] != L'\0';
}

int FirstFreeSlot()
{
    for (int slot = 0; slot < MAX_CHARACTERS_PER_ACCOUNT; ++slot)
    {
        if (!Occupied(slot))
        {
            return slot;
        }
    }
    return -1;
}

std::array<bool, Rules::ClassCardCount> ClassCards()
{
    return {g_CharCardEnable.bCharacterEnable[0], g_CharCardEnable.bCharacterEnable[1],
            g_CharCardEnable.bCharacterEnable[2]};
}

// One character as char-list and the answers report it. Slots count from 1, as select-char's do.
json CharacterSummary(int slot)
{
    const CHARACTER& character = CharactersClient[slot];
    json summary;
    summary["slot"] = slot + 1;
    summary["name"] = Core::Text::ToUtf8(character.ID);
    summary["class"] = std::string(Rules::ClassCodeName(static_cast<int>(character.Class)));
    summary["class_id"] = static_cast<int>(character.Class);
    summary["class_name"] = Core::Text::ToUtf8(gCharacterManager.GetCharacterClassText(character.Class));
    summary["level"] = character.Level;
    summary["guild_member"] = character.GuildStatus != G_NONE;
    summary["item_blocked"] = (character.CtlCode & (CTLCODE_02BLOCKITEM | CTLCODE_10ACCOUNT_BLOCKITEM)) != 0;
    return summary;
}

json CharacterList()
{
    json characters = json::array();
    for (int slot = 0; slot < MAX_CHARACTERS_PER_ACCOUNT; ++slot)
    {
        if (Occupied(slot))
        {
            characters.push_back(CharacterSummary(slot));
        }
    }
    return characters;
}

constexpr std::string_view ClassNamesHint =
    "DarkWizard, DarkKnight, FairyElf, MagicGladiator, DarkLord, Summoner, RageFighter, or 0 to 6";

// char-list: the account's characters, and what the client lets it create now.
std::string CharList(const Request& request, std::unique_ptr<Act>&)
{
    if (App::Control::Commands::CurrentSceneName() != "character_list")
    {
        return EncodeError(request.EncodedId(), ErrorCode::WrongScene, "char-list needs the character select screen");
    }

    const auto cards = ClassCards();
    json creatable = json::array();
    for (int classId = 0; classId < Rules::CreatableClassCount; ++classId)
    {
        if (Rules::ClassCreatable(classId, cards))
        {
            creatable.push_back(std::string(Rules::ClassCodeName(classId)));
        }
    }

    json result;
    result["characters"] = CharacterList();
    result["max_slots"] = MAX_CHARACTERS_PER_ACCOUNT;
    result["can_create"] = FirstFreeSlot() >= 0;
    result["creatable_classes"] = std::move(creatable);
    return EncodeResult(request.EncodedId(), result.dump());
}

// char-create: sends the request the way CCharMakeWin::RequestCreateCharacter does, then waits
// for the server's answer. One request sends one packet.
class CharCreateAct : public Act
{
public:
    CharCreateAct(std::string name, int classId, int requestedSlot)
        : m_name(std::move(name)), m_classId(classId), m_requestedSlot(requestedSlot)
    {
    }

    [[nodiscard]] std::string_view Name() const override
    {
        return "char-create";
    }
    [[nodiscard]] std::optional<std::chrono::milliseconds> Deadline() const override
    {
        return CharCreateDeadline;
    }
    [[nodiscard]] std::string ProgressObject() const override
    {
        json progress;
        progress["name"] = m_name;
        progress["class"] = std::string(Rules::ClassCodeName(m_classId));
        progress["sent"] = m_sent;
        return progress.dump();
    }

    [[nodiscard]] Status Tick(std::string& response) override
    {
        if (!m_sent)
        {
            Send();
            return Status::Running;
        }

        if (CurrentProtocolState == RECEIVE_CREATE_CHARACTER_SUCCESS)
        {
            const int slot = FirstNewSlot();
            if (slot < 0)
            {
                response = EncodeError(EncodedId(), ErrorCode::Failed,
                                       "the server accepted the character, but no new slot appeared");
                return Status::Finished;
            }
            json result = CharacterSummary(slot);
            if (m_requestedSlot >= 0 && slot != m_requestedSlot)
            {
                result["requested_slot"] = m_requestedSlot + 1;
            }
            response = EncodeResult(EncodedId(), result.dump());
            return Status::Finished;
        }

        const int code = g_MsgWin.PendingMessageCode();
        if (code == RECEIVE_CREATE_CHARACTER_FAIL || code == RECEIVE_CREATE_CHARACTER_FAIL2)
        {
            // The client's own popup shows the reason and reopens the create window; both are closed again.
            g_MsgWin.DismissMessage();
            g_CharMakeWin.Show(false);

            json details;
            if (code == RECEIVE_CREATE_CHARACTER_FAIL2)
            {
                details["reason"] = "no_slot";
                response = EncodeError(EncodedId(), ErrorCode::NotAllowed, "the server allows no more characters",
                                       details.dump());
            }
            else
            {
                // The server sends one message for both a taken name and a name it will not take.
                details["reason"] = "name_taken_or_invalid";
                response = EncodeError(EncodedId(), ErrorCode::Failed,
                                       "the server refused the name: taken, or not allowed", details.dump());
            }
            return Status::Finished;
        }
        return Status::Running;
    }

private:
    void Send()
    {
        for (int slot = 0; slot < MAX_CHARACTERS_PER_ACCOUNT; ++slot)
        {
            m_before[slot] = Occupied(slot);
        }

        // The packet carries the class and skin of CharacterView, which the new hero is built from.
        CharacterView.Class = static_cast<CLASS_TYPE>(m_classId);
        const auto classByte = static_cast<CharacterClassNumber>((CharacterView.Class << 2) + CharacterView.Skin);
        const std::wstring wideName = Core::Text::FromUtf8(m_name);

        CurrentProtocolState = REQUEST_CREATE_CHARACTER;
        SocketClient->ToGameServer()->SendCreateCharacter(MU_C16(wideName.c_str()), classByte);
        m_sent = true;

        g_CharMakeWin.Show(false);
        CSceneUICoordinator::Instance().PopUpMsgWin(MESSAGE_WAIT);
    }

    int FirstNewSlot() const
    {
        for (int slot = 0; slot < MAX_CHARACTERS_PER_ACCOUNT; ++slot)
        {
            if (Occupied(slot) && !m_before[slot])
            {
                return slot;
            }
        }
        return -1;
    }

    std::string m_name;
    int m_classId;
    int m_requestedSlot;
    bool m_sent = false;
    std::array<bool, MAX_CHARACTERS_PER_ACCOUNT> m_before{};
};

// char-create's arguments: `name`, `class` (code name or number), and an optional `slot`.
std::string CharCreate(const Request& request, std::unique_ptr<Act>& act)
{
    if (App::Control::Commands::CurrentSceneName() != "character_list")
    {
        return EncodeError(request.EncodedId(), ErrorCode::WrongScene, "char-create needs the character select screen");
    }

    std::string name;
    if (!request.GetString("name", name))
    {
        return EncodeError(request.EncodedId(), ErrorCode::BadRequest, "`char-create` needs `name`");
    }

    std::string classText;
    if (!request.GetString("class", classText))
    {
        int classNumber = 0;
        if (!request.GetInt("class", classNumber))
        {
            return EncodeError(request.EncodedId(), ErrorCode::BadRequest,
                               "`char-create` needs `class`: " + std::string(ClassNamesHint));
        }
        classText = std::to_string(classNumber);
    }

    int requestedSlot = -1;
    if (request.Has("slot"))
    {
        int number = 0;
        if (!request.GetInt("slot", number) || number < 1 || number > MAX_CHARACTERS_PER_ACCOUNT)
        {
            return EncodeError(request.EncodedId(), ErrorCode::BadRequest, "`slot` is a whole number from 1 to 5");
        }
        if (Occupied(number - 1))
        {
            return EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                               "slot " + std::to_string(number) + " already holds a character");
        }
        requestedSlot = number - 1;
    }

    const std::optional<int> classId = Rules::ResolveCreatableClass(classText);
    switch (Rules::CheckCreate(name, classId, ClassCards(), FirstFreeSlot() >= 0))
    {
    case Rules::CreateRefusal::InvalidName:
        return EncodeError(request.EncodedId(), ErrorCode::BadRequest,
                           "`name` is 4 to 10 letters or digits, with no spaces or symbols");
    case Rules::CreateRefusal::UnknownClass:
        return EncodeError(request.EncodedId(), ErrorCode::BadRequest,
                           "unknown class `" + classText + "`; use " + std::string(ClassNamesHint));
    case Rules::CreateRefusal::ClassNotAllowed:
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                           "this account may not create " + std::string(Rules::ClassCodeName(*classId)) +
                               " yet: the server has not sent its class card");
    case Rules::CreateRefusal::NoFreeSlot:
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed, "no free character slot");
    case Rules::CreateRefusal::None:
        break;
    }

    if (g_MsgWin.PendingMessageCode() == MESSAGE_WAIT)
    {
        return EncodeError(request.EncodedId(), ErrorCode::Busy, "the client is still waiting for a character reply");
    }

    act = std::make_unique<CharCreateAct>(name, *classId, requestedSlot);
    return {};
}

// char-delete: sends the delete the way CMsgWin::RequestDeleteCharacter does, then waits for the
// server's answer. The personal code is sent and never kept in progress or in any answer.
class CharDeleteAct : public Act
{
public:
    CharDeleteAct(int slot, std::string name, std::wstring code)
        : m_slot(slot), m_name(std::move(name)), m_code(std::move(code))
    {
    }

    [[nodiscard]] std::string_view Name() const override
    {
        return "char-delete";
    }
    [[nodiscard]] std::optional<std::chrono::milliseconds> Deadline() const override
    {
        return CharDeleteDeadline;
    }
    [[nodiscard]] std::string ProgressObject() const override
    {
        json progress;
        progress["slot"] = m_slot + 1;
        progress["name"] = m_name;
        progress["sent"] = m_sent;
        return progress.dump();
    }

    [[nodiscard]] Status Tick(std::string& response) override
    {
        if (!m_sent)
        {
            // ReceiveDeleteCharacter and the success popup both work on the selected hero's slot.
            SelectedHero = m_slot;
            CurrentProtocolState = REQUEST_DELETE_CHARACTER;
            SocketClient->ToGameServer()->SendDeleteCharacter(MU_C16(CharactersClient[m_slot].ID),
                                                              MU_C16(m_code.c_str()));
            m_sent = true;
            CSceneUICoordinator::Instance().PopUpMsgWin(MESSAGE_WAIT);
            return Status::Running;
        }

        if (!Occupied(m_slot))
        {
            if (g_MsgWin.PendingMessageCode() == MESSAGE_DELETE_CHARACTER_SUCCESS)
            {
                g_MsgWin.DismissMessage();
            }
            json result;
            result["deleted"] = true;
            result["slot"] = m_slot + 1;
            result["name"] = m_name;
            response = EncodeResult(EncodedId(), result.dump());
            return Status::Finished;
        }

        const int code = g_MsgWin.PendingMessageCode();
        if (code == MESSAGE_DELETE_CHARACTER_GUILDWARNING || code == MESSAGE_DELETE_CHARACTER_ITEM_BLOCK ||
            code == MESSAGE_STORAGE_RESIDENTWRONG)
        {
            g_MsgWin.DismissMessage();
            json details;
            if (code == MESSAGE_DELETE_CHARACTER_GUILDWARNING)
            {
                details["reason"] = "guild_member";
                response = EncodeError(EncodedId(), ErrorCode::NotAllowed, "the server refused: a guild member",
                                       details.dump());
            }
            else if (code == MESSAGE_DELETE_CHARACTER_ITEM_BLOCK)
            {
                details["reason"] = "item_blocked";
                response = EncodeError(EncodedId(), ErrorCode::NotAllowed, "the server refused: item-blocked",
                                       details.dump());
            }
            else
            {
                details["reason"] = "wrong_code";
                response = EncodeError(EncodedId(), ErrorCode::BadRequest,
                                       "the server did not accept the personal code", details.dump());
            }
            return Status::Finished;
        }
        return Status::Running;
    }

private:
    int m_slot;
    std::string m_name;
    std::wstring m_code;
    bool m_sent = false;
};

// char-delete's arguments: `slot`, the exact `name` in it, and `code`, the personal code.
std::string CharDelete(const Request& request, std::unique_ptr<Act>& act)
{
    if (App::Control::Commands::CurrentSceneName() != "character_list")
    {
        return EncodeError(request.EncodedId(), ErrorCode::WrongScene, "char-delete needs the character select screen");
    }

    std::string name;
    int number = 0;
    std::string code;
    if (!request.GetString("name", name) || name.empty() || !request.GetInt("slot", number) || number < 1 ||
        number > MAX_CHARACTERS_PER_ACCOUNT || !request.GetString("code", code) || code.empty())
    {
        return EncodeError(request.EncodedId(), ErrorCode::BadRequest,
                           "`char-delete` needs `slot` (1 to 5), the exact `name` in it, and `code`, the personal code");
    }
    // The client's own field takes this many characters; a longer code is refused, not cut.
    if (code.size() > static_cast<std::size_t>(g_iLengthAuthorityCode))
    {
        return EncodeError(request.EncodedId(), ErrorCode::BadRequest, "`code` is longer than the client's field takes");
    }

    const int slot = number - 1;
    const std::wstring wideName = Core::Text::FromUtf8(name);
    if (!Occupied(slot) || wideName != CharactersClient[slot].ID)
    {
        json details;
        details["characters"] = CharacterList();
        return EncodeError(request.EncodedId(), ErrorCode::NoSuchCharacter,
                           "no character named `" + name + "` in slot " + std::to_string(number), details.dump());
    }

    const CHARACTER& character = CharactersClient[slot];
    if (!Rules::CanDeleteCharacter(character.GuildStatus != G_NONE,
                                   (character.CtlCode & (CTLCODE_02BLOCKITEM | CTLCODE_10ACCOUNT_BLOCKITEM)) != 0))
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                           "a guild member or an item-blocked character cannot be deleted");
    }

    if (g_MsgWin.PendingMessageCode() == MESSAGE_WAIT)
    {
        return EncodeError(request.EncodedId(), ErrorCode::Busy, "the client is still waiting for a character reply");
    }

    act = std::make_unique<CharDeleteAct>(slot, name, Core::Text::FromUtf8(code));
    return {};
}
} // namespace

namespace App::Control::Commands
{
std::string CharList(const Request& request, std::unique_ptr<Act>& act)
{
    return ::CharList(request, act);
}

std::string CharCreate(const Request& request, std::unique_ptr<Act>& act)
{
    return ::CharCreate(request, act);
}

std::string CharDelete(const Request& request, std::unique_ptr<Act>& act)
{
    return ::CharDelete(request, act);
}
} // namespace App::Control::Commands
