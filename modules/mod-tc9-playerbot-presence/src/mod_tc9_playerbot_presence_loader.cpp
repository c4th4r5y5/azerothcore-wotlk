/*
 * mod-tc9-playerbot-presence
 *
 * ToCloud9's cluster layer tracks "who is online" via a NATS event
 * (gw.char.logged-in / gw.char.logged-out) that the *gateway* publishes when
 * a real client finishes CMsgPlayerLogin. Services like charserver build
 * their online-character cache entirely from that event stream.
 *
 * Playerbot characters never go through the gateway at all — mod-playerbots
 * drives them through a fake, in-process WorldSession directly on the
 * worldserver for performance. They're fully real Player objects to the
 * core (duel, trade, chat, targeting all work normally), but since they
 * never trigger the gateway's login event, charserver never learns they
 * exist. That makes them invisible to anything backed by that cache:
 * /who won't list them, and /invite by name resolves through
 * charserver's CharacterOnlineByName, so it fails with "player not found"
 * even though the bot is genuinely online.
 *
 * This module closes that gap: it publishes the same NATS events the
 * gateway would have, for playerbot logins/logouts specifically, using the
 * exact wire format apps/charserver/service/characters-listener.go expects
 * (see ToCloud9/shared/events/events-gateway.go for the payload schema).
 *
 * Bot detection uses WorldSession::IsBot() rather than mod-playerbots'
 * GET_PLAYERBOT_AI(): the latter is only populated by an async task queued
 * during login, which hasn't run yet by the time OnPlayerLogin fires here.
 * IsBot() is a plain WorldSession field set synchronously at construction,
 * before HandlePlayerLoginFromDB even starts, and it's core (not a
 * mod-playerbots dependency), so this module needs nothing from it.
 */

#include "Config.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "TC9Sidecar.h"
#include "WorldSession.h"

#include <sstream>

namespace
{
    std::string JsonEscape(std::string const& s)
    {
        std::string out;
        out.reserve(s.size());
        for (char c : s)
        {
            switch (c)
            {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                default:
                    out += c;
            }
        }
        return out;
    }

    // gw.char.logged-in payload — mirrors GWEventCharacterLoggedInPayload.
    std::string BuildLoggedInPayload(Player* player)
    {
        std::ostringstream json;
        json << "{\"v\":\"0.0.1\",\"t\":1,\"p\":{"
             << "\"RealmID\":" << sConfigMgr->GetOption<uint32>("RealmID", 1) << ","
             << "\"GatewayID\":\"playerbot\","
             << "\"CharGUID\":" << player->GetGUID().GetCounter() << ","
             << "\"CharName\":\"" << JsonEscape(player->GetName()) << "\","
             << "\"CharRace\":" << uint32(player->getRace()) << ","
             << "\"CharClass\":" << uint32(player->getClass()) << ","
             << "\"CharGender\":" << uint32(player->getGender()) << ","
             << "\"CharLevel\":" << uint32(player->GetLevel()) << ","
             << "\"CharZone\":" << player->GetZoneId() << ","
             << "\"CharMap\":" << player->GetMapId() << ","
             << "\"CharPosX\":" << player->GetPositionX() << ","
             << "\"CharPosY\":" << player->GetPositionY() << ","
             << "\"CharPosZ\":" << player->GetPositionZ() << ","
             << "\"CharGuildID\":" << player->GetGuildId() << ","
             << "\"AccountID\":" << player->GetSession()->GetAccountId()
             << "}}";
        return json.str();
    }

    // gw.char.logged-out payload — mirrors GWEventCharacterLoggedOutPayload.
    std::string BuildLoggedOutPayload(Player* player)
    {
        std::ostringstream json;
        json << "{\"v\":\"0.0.1\",\"t\":2,\"p\":{"
             << "\"RealmID\":" << sConfigMgr->GetOption<uint32>("RealmID", 1) << ","
             << "\"GatewayID\":\"playerbot\","
             << "\"CharGUID\":" << player->GetGUID().GetCounter() << ","
             << "\"CharName\":\"" << JsonEscape(player->GetName()) << "\","
             << "\"CharGuildID\":" << player->GetGuildId() << ","
             << "\"AccountID\":" << player->GetSession()->GetAccountId()
             << "}}";
        return json.str();
    }
}

class TC9PlayerbotPresence : public PlayerScript
{
public:
    TC9PlayerbotPresence() : PlayerScript("TC9PlayerbotPresence") {}

    void OnPlayerLogin(Player* player) override
    {
        if (!player || !sToCloud9Sidecar->ClusterModeEnabled() || !player->GetSession()->IsBot())
            return;

        sToCloud9Sidecar->NatsPublish("gw.char.logged-in", BuildLoggedInPayload(player));
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player || !sToCloud9Sidecar->ClusterModeEnabled() || !player->GetSession()->IsBot())
            return;

        sToCloud9Sidecar->NatsPublish("gw.char.logged-out", BuildLoggedOutPayload(player));
    }
};

void Addmod_tc9_playerbot_presenceScripts()
{
    new TC9PlayerbotPresence();
}
