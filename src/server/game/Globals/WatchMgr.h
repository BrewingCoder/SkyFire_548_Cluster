/*
* [lab] WatchMgr - character-scoped event watcher for live debugging.
* Flip it on for a player (in-game or over RA) and server events happening
* around them (GO use/state, creature spawn/death, unit flag changes) are
* emitted as machine-readable JSON Lines, each prefixed with the literal
* marker "WATCHJSON " so they are trivially grep-able out of the server log.
* Near-zero cost when no watch is active.
*/
#ifndef SF_WATCHMGR_H
#define SF_WATCHMGR_H

#include "Define.h"
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>

class WorldObject;

enum WatchCategory
{
    WATCH_CAT_GO    = 0x01, // gameobject use + state change
    WATCH_CAT_SPAWN = 0x02, // creature added to world
    WATCH_CAT_DEATH = 0x04, // creature just died
    WATCH_CAT_FLAGS = 0x08, // UNIT_FIELD_FLAGS set/remove
    WATCH_CAT_MOVE  = 0x10, // creature movement splines sent to clients (launch/stop/rejected) - noisy, opt-in
    WATCH_CAT_CAST  = 0x20, // creature spell casts: start (with cast time) / failed (with SpellCastResult) - opt-in
    WATCH_CAT_DEFAULT = 0x0F, // what an omitted cats arg means (everything except the noisy 'move')
    WATCH_CAT_ALL   = 0xFF
};

class WatchMgr
{
public:
    static WatchMgr* instance();

    // control (return false + fill outInfo on failure)
    bool StartByName(std::string const& name, float radius, uint32 catMask, std::string& outInfo);
    bool StartByGuid(uint32 lowGuid, float radius, uint32 catMask, std::string& outInfo);
    bool StopByName(std::string const& name, std::string& outInfo);
    void StopAll();
    std::string Status();

    // cheap early-out used by the chokepoints
    bool Active() const { return m_active.load(std::memory_order_relaxed); }

    // emit one JSON event for every active watch in range of 'subject'
    void Event(WorldObject const* subject, uint32 cat, char const* ev, std::string const& extraJson = "");

    static std::string DecodeUnitFlags(uint32 mask);
    static uint32 ParseCategories(std::string const& csv); // "go,spawn,move" -> mask; "" -> DEFAULT; "all" -> ALL

private:
    WatchMgr() { }
    void refreshActive();
    static char const* CatLabel(uint32 catBit);

    struct Watch { uint64 guid; float radius; uint32 catMask; };
    std::unordered_map<uint64, Watch> m_watches;
    std::mutex m_mutex;
    std::atomic<bool> m_active{ false };
};

#define sWatchMgr WatchMgr::instance()

#endif
