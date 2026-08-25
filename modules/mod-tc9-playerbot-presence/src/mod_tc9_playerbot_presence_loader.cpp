/*
 * mod-tc9-playerbot-presence
 *
 * ToCloud9's cluster layer tracks "who is online" via a NATS event
 * (lb.char.logged-in / lb.char.logged-out) that the *gateway* publishes when
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
 * exact wire format apps/charserver/service/characters-listener.go expects.
 *
 * IMPORTANT: the deployed charserver is still on v0.0.4, which predates the
 * "game-load-balancer" -> "gateway" rename — it subscribes to the *old*
 * lb.char.logged-in/-out subjects with a LoadBalancerID field, not the
 * gw.char.* ones current ToCloud9 HEAD publishes (shared/events/events-
 * gateway.go). This targets what's actually deployed. If charserver is ever
 * upgraded past v0.0.4, this needs to switch to the gw.* schema too — and
 * note the *real* gateway (already on v0.0.5) is publishing gw.char.* right
 * now, which the v0.0.4 charserver also can't understand, so this same
 * mismatch likely affects real players' /who and /invite, not just bots.
 *
 * Bot detection uses WorldSession::IsBot() rather than mod-playerbots'
 * GET_PLAYERBOT_AI(): the latter is only populated by an async task queued
 * during login, which hasn't run yet by the time OnPlayerLogin fires here.
 * IsBot() is a plain WorldSession field set synchronously at construction,
 * before HandlePlayerLoginFromDB even starts, and it's core (not a
 * mod-playerbots dependency), so this module needs nothing from it.
 */

#include "Config.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "TC9Sidecar.h"
#include "WorldSession.h"

#include <cstdlib>
#include <sstream>
#include <string>

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

    // lb.char.logged-in payload — mirrors LBEventCharacterLoggedInPayload.
    std::string BuildLoggedInPayload(Player* player)
    {
        std::ostringstream json;
        json << "{\"v\":\"0.0.1\",\"t\":1,\"p\":{"
             << "\"RealmID\":" << sConfigMgr->GetOption<uint32>("RealmID", 1) << ","
             << "\"LoadBalancerID\":\"playerbot\","
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

    // lb.char.logged-out payload — mirrors LBEventCharacterLoggedOutPayload.
    std::string BuildLoggedOutPayload(Player* player)
    {
        std::ostringstream json;
        json << "{\"v\":\"0.0.1\",\"t\":2,\"p\":{"
             << "\"RealmID\":" << sConfigMgr->GetOption<uint32>("RealmID", 1) << ","
             << "\"LoadBalancerID\":\"playerbot\","
             << "\"CharGUID\":" << player->GetGUID().GetCounter() << ","
             << "\"CharName\":\"" << JsonEscape(player->GetName()) << "\","
             << "\"CharGuildID\":" << player->GetGuildId() << ","
             << "\"AccountID\":" << player->GetSession()->GetAccountId()
             << "}}";
        return json.str();
    }

    // Payload fields we care about are unquoted integers (JSON produced by
    // Go's encoding/json, no whitespace), so a substring-find + strtoull is
    // plenty robust here without pulling in a JSON library for one caller.
    uint64 ExtractUint64Field(std::string const& json, char const* fieldName)
    {
        std::string needle = std::string("\"") + fieldName + "\":";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return 0;
        return std::strtoull(json.c_str() + pos + needle.size(), nullptr, 10);
    }

    // Handles group.invite.created (published by groupserver — see
    // ToCloud9/shared/events/events-group.go, schema unchanged since v0.0.4
    // so no version mismatch here, unlike the presence events above). Real
    // clients get this relayed to them by their gateway session, which shows
    // the invite popup and sends CMsgGroupAccept when the player clicks
    // Accept. Bots have neither, so mod-playerbots' own auto-accept action
    // never fires either — it's gated on Player::GetGroupInvite(), which is
    // core Group-class state this cluster architecture never populates
    // (pending invites are a gateway/client-only concept here; the
    // worldserver's Group mirror only learns about *confirmed* membership,
    // via TC9GroupHooks::OnGroupMemberAdded). So instead of trying to
    // replicate that invite state locally, this calls groupserver's
    // AcceptInvite RPC directly for the bot — the exact same call the
    // gateway makes when a real player clicks Accept — and lets the normal
    // group.member.added event flow back through the existing hook.
    void OnGroupInviteCreated(char const* /*subject*/, char const* payload, int payloadLen)
    {
        if (!payload || payloadLen <= 0)
            return;

        std::string json(payload, size_t(payloadLen));

        uint32 realmId = uint32(ExtractUint64Field(json, "RealmID"));
        uint64 inviteeGuid = ExtractUint64Field(json, "InviteeGUID");
        if (!inviteeGuid)
            return;

        if (realmId && realmId != sConfigMgr->GetOption<uint32>("RealmID", 1))
            return;

        Player* invitee = ObjectAccessor::FindPlayer(ObjectGuid(inviteeGuid));
        if (!invitee || !invitee->GetSession() || !invitee->GetSession()->IsBot())
            return;

        sToCloud9Sidecar->GroupAcceptInvite(realmId ? realmId : sConfigMgr->GetOption<uint32>("RealmID", 1), inviteeGuid);
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

        sToCloud9Sidecar->NatsPublish("lb.char.logged-in", BuildLoggedInPayload(player));
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player || !sToCloud9Sidecar->ClusterModeEnabled() || !player->GetSession()->IsBot())
            return;

        sToCloud9Sidecar->NatsPublish("lb.char.logged-out", BuildLoggedOutPayload(player));
    }
};

class TC9GroupInviteWatcher : public WorldScript
{
public:
    TC9GroupInviteWatcher() : WorldScript("TC9GroupInviteWatcher") {}

    void OnStartup() override
    {
        sToCloud9Sidecar->NatsSubscribe("group.invite.created", &OnGroupInviteCreated);
    }
};

void Addmod_tc9_playerbot_presenceScripts()
{
    new TC9PlayerbotPresence();
    new TC9GroupInviteWatcher();
}
