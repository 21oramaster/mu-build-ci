#include "stdafx.h"
#include "App/Control/ControlCommands.h"
#include "App/Control/ControlRules.h"

#include "Character/CharacterManager.h"
#include "Dotnet/PacketFunctions_CommonEnums.h"
#include "Engine/Object/ZzzCharacter.h"
#include "Engine/Object/ZzzInfomation.h"
#include "Network/Server/WSclient.h"

#include "json.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace
{
using App::Control::Act;
using App::Control::ErrorCode;
using App::Control::Request;
using nlohmann::json;
namespace Rules = App::Control::Rules;

// The server answers each point on its own (0x06, ReceiveAddPoint); a point it does not answer in this time is refused.
constexpr std::chrono::milliseconds PointDeadline{2000};

// The stat names the protocol takes, and their attribute. `command` is the Dark Lord's fifth.
struct StatName
{
    std::string_view name;
    CharacterStatAttribute attribute;
};
constexpr StatName StatNames[] = {
    {"strength", CharacterStatAttribute::Strength},
    {"agility", CharacterStatAttribute::Agility},
    {"vitality", CharacterStatAttribute::Vitality},
    {"energy", CharacterStatAttribute::Energy},
    {"command", CharacterStatAttribute::Leadership},
};

const StatName* FindStat(std::string_view name)
{
    for (const StatName& stat : StatNames)
    {
        if (stat.name == name)
        {
            return &stat;
        }
    }
    return nullptr;
}

// stat-add: one point at a time, each sent after the last is answered (CharacterInfoWindow.cpp, RmlClickIncreaseStat).
class StatAddAct : public Act
{
public:
    StatAddAct(std::string_view name, CharacterStatAttribute attribute, int points)
        : m_name(name), m_attribute(attribute), m_points(points)
    {
    }

    [[nodiscard]] std::string_view Name() const override
    {
        return "stat-add";
    }
    // Each point has its own deadline, see Tick.
    [[nodiscard]] std::optional<std::chrono::milliseconds> Deadline() const override
    {
        return std::nullopt;
    }

    [[nodiscard]] std::string ProgressObject() const override
    {
        json progress;
        progress["stat"] = std::string(m_name);
        progress["points"] = m_done;
        return progress.dump();
    }

    [[nodiscard]] Status Tick(std::string& response) override
    {
        if (m_done == m_points)
        {
            json result;
            result["stat"] = std::string(m_name);
            result["points"] = m_done;
            result["level_up_points"] = static_cast<int>(CharacterAttribute->LevelUpPoint);
            response = App::Control::EncodeResult(EncodedId(), result.dump());
            return Status::Finished;
        }

        const auto now = std::chrono::steady_clock::now();
        if (!m_inFlight)
        {
            if (CharacterAttribute->LevelUpPoint <= 0)
            {
                response = App::Control::EncodeError(EncodedId(), ErrorCode::NotAllowed,
                                                     "the level-up points ran out", ProgressObject());
                return Status::Finished;
            }

            m_before = CharacterAttribute->LevelUpPoint;
            m_sentAt = now;
            SocketClient->ToGameServer()->SendIncreaseCharacterStatPoint(m_attribute);
            m_inFlight = true;
            return Status::Running;
        }

        // The answer takes a level-up point away; a refusal changes nothing.
        if (CharacterAttribute->LevelUpPoint < m_before)
        {
            ++m_done;
            m_inFlight = false;
            return Status::Running;
        }

        if (now - m_sentAt < PointDeadline)
        {
            return Status::Running;
        }
        response = App::Control::EncodeError(EncodedId(), ErrorCode::Failed,
                                             "the server did not add the point", ProgressObject());
        return Status::Finished;
    }

private:
    std::string_view m_name;
    CharacterStatAttribute m_attribute;
    int m_points;
    int m_done = 0;
    bool m_inFlight = false;
    int m_before = 0;
    std::chrono::steady_clock::time_point m_sentAt{};
};
} // namespace

namespace App::Control::Commands
{
// stat-add: `stat` is strength, agility, vitality, energy or command (a Dark Lord only);
// `points` is 1 or more, 1 by default.
std::string StatAdd(const Request& request, std::unique_ptr<Act>& act)
{
    std::string name;
    const StatName* stat = nullptr;
    if (!request.GetString("stat", name) || (stat = FindStat(name)) == nullptr)
    {
        return EncodeError(request.EncodedId(), ErrorCode::BadRequest,
                           "`stat-add` needs a `stat`: strength, agility, vitality, energy or command");
    }

    int points = 1;
    if (request.Has("points") && (!request.GetInt("points", points) || points < 1))
    {
        return EncodeError(request.EncodedId(), ErrorCode::BadRequest, "`points` is a whole number, at least 1");
    }

    if (Hero == nullptr || CharacterAttribute == nullptr)
    {
        return EncodeError(request.EncodedId(), ErrorCode::WrongScene, "no character is in the world");
    }

    if (stat->attribute == CharacterStatAttribute::Leadership &&
        gCharacterManager.GetBaseClass(Hero->Class) != CLASS_DARK_LORD)
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed, "only a Dark Lord has command");
    }

    const int available = static_cast<int>(CharacterAttribute->LevelUpPoint);
    if (!Rules::PointsWithin(points, available))
    {
        return EncodeError(request.EncodedId(), ErrorCode::NotAllowed,
                           "not enough level-up points: " + std::to_string(points) + " asked, " +
                               std::to_string(available) + " left");
    }

    act = std::make_unique<StatAddAct>(stat->name, stat->attribute, points);
    return {};
}
} // namespace App::Control::Commands
