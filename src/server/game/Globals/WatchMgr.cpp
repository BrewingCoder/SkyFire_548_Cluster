/*
* [lab] WatchMgr implementation. See WatchMgr.h.
*/
#include "WatchMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Object.h"
#include <sstream>
#include <chrono>
#include <cstdio>

WatchMgr* WatchMgr::instance()
{
    static WatchMgr inst;
    return &inst;
}

void WatchMgr::refreshActive()
{
    m_active.store(!m_watches.empty(), std::memory_order_relaxed);
}

char const* WatchMgr::CatLabel(uint32 catBit)
{
    switch (catBit)
    {
        case WATCH_CAT_GO:    return "go";
        case WATCH_CAT_SPAWN: return "spawn";
        case WATCH_CAT_DEATH: return "death";
        case WATCH_CAT_FLAGS: return "flags";
        default:              return "?";
    }
}

uint32 WatchMgr::ParseCategories(std::string const& csv)
{
    if (csv.empty() || csv == "all")
        return WATCH_CAT_ALL;

    uint32 mask = 0;
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ','))
    {
        if (tok == "go")         mask |= WATCH_CAT_GO;
        else if (tok == "spawn") mask |= WATCH_CAT_SPAWN;
        else if (tok == "death") mask |= WATCH_CAT_DEATH;
        else if (tok == "flags") mask |= WATCH_CAT_FLAGS;
    }
    return mask ? mask : WATCH_CAT_ALL;
}

std::string WatchMgr::DecodeUnitFlags(uint32 mask)
{
    static struct { uint32 bit; char const* name; } const names[] =
    {
        { 0x00000002, "NON_ATTACKABLE" },
        { 0x00000004, "DISABLE_MOVE" },
        { 0x00000008, "PVP_ATTACKABLE" },
        { 0x00000080, "NOT_ATTACKABLE_1" },
        { 0x00000100, "IMMUNE_TO_PC" },
        { 0x00000200, "IMMUNE_TO_NPC" },
        { 0x00020000, "PACIFIED" },
        { 0x00040000, "STUNNED" },
        { 0x00080000, "IN_COMBAT" },
        { 0x00400000, "CONFUSED" },
        { 0x00800000, "FLEEING" },
        { 0x02000000, "NOT_SELECTABLE" },
    };

    std::string out;
    uint32 known = 0;
    for (auto const& n : names)
        if (mask & n.bit)
        {
            if (!out.empty()) out += ",";
            out += "\""; out += n.name; out += "\"";
            known |= n.bit;
        }

    if (uint32 rest = (mask & ~known))
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "\"0x%X\"", rest);
        if (!out.empty()) out += ",";
        out += buf;
    }
    return "[" + out + "]";
}

static std::string JsonEscape(std::string const& s)
{
    std::string o;
    o.reserve(s.size() + 2);
    for (char c : s)
    {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n' || c == '\r') o += ' ';
        else o += c;
    }
    return o;
}

bool WatchMgr::StartByName(std::string const& name, float radius, uint32 catMask, std::string& outInfo)
{
    Player* p = ObjectAccessor::FindPlayerByName(name);
    if (!p)
    {
        outInfo = "player '" + name + "' not found or offline";
        return false;
    }
    return StartByGuid(p->GetGUIDLow(), radius, catMask, outInfo);
}

bool WatchMgr::StartByGuid(uint32 lowGuid, float radius, uint32 catMask, std::string& outInfo)
{
    Player* p = nullptr;
    // resolve the online player by low guid
    HashMapHolder<Player>::MapType const& m = ObjectAccessor::GetPlayers();
    for (HashMapHolder<Player>::MapType::const_iterator it = m.begin(); it != m.end(); ++it)
        if (it->second->IsInWorld() && it->second->GetGUIDLow() == lowGuid)
        {
            p = it->second;
            break;
        }

    if (!p)
    {
        outInfo = "no online player with guid " + std::to_string(lowGuid);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_watches[p->GetGUID()] = Watch{ p->GetGUID(), radius, catMask };
        refreshActive();
    }

    std::ostringstream ss;
    ss << "watching '" << p->GetName() << "' (guid " << lowGuid << ") radius="
       << (radius > 0 ? std::to_string((int)radius) + "yd" : "instance") << " cats=0x" << std::hex << catMask;
    outInfo = ss.str();

    // marker so the start is visible in the same stream we parse
    SF_LOG_INFO("misc", "WATCHJSON {\"ev\":\"WATCH_START\",\"w\":\"%s\",\"guid\":%u,\"radius\":%.0f,\"cats\":%u}",
        JsonEscape(p->GetName()).c_str(), lowGuid, radius, catMask);
    return true;
}

bool WatchMgr::StopByName(std::string const& name, std::string& outInfo)
{
    Player* p = ObjectAccessor::FindPlayerByName(name);
    if (!p)
    {
        outInfo = "player '" + name + "' not found or offline";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_watches.erase(p->GetGUID());
        refreshActive();
    }
    outInfo = "stopped watching '" + p->GetName() + "'";
    return true;
}

void WatchMgr::StopAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_watches.clear();
    refreshActive();
}

std::string WatchMgr::Status()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_watches.empty())
        return "no active watches";

    std::ostringstream ss;
    ss << m_watches.size() << " active watch(es):";
    for (auto const& kv : m_watches)
    {
        Player* p = ObjectAccessor::FindPlayer(kv.first);
        ss << "\n  " << (p ? p->GetName() : "<offline>")
           << " guid=" << (p ? p->GetGUIDLow() : 0u)
           << " radius=" << (kv.second.radius > 0 ? std::to_string((int)kv.second.radius) + "yd" : "instance")
           << " cats=0x" << std::hex << kv.second.catMask << std::dec;
    }
    return ss.str();
}

void WatchMgr::Event(WorldObject const* subject, uint32 cat, char const* ev, std::string const& extraJson)
{
    if (!m_active.load(std::memory_order_relaxed) || !subject)
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_watches.empty())
        return;

    for (auto const& kv : m_watches)
    {
        Watch const& w = kv.second;
        if (!(w.catMask & cat))
            continue;

        Player* p = ObjectAccessor::FindPlayer(kv.first);
        if (!p || !p->IsInWorld())
            continue;
        if (p->GetMapId() != subject->GetMapId() || p->GetInstanceId() != subject->GetInstanceId())
            continue;

        float dist = p->GetDistance(subject);
        if (w.radius > 0.0f && dist > w.radius)
            continue;

        std::ostringstream ss;
        ss << "WATCHJSON {"
           << "\"ts\":" << std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count()
           << ",\"w\":\"" << JsonEscape(p->GetName()) << "\""
           << ",\"map\":" << subject->GetMapId()
           << ",\"inst\":" << subject->GetInstanceId()
           << ",\"cat\":\"" << CatLabel(cat) << "\""
           << ",\"ev\":\"" << ev << "\""
           << ",\"entry\":" << subject->GetEntry()
           << ",\"guid\":" << subject->GetGUIDLow()
           << ",\"name\":\"" << JsonEscape(subject->GetName()) << "\""
           << ",\"x\":" << subject->GetPositionX()
           << ",\"y\":" << subject->GetPositionY()
           << ",\"z\":" << subject->GetPositionZ()
           << ",\"dist\":" << dist;
        if (!extraJson.empty())
            ss << extraJson; // must start with a comma
        ss << "}";

        SF_LOG_INFO("misc", "%s", ss.str().c_str());
    }
}
