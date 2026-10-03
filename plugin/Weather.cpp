// Weather sync: the host's weather is everybody's weather.
//
// Kenshi runs one WeatherInstance per WeatherRegion (a biome group); each region rolls its own
// seasons and weathers. Every 5 s (and on change) the host sends, for each region that is active
// on its side, the season, the weather type and its timing/strength/wind. Clients write them into
// their region and stop rolling new weather themselves (Season::getNewWeather is skipped for
// regions the host describes), so a sandstorm or acid rain starts and ends for everybody at once.
//
// Regions are matched by the ids of their seasons (the same on every machine with the same mods).
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/GameData.h>
#include <kenshi/Weather.h>
#include <core/Functions.h>

#include "Shared.h"

#include <map>
#include <set>
#include <vector>

using namespace mp;

// Access to the private WeatherRegion::weatherChanged (explicit instantiation may name private
// members; the friend function hands the member pointer out).
namespace kmp_access
{
    template <typename Tag, typename Tag::type M> struct Rob { friend typename Tag::type get(Tag) { return M; } };
    struct WeatherChangedTag { typedef void (WeatherRegion::*type)(bool); friend type get(WeatherChangedTag); };
    template struct Rob<WeatherChangedTag, &WeatherRegion::weatherChanged>;
}

namespace kmp {

namespace
{
    const DWORD SEND_PERIOD_MS = 5000;
    const DWORD HOLD_MS = 30000;          // a client region follows the host this long after an update

    struct RegionWeather
    {
        std::string key;                  // season ids of the region
        int32_t seasonIndex, seasonEndDay;
        std::string weather;              // Weather::weatherData id
        float strength, effectStrength, windSpeed, windX, windY, windZ, weatherTime;
        int32_t startMin, endMin;
        void write(ByteWriter& w) const
        {
            w.str(key); w.u32((uint32_t)seasonIndex); w.u32((uint32_t)seasonEndDay); w.str(weather);
            w.f32(strength); w.f32(effectStrength); w.f32(windSpeed); w.f32(windX); w.f32(windY); w.f32(windZ);
            w.f32(weatherTime); w.u32((uint32_t)startMin); w.u32((uint32_t)endMin);
        }
        void read(ByteReader& r)
        {
            key = r.str(); seasonIndex = (int32_t)r.u32(); seasonEndDay = (int32_t)r.u32(); weather = r.str();
            strength = r.f32(); effectStrength = r.f32(); windSpeed = r.f32(); windX = r.f32(); windY = r.f32(); windZ = r.f32();
            weatherTime = r.f32(); startMin = (int32_t)r.u32(); endMin = (int32_t)r.u32();
        }
        bool sanitize()
        {
            if (key.empty() || seasonIndex < 0 || seasonIndex > 64) return false;
            strength = clampF(strength, 0.f, 10.f); effectStrength = clampF(effectStrength, 0.f, 10.f);
            windSpeed = clampF(windSpeed, 0.f, 1000.f);
            windX = clampF(windX, -1.f, 1.f); windY = clampF(windY, -1.f, 1.f); windZ = clampF(windZ, -1.f, 1.f);
            weatherTime = clampF(weatherTime, 0.f, 1.0e7f);
            return true;
        }
    };

    std::string regionKey(WeatherRegion* r)
    {
        std::string k;
        for (size_t i = 0; i < r->seasons.size(); ++i)
        {
            Season* s = r->seasons[i];
            if (s && s->seasonData) { if (!k.empty()) k += ','; k += s->seasonData->stringID; }
        }
        return k;
    }

    // Distinct regions the engine currently tracks (those with listeners: near loaded objects).
    bool safeRegions(std::vector<WeatherRegion*>& out)
    {
        __try
        {
            WeatherSystem* ws = WeatherSystem::getInstance();
            if (!ws) return false;
            for (auto it = ws->weatherListeners.begin(); it != ws->weatherListeners.end(); ++it)
                if (it->second) out.push_back(it->second);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void regions(std::vector<WeatherRegion*>& out)
    {
        std::vector<WeatherRegion*> all;
        if (!safeRegions(all)) return;
        std::set<WeatherRegion*> seen;
        for (size_t i = 0; i < all.size(); ++i) if (seen.insert(all[i]).second) out.push_back(all[i]);
    }

    bool capture(WeatherRegion* r, RegionWeather& w)
    {
        WeatherInstance* in = r->weatherInstance;
        if (!in || !in->weather || !in->weather->weatherData) return false;
        w.key = regionKey(r);
        if (w.key.empty()) return false;
        w.seasonIndex = r->currentSeasonIndex; w.seasonEndDay = r->currentSeasonEndDay;
        w.weather = in->weather->weatherData->stringID;
        w.strength = in->strength; w.effectStrength = in->effectStrength; w.windSpeed = in->windSpeed;
        w.windX = in->windDirection.x; w.windY = in->windDirection.y; w.windZ = in->windDirection.z;
        w.weatherTime = in->weatherTime; w.startMin = in->startTimeMinutes; w.endMin = in->endTimeMinutes;
        return true;
    }

    void (WeatherRegion::*g_weatherChanged)(bool) = NULL;
    typedef void (*WeatherChangedFn)(WeatherRegion*, bool);
    WeatherChangedFn g_weatherChangedFn = NULL;

    // Writes the host's weather into a client region. Returns false on engine fault.
    bool safeApply(WeatherRegion* r, const RegionWeather* w, Weather* type, bool* changed)
    {
        __try
        {
            *changed = false;
            if (r->currentSeasonIndex != w->seasonIndex && w->seasonIndex < (int)r->seasons.size())
                r->setCurrentSeason(w->seasonIndex, w->seasonEndDay);
            WeatherInstance* in = r->weatherInstance;
            if (!in) return false;
            if (in->weather != type) { in->weather = type; *changed = true; }
            in->strength = w->strength; in->effectStrength = w->effectStrength; in->windSpeed = w->windSpeed;
            in->windDirection = Ogre::Vector3(w->windX, w->windY, w->windZ);
            in->weatherTime = w->weatherTime; in->startTimeMinutes = w->startMin; in->endTimeMinutes = w->endMin;
            if (*changed && g_weatherChangedFn) g_weatherChangedFn(r, true);
            r->requestUpdateEffects = true;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    Weather* findWeather(WeatherRegion* r, int seasonIndex, const std::string& sid)
    {
        // The announced season first, then any other (weathers can be shared between seasons).
        for (int pass = -1; pass < (int)r->seasons.size(); ++pass)
        {
            size_t i = pass < 0 ? (size_t)seasonIndex : (size_t)pass;
            if (i >= r->seasons.size() || !r->seasons[i]) continue;
            lektor<Weather*>& list = r->seasons[i]->weathers;
            for (uint32_t k = 0; k < list.size(); ++k)
                if (list[k] && list[k]->weatherData && list[k]->weatherData->stringID == sid) return list[k];
        }
        return NULL;
    }

    DWORD g_lastSend = 0;
    uint32_t g_lastHash = 0;
    std::map<WeatherRegion*, DWORD> g_heldUntil;   // client: regions following the host (g_lock)

    void (*getNewWeather_orig)(Season*);
    void getNewWeather_hook(Season* self)
    {
        if (self && !g_session.isHost() && g_session.active())
        {
            Lock l;   // weather may update on the background thread
            std::map<WeatherRegion*, DWORD>::iterator it = g_heldUntil.find(self->regionWeather);
            if (it != g_heldUntil.end() && GetTickCount() < it->second) return;   // the host decides
        }
        getNewWeather_orig(self);
    }
}

void weather_tick(DWORD now)
{
    if (!g_session.isHost() || !g_cfg.weatherSync) return;
    if (now - g_lastSend < 1000) return;              // change check each second
    std::vector<WeatherRegion*> rs;
    regions(rs);
    std::vector<RegionWeather> list;
    for (size_t i = 0; i < rs.size(); ++i) { RegionWeather w; if (capture(rs[i], w)) list.push_back(w); }
    if (list.empty()) return;
    ByteWriter w; w.u16((uint16_t)list.size());
    for (size_t i = 0; i < list.size(); ++i) list[i].write(w);
    // Hash without the ever-moving timer so steady weather is only refreshed every 5 s.
    ByteWriter id; for (size_t i = 0; i < list.size(); ++i) { id.str(list[i].key); id.str(list[i].weather); id.u32((uint32_t)list[i].seasonIndex); }
    uint32_t h = hashBytes(id.data);
    if (h == g_lastHash && now - g_lastSend < SEND_PERIOD_MS) return;
    g_lastHash = h; g_lastSend = now;
    g_session.send(MSG_WEATHER, w.data);
}

void weather_onMessage(const NetEvent& e)
{
    if (g_session.isHost() || e.sender != HOST_ID || !g_cfg.weatherSync) return;
    ByteReader r(e.body);
    uint16_t n = r.u16();
    if (!r.ok() || n > 256) return;
    std::vector<RegionWeather> list;
    for (uint16_t i = 0; i < n && r.ok(); ++i) { RegionWeather w; w.read(r); if (r.ok() && w.sanitize()) list.push_back(w); }
    if (!r.ok()) return;

    std::vector<WeatherRegion*> rs;
    regions(rs);
    DWORD now = GetTickCount();
    for (size_t i = 0; i < rs.size(); ++i)
    {
        std::string key = regionKey(rs[i]);
        for (size_t k = 0; k < list.size(); ++k)
        {
            if (list[k].key != key) continue;
            Weather* type = findWeather(rs[i], list[k].seasonIndex, list[k].weather);
            if (!type) { static DWORD warn = 0; if (now - warn > 30000) { warn = now; log("weather '%s' unknown here (missing mod?)", list[k].weather.c_str()); } break; }
            bool changed = false;
            if (safeApply(rs[i], &list[k], type, &changed))
            {
                { Lock l; g_heldUntil[rs[i]] = now + HOLD_MS; }
                if (changed) log("weather synced: %s", list[k].weather.c_str());
            }
            break;
        }
    }
}

// Host: the weather goes out at the next tick, even if it did not change (a player joined / reloaded).
void weather_sendNow() { g_lastHash = 0; g_lastSend = 0; }

void weather_onWorldReload()
{
    { Lock l; g_heldUntil.clear(); }
    g_lastHash = 0;
}

bool weather_install()
{
    g_weatherChanged = get(kmp_access::WeatherChangedTag());
    g_weatherChangedFn = (WeatherChangedFn)KenshiLib::GetRealAddress(g_weatherChanged);
    if (!g_weatherChangedFn) log("WeatherRegion::weatherChanged not found (weather effects refresh on their own)");
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Season::getNewWeather), getNewWeather_hook, &getNewWeather_orig))
    { log("hook Season::getNewWeather failed (weather sync only corrective)"); }
    return true;
}

} // namespace kmp
