/*
 * mod-tc9-playerbot-presence
 *
 * Publishes lb.char.logged-in/-out for playerbot logins so charserver's
 * online-character cache includes bots, fixing /who and /invite by name.
 * Bots never go through the gateway, so charserver never otherwise learns
 * they're online.
 *
 * Uses the legacy lb.char.* schema, not gw.char.*, because the deployed
 * charserver is pinned to v0.0.4, which predates that rename. Real players
 * likely hit the same mismatch, since the live gateway is already v0.0.5.
 *
 * Bot detection uses WorldSession::IsBot(), not GET_PLAYERBOT_AI(), since
 * the latter isn't populated yet when OnPlayerLogin fires.
 */

#include "Config.h"
#include "Log.h"
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

    // Mirrors LBEventCharacterLoggedInPayload.
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

    // Mirrors LBEventCharacterLoggedOutPayload.
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

    // Fields are unquoted integers, so substring-find + strtoull is enough.
    uint64 ExtractUint64Field(std::string const& json, char const* fieldName)
    {
        std::string needle = std::string("\"") + fieldName + "\":";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return 0;
        return std::strtoull(json.c_str() + pos + needle.size(), nullptr, 10);
    }

    // Bots have no client to click Accept, and mod-playerbots' own
    // auto-accept is gated on Player::GetGroupInvite(), which this cluster
    // architecture never populates. Call groupserver's AcceptInvite RPC
    // directly instead, the same one the gateway calls for real players.
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

        bool accepted = sToCloud9Sidecar->GroupAcceptInvite(
            realmId ? realmId : sConfigMgr->GetOption<uint32>("RealmID", 1), inviteeGuid);
        LOG_INFO("server", "TC9GroupInviteWatcher: bot {} (GUID {}) invite accept {}",
            invitee->GetName(), inviteeGuid, accepted ? "succeeded" : "failed");
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

    // Not OnStartup(): it runs before sToCloud9Sidecar->Init(), so
    // subscribing there is a silent no-op. Subscribe on the first tick.
    void OnUpdate(uint32 /*diff*/) override
    {
        if (_subscribed || !sToCloud9Sidecar->ClusterModeEnabled())
            return;

        _subscribed = sToCloud9Sidecar->NatsSubscribe("group.invite.created", &OnGroupInviteCreated);
        LOG_INFO("server", "TC9GroupInviteWatcher: subscribe to group.invite.created {}",
            _subscribed ? "succeeded" : "failed");
    }

private:
    bool _subscribed = false;
};

void Addmod_tc9_playerbot_presenceScripts()
{
    new TC9PlayerbotPresence();
    new TC9GroupInviteWatcher();
}
