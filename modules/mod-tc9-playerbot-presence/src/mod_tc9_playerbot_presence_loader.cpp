/*
 * mod-tc9-playerbot-presence
 *
 * Publishes gw.char.logged-in/-out for playerbot logins so charserver's
 * online-character cache includes bots, fixing /who and /invite by name.
 * Bots never go through the gateway, so charserver never otherwise learns
 * they're online.
 *
 * Bot detection uses WorldSession::IsBot(), not GET_PLAYERBOT_AI(), since
 * the latter isn't populated yet when OnPlayerLogin fires.
 */

#include "Config.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Guild.h"
#include "GuildMgr.h"
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

    // Mirrors GWEventCharacterLoggedInPayload.
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

    // Mirrors GWEventCharacterLoggedOutPayload.
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

    // Fields are unquoted integers, so substring-find + strtoull is enough.
    uint64 ExtractUint64Field(std::string const& json, char const* fieldName)
    {
        std::string needle = std::string("\"") + fieldName + "\":";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return 0;
        return std::strtoull(json.c_str() + pos + needle.size(), nullptr, 10);
    }

    // Unescapes a JSON string value; good enough for chat text, not a full JSON parser.
    std::string ExtractStringField(std::string const& json, char const* fieldName)
    {
        std::string needle = std::string("\"") + fieldName + "\":\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return "";
        pos += needle.size();

        std::string out;
        while (pos < json.size() && json[pos] != '"')
        {
            if (json[pos] == '\\' && pos + 1 < json.size())
            {
                char next = json[pos + 1];
                switch (next)
                {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'u': pos += 4; break; // drop \uXXXX rather than decode it
                    default: out += next;
                }
                pos += 2;
            }
            else
            {
                out += json[pos];
                pos++;
            }
        }
        return out;
    }

    // Bots have no client, so their AI never sees a whisper/party/raid/guild
    // message sent through the gateway: it intercepts those chat types and
    // relays them via groupserver/chatserver to other real clients' own
    // gateway sessions, never forwarding the raw packet to the worldserver.
    // That means core's OnPlayerCanUseChat, which mod-playerbots hooks to
    // parse chat as bot commands, never fires for a bot-directed message.
    // Subscribe to the same delivery events a real gateway would consume and
    // invoke that hook directly for any local bot recipient.
    void OnBotWhisperReceived(char const* /*subject*/, char const* payload, int payloadLen)
    {
        if (!payload || payloadLen <= 0)
            return;

        std::string json(payload, size_t(payloadLen));

        uint64 senderGuid = ExtractUint64Field(json, "SenderGUID");
        uint64 receiverGuid = ExtractUint64Field(json, "ReceiverGUID");
        uint32 language = uint32(ExtractUint64Field(json, "Language"));
        std::string msg = ExtractStringField(json, "Msg");
        if (!senderGuid || !receiverGuid || msg.empty())
            return;

        Player* receiver = ObjectAccessor::FindPlayer(ObjectGuid(receiverGuid));
        if (!receiver || !receiver->GetSession() || !receiver->GetSession()->IsBot())
            return;

        Player* sender = ObjectAccessor::FindPlayer(ObjectGuid(senderGuid));
        if (!sender)
            return;

        sScriptMgr->OnPlayerCanUseChat(sender, CHAT_MSG_WHISPER, language, msg, receiver);
    }

    // See OnBotWhisperReceived. mod-playerbots' Group overload walks every
    // member itself, so one call here covers every local bot in the group.
    void OnGroupChatMessage(char const* /*subject*/, char const* payload, int payloadLen)
    {
        if (!payload || payloadLen <= 0)
            return;

        std::string json(payload, size_t(payloadLen));

        uint64 senderGuid = ExtractUint64Field(json, "SenderGUID");
        uint32 groupId = uint32(ExtractUint64Field(json, "GroupID"));
        uint32 language = uint32(ExtractUint64Field(json, "Language"));
        uint32 messageType = uint32(ExtractUint64Field(json, "MessageType"));
        std::string msg = ExtractStringField(json, "Msg");
        if (!senderGuid || !groupId || msg.empty())
            return;

        Group* group = sGroupMgr->GetGroupByGUID(groupId);
        if (!group)
            return;

        Player* sender = ObjectAccessor::FindPlayer(ObjectGuid(senderGuid));
        if (!sender)
            return;

        sScriptMgr->OnPlayerCanUseChat(sender, messageType, language, msg, group);
    }

    // See OnBotWhisperReceived. mod-playerbots' Guild overload only checks
    // the sender's own controlled bots, so one call here is enough.
    void OnGuildChatMessage(char const* /*subject*/, char const* payload, int payloadLen)
    {
        if (!payload || payloadLen <= 0)
            return;

        std::string json(payload, size_t(payloadLen));

        uint64 senderGuid = ExtractUint64Field(json, "SenderGUID");
        uint32 guildId = uint32(ExtractUint64Field(json, "GuildID"));
        uint32 language = uint32(ExtractUint64Field(json, "Language"));
        std::string msg = ExtractStringField(json, "Msg");
        if (!senderGuid || !guildId || msg.empty())
            return;

        Guild* guild = sGuildMgr->GetGuildById(guildId);
        if (!guild)
            return;

        Player* sender = ObjectAccessor::FindPlayer(ObjectGuid(senderGuid));
        if (!sender)
            return;

        sScriptMgr->OnPlayerCanUseChat(sender, CHAT_MSG_GUILD, language, msg, guild);
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
        LOG_INFO("server", "TC9ClusterEventWatcher: bot {} (GUID {}) invite accept {}",
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

        sToCloud9Sidecar->NatsPublish("gw.char.logged-in", BuildLoggedInPayload(player));
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player || !sToCloud9Sidecar->ClusterModeEnabled() || !player->GetSession()->IsBot())
            return;

        sToCloud9Sidecar->NatsPublish("gw.char.logged-out", BuildLoggedOutPayload(player));
    }
};

class TC9ClusterEventWatcher : public WorldScript
{
public:
    TC9ClusterEventWatcher() : WorldScript("TC9ClusterEventWatcher") {}

    // Not OnStartup(): it runs before sToCloud9Sidecar->Init(), so
    // subscribing there is a silent no-op. Subscribe on the first tick.
    void OnUpdate(uint32 /*diff*/) override
    {
        if (_subscribed || !sToCloud9Sidecar->ClusterModeEnabled())
            return;

        _subscribed = true;
        Subscribe("group.invite.created", &OnGroupInviteCreated);
        Subscribe("chat.gw.playerbot.income.whisper", &OnBotWhisperReceived);
        Subscribe("group.message.new", &OnGroupChatMessage);
        Subscribe("guild.message.new", &OnGuildChatMessage);
    }

private:
    bool _subscribed = false;

    static void Subscribe(std::string const& subject, void (*handler)(char const*, char const*, int))
    {
        bool ok = sToCloud9Sidecar->NatsSubscribe(subject, handler);
        LOG_INFO("server", "TC9ClusterEventWatcher: subscribe to {} {}", subject, ok ? "succeeded" : "failed");
    }
};

void Addmod_tc9_playerbot_presenceScripts()
{
    new TC9PlayerbotPresence();
    new TC9ClusterEventWatcher();
}
