/***************************************
 * mod_weather_vibe — GLOBAL environment weather controller (zone-wide)
 *
 * CONFIG PER-ZONE ENTRIES (array form):
 *   WeatherVibe.Zone.<ZoneId>[i] = [type, weight, min, max, minMinutes, maxMinutes, "description"]
 *     - type:       0=fine, 1=rain, 2=snow, 3=storm (sand/ash), 86=thunders
 *     - weight:     relative selection weight (0 disables)
 *     - min/max:    intensity in [0..1] (match core bands; see below)
 *     - min/maxMinutes: dwell time window for this pattern (real minutes)
 *     - description: free text; used in debug broadcasts
 *
 * CORE WEATHER BANDS (AzerothCore Weather.cpp):
 *   grade < 0.27 => WEATHER_STATE_FINE (even for non-fine types)
 *   0.27–0.39 => LIGHT, 0.40–0.69 => MEDIUM, 0.70–1.00 => HEAVY
 *   THUNDERS is a discrete state, driven by type=86 (grade still sent).
 *
 * GLOBAL SETTINGS:
 *   WeatherVibe.Enable = 1/0
 *   WeatherVibe.Interval = <seconds>              // engine tick
 *
 *   # Time (in seconds) to fade between weather patterns. 0 = instant
 *   WeatherVibe.TransitionTime.Min = 60
 *   WeatherVibe.TransitionTime.Max = 120
 *
 *   WeatherVibe.Jitter.Zone.Min = <seconds>       // per-zone extra delay
 *   WeatherVibe.Jitter.Zone.Max = <seconds>
 *   WeatherVibe.Seasons = "auto"|"off"|"spring"|"summer"|"fall"|"winter"
 *   WeatherVibe.Transition.NotAllowed[n] = [fromType, toType]
 *   WeatherVibe.Debug = 0/1                       // broadcast zone text on apply
 *
 * ENGINE BEHAVIOR (this module):
 *   - Keeps a palette of patterns per zone; each application picks one pattern
 *     with season-adjusted weights and (optionally) transition bans.
 *   - On selection, choose a random intensity within [min,max] and a dwell time
 *     in [minMinutes,maxMinutes] plus per-zone jitter (sec).
 *   - When switching patterns, the module cross-fades intensity from the old
 *     grade to the new target grade over TransitionTime (random in Min..Max).
 *     If the type changes (e.g., rain -> fine), we flip the type halfway
 *     through the fade for a natural handoff.
 *   - When dwell elapses, a new pattern is picked (respecting bans).
 *   - On each tick, we only push to the core if type changed, or intensity
 *     changed meaningfully (> epsilon).
 *   - If WeatherVibe.Debug=1, we broadcast a terse text description to players
 *     in the zone at the moment a new pattern is applied.
 ***************************************/
#include "ScriptMgr.h"
#include "World.h"
#include "Log.h"
#include "WeatherMgr.h"
#include "Weather.h"
#include "GameTime.h"
#include "Configuration/Config.h"
#include "Common.h"
#include "WorldSessionMgr.h"

#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>
#include <random>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <cmath>

namespace
{
    // Canonical type codes (match core WeatherType):
    // 0=fine, 1=rain, 2=snow, 3=storm/sand/ash, 86=thunders
    static constexpr uint32 kTypeCodes[5] = { 0u, 1u, 2u, 3u, 86u };

    static inline int TypeIndex(uint32 typeCode)
    {
        for (int i = 0; i < 5; ++i)
            if (kTypeCodes[i] == typeCode)
                return i;
        return 0;
    }

    static char const* TypeName(uint32 t)
    {
        switch (t)
        {
        case 0:   return "fine";
        case 1:   return "rain";
        case 2:   return "snow";
        case 3:   return "storm";
        case 86:  return "thunders";
        default:  return "unknown";
        }
    }

    struct Pattern
    {
        uint32 type = 0;       // 0,1,2,3,86
        float  weight = 0.0f;  // relative weight
        float  mn = 0.0f;      // intensity min [0..1]
        float  mx = 0.0f;      // intensity max [0..1]
        uint32 minMin = 1;     // min minutes
        uint32 maxMin = 5;     // max minutes
        std::string desc;      // human readable description

        bool enabled() const { return weight > 0.0f && mx >= mn + 0.00001f; }

        float clamp(float v) const
        {
            return std::min(std::max(v, mn), mx);
        }
    };

    struct ZoneProfile
    {
        std::vector<Pattern> patterns;
        bool empty() const
        {
            for (auto const& p : patterns)
                if (p.enabled())
                    return false;
            return true;
        }
    };

    struct ZoneState
    {
        bool   inited = false;

        // Currently displayed state
        uint32 typeCode = 0;
        float  grade = 0.0f;

        // Target within current pattern
        float  targetGrade = 0.0f;
        int    patternIndex = -1;

        // Dwell control
        time_t dwellUntil = 0;             // epoch seconds when we can change pattern
        time_t nextTickEligible = 0;       // per-zone jitter gate (epoch seconds)

        // --- Transition (time-based cross-fade) ---
        bool   transitionActive = false;
        time_t transitionStart = 0;       // epoch seconds
        time_t transitionEnd = 0;       // epoch seconds
        float  startGrade = 0.0f;    // grade at transition start
        uint32 nextTypeCode = 0;       // type of the next pattern
        bool   typeFlipPending = false;   // flip type once we reach 50% progress
    };

    // settings
    static bool   s_enable = true;
    static uint32 s_interval = 120;        // seconds
    static uint32 s_jitterMin = 1;         // seconds
    static uint32 s_jitterMax = 5;         // seconds
    static bool   s_debug = false;

    // NEW: time-based transition
    static uint32 s_transMinSec = 0;       // seconds
    static uint32 s_transMaxSec = 0;       // seconds

    enum class SeasonMode { Off, Auto, Spring, Summer, Fall, Winter };
    static SeasonMode s_seasonMode = SeasonMode::Auto;

    // Per-season multipliers by type index (0=fine,1=rain,2=snow,3=storm,4=thunders)
    static float const kSeasonMul[4][5] = {
        /* Spring */ { 0.90f, 1.25f, 0.60f, 1.00f, 1.10f },
        /* Summer */ { 1.20f, 0.90f, 0.20f, 1.20f, 1.30f },
        /* Fall   */ { 0.90f, 1.20f, 0.70f, 1.00f, 0.90f },
        /* Winter */ { 0.70f, 0.50f, 1.50f, 1.20f, 0.70f }
    };

    // Banned direct transitions (fromType, toType)
    static std::vector<std::pair<uint32, uint32>> s_bannedTransitions;

    // ZoneId -> profile/state
    static std::unordered_map<uint32, ZoneProfile> s_zoneProfiles;
    static std::unordered_map<uint32, ZoneState>   s_zoneState;

    // RNG
    static std::mt19937& Rng()
    {
        static std::mt19937 g(static_cast<uint32>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()));
        return g;
    }

    static inline float Clamp01(float v) { return std::min(std::max(v, 0.0f), 1.0f); }

    static float RandomIn(float a, float b)
    {
        if (b < a) std::swap(a, b);
        std::uniform_real_distribution<float> dist(a, b);
        return dist(Rng());
    }

    static uint32 RandomInUInt(uint32 a, uint32 b)
    {
        if (b < a) std::swap(b, a);
        if (a == b) return a;
        std::uniform_int_distribution<uint32> D(a, b);
        return D(Rng());
    }

    static bool IsTransitionBanned(uint32 fromType, uint32 toType)
    {
        for (auto const& pr : s_bannedTransitions)
            if (pr.first == fromType && pr.second == toType)
                return true;
        return false;
    }

    // Compute season index like the core (0=spring,1=summer,2=fall,3=winter)
    static int CurrentSeasonIndex()
    {
        time_t now = GameTime::GetGameTime().count();
        tm* g = std::gmtime(&now);
        int day = g ? (g->tm_yday + 1) : 1;
        int season = ((day - 78 + 365) / 91) % 4;
        if (season < 0) season += 4;
        return season;
    }

    static int ResolveSeasonIndex()
    {
        switch (s_seasonMode)
        {
        case SeasonMode::Off:    return -1;
        case SeasonMode::Auto:   return CurrentSeasonIndex();
        case SeasonMode::Spring: return 0;
        case SeasonMode::Summer: return 1;
        case SeasonMode::Fall:   return 2;
        case SeasonMode::Winter: return 3;
        }
        return -1;
    }

    static float SeasonAdjustedWeight(Pattern const& p)
    {
        int si = ResolveSeasonIndex();
        if (si < 0) return p.weight;

        int ti = TypeIndex(p.type);
        float mul = kSeasonMul[si][ti];
        if (p.weight <= 0.0f) return 0.0f;

        float w = p.weight * mul;
        return w < 0.0f ? 0.0f : w;
    }

    // Ensure Weather object exists for a zone
    static Weather* GetZoneWeather(uint32 zoneId)
    {
        if (Weather* w = WeatherMgr::FindWeather(zoneId))
            return w;
        if (WeatherMgr::AddWeather(zoneId))
            return WeatherMgr::FindWeather(zoneId);
        return nullptr;
    }

    // Push (type,grade) to core + DEBUG BROADCAST HERE
    static void PushToCore(uint32 zoneId, uint32 typeCode, float grade)
    {
        if (Weather* w = GetZoneWeather(zoneId))
        {
            float g = Clamp01(grade);
            w->SetWeather(static_cast<WeatherType>(typeCode), g);

            if (s_debug)
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                    "[WeatherVibe] Zone %u update → %s (grade %.2f)",
                    zoneId, TypeName(typeCode), g);
                WorldSessionMgr::Instance()->SendZoneText(zoneId, buf);
                LOG_INFO("weather", "{}", buf);
            }
        }
    }

    // Pick a new pattern for zone, honoring season multipliers and transition bans
    static int PickPattern(uint32 /*zoneId*/, ZoneProfile const& Z, uint32 fromType)
    {
        float total = 0.0f;
        std::vector<float> eff;
        eff.reserve(Z.patterns.size());
        for (auto const& p : Z.patterns)
        {
            float w = p.enabled() ? SeasonAdjustedWeight(p) : 0.0f;
            if (w > 0.0f && IsTransitionBanned(fromType, p.type))
                w = 0.0f;
            eff.push_back(w);
            total += w;
        }

        if (total <= 0.0001f)
        {
            // fallback: allow anything enabled ignoring bans
            total = 0.0f;
            for (size_t i = 0; i < Z.patterns.size(); ++i)
            {
                float w = Z.patterns[i].enabled() ? Z.patterns[i].weight : 0.0f;
                eff[i] = w;
                total += w;
            }
            if (total <= 0.0001f)
                return -1;
        }

        std::uniform_real_distribution<float> U(0.f, total);
        float r = U(Rng());
        float acc = 0.f;
        for (size_t i = 0; i < eff.size(); ++i)
        {
            acc += eff[i];
            if (r <= acc)
                return static_cast<int>(i);
        }
        // Shouldn't get here, but just in case:
        for (int i = static_cast<int>(eff.size()) - 1; i >= 0; --i)
            if (eff[i] > 0.0f)
                return i;

        return -1;
    }

    static uint32 RandomJitterSec()
    {
        if (s_jitterMax < s_jitterMin)
            std::swap(s_jitterMax, s_jitterMin);
        return RandomInUInt(s_jitterMin, s_jitterMax);
    }

    static void StartPattern(uint32 zoneId, ZoneProfile const& Z, ZoneState& S, int patIndex)
    {
        if (patIndex < 0 || patIndex >= static_cast<int>(Z.patterns.size()))
            return;

        Pattern const& P = Z.patterns[patIndex];

        S.patternIndex = patIndex;

        // Choose a target grade in range
        float target = RandomIn(P.mn, P.mx);

        uint32 dwellMin = std::max<uint32>(1, P.minMin);
        uint32 dwellMax = std::max<uint32>(dwellMin, P.maxMin);
        uint32 dwellMinSec = dwellMin * 60u;
        uint32 dwellMaxSec = dwellMax * 60u;
        uint32 dwellSec = RandomInUInt(dwellMinSec, dwellMaxSec) + RandomJitterSec();

        time_t now = GameTime::GetGameTime().count();
        S.dwellUntil = now + dwellSec;
        S.nextTickEligible = now + RandomJitterSec();

        // --- Transition setup (time-based) ---
        uint32 transMin = s_transMinSec;
        uint32 transMax = s_transMaxSec;
        if (transMax < transMin) std::swap(transMax, transMin);
        uint32 transDur = RandomInUInt(transMin, transMax);

        if (!S.inited)
        {
            // First-time bootstrap: snap to the selected pattern
            S.typeCode = P.type;
            S.grade = P.clamp(target);
            S.targetGrade = S.grade;
            S.transitionActive = false;
            S.typeFlipPending = false;
            S.inited = true;
            PushToCore(zoneId, S.typeCode, S.grade);
        }
        else
        {
            // Prepare cross-fade from current to new target
            S.startGrade = S.grade;
            S.targetGrade = P.clamp(target);
            S.nextTypeCode = P.type;
            S.typeFlipPending = (S.nextTypeCode != S.typeCode);

            if (transDur == 0)
            {
                // Instant
                S.grade = S.targetGrade;
                S.typeCode = S.nextTypeCode;
                S.transitionActive = false;
                S.typeFlipPending = false;
                PushToCore(zoneId, S.typeCode, S.grade);
            }
            else
            {
                S.transitionStart = now;
                S.transitionEnd = now + transDur;
                S.transitionActive = true;
            }
        }
    }

    static bool ShouldTickZone(ZoneState const& S, time_t now)
    {
        return now >= S.nextTickEligible;
    }

    static void BumpNextTick(ZoneState& S, time_t now)
    {
        S.nextTickEligible = now + RandomJitterSec();
    }

    // Per-tick evolution: handle transition fade; when dwell expires, pick new pattern
    static void StepZone(uint32 zoneId, ZoneProfile const& Z, ZoneState& S)
    {
        if (Z.empty())
            return;

        time_t now = GameTime::GetGameTime().count();
        if (!ShouldTickZone(S, now))
            return;

        BumpNextTick(S, now);

        if (!S.inited)
        {
            int pi = PickPattern(zoneId, Z, /*fromType*/ 0);
            StartPattern(zoneId, Z, S, pi);
            return;
        }

        float oldGrade = S.grade;
        uint32 oldType = S.typeCode;

        // Handle active transition fade
        if (S.transitionActive)
        {
            // Compute progress in [0..1]
            double total = std::max<time_t>(1, S.transitionEnd - S.transitionStart);
            double done = std::clamp<double>(now - S.transitionStart, 0, total);
            float  t = static_cast<float>(done / total);

            // Lerp grade
            S.grade = S.startGrade + (S.targetGrade - S.startGrade) * t;

            // Flip type at halfway point if pending
            if (S.typeFlipPending && t >= 0.5f)
            {
                S.typeCode = S.nextTypeCode;
                S.typeFlipPending = false;
            }

            // End of transition
            if (now >= S.transitionEnd)
            {
                S.grade = S.targetGrade;
                S.typeCode = S.nextTypeCode;
                S.transitionActive = false;
                S.typeFlipPending = false;
            }

            // Push if meaningful change
            if (std::fabs(S.grade - oldGrade) > 0.005f || S.typeCode != oldType)
                PushToCore(zoneId, S.typeCode, S.grade);

            return;
        }

        // If still within dwell, nothing to change
        if (now < S.dwellUntil && S.patternIndex >= 0 && S.patternIndex < static_cast<int>(Z.patterns.size()))
        {
            // No-op (grade stays at target during dwell)
            return;
        }

        // Dwell elapsed: pick new pattern (respect bans)
        int next = PickPattern(zoneId, Z, /*fromType*/ S.typeCode);
        if (next < 0)
            next = S.patternIndex; // fallback to previous

        StartPattern(zoneId, Z, S, next);

        // If we snapped instantly just now, ensure a push occurred:
        if (!S.transitionActive && (std::fabs(S.grade - oldGrade) > 0.005f || S.typeCode != oldType))
            PushToCore(zoneId, S.typeCode, S.grade);
    }

    // --- CONFIG LOADING ---

    static int ToLower(int c) { return std::tolower(c); }

    static SeasonMode ParseSeasonMode(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), ToLower);
        if (s == "off")    return SeasonMode::Off;
        if (s == "auto")   return SeasonMode::Auto;
        if (s == "spring") return SeasonMode::Spring;
        if (s == "summer") return SeasonMode::Summer;
        if (s == "fall")   return SeasonMode::Fall;
        if (s == "winter") return SeasonMode::Winter;
        return SeasonMode::Auto;
    }

    static bool ParseNotAllowed(std::string const& val, uint32& fromT, uint32& toT)
    {
        // Expect something like: [2, 1]
        int a = -1, b = -1;
        int n = std::sscanf(val.c_str(), " [ %d , %d ] ", &a, &b);
        if (n == 2 && a >= 0 && b >= 0)
        {
            fromT = static_cast<uint32>(a);
            toT = static_cast<uint32>(b);
            return true;
        }
        return false;
    }

    static std::string ExtractQuoted(std::string const& s)
    {
        size_t first = s.find('"');
        if (first == std::string::npos) return {};
        size_t last = s.find_last_of('"');
        if (last == std::string::npos || last <= first) return {};
        return s.substr(first + 1, last - first - 1);
    }

    static bool ParsePatternArray(std::string val, Pattern& out)
    {
        // Grab description (quoted), then remove it from the string
        std::string desc = ExtractQuoted(val);

        // Remove quotes content (best-effort)
        if (!desc.empty())
        {
            size_t q1 = val.find('"');
            size_t q2 = val.find_last_of('"');
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1)
            {
                std::string before = val.substr(0, q1);
                std::string after = val.substr(q2 + 1);
                val = before + after;
            }
        }

        // Strip brackets
        auto strip = [](std::string& s)
            {
                s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c)
                    {
                        return c == '[' || c == ']';
                    }), s.end());
            };
        strip(val);

        // Now parse up to 6 comma-separated scalars: type, weight, min, max, minMin, maxMin
        int itype = 0;
        float w = 0.f, mn = 0.f, mx = 0.f;
        uint32 minMin = 1, maxMin = 5;

        int n = std::sscanf(val.c_str(), " %d , %f , %f , %f , %u , %u ", &itype, &w, &mn, &mx, &minMin, &maxMin);
        if (n < 4)
            return false;
        if (n == 4)
        {
            minMin = 1;
            maxMin = 5;
        }

        out.type = static_cast<uint32>(itype);
        out.weight = std::max(0.0f, w);
        out.mn = Clamp01(mn);
        out.mx = Clamp01(mx);
        if (out.mx < out.mn) std::swap(out.mx, out.mn);
        out.minMin = std::max<uint32>(1, minMin);
        out.maxMin = std::max<uint32>(out.minMin, maxMin);
        out.desc = desc;
        return true;
    }

    static void LoadConfig()
    {
        s_enable = sConfigMgr->GetOption<bool>("WeatherVibe.Enable", true);
        s_interval = sConfigMgr->GetOption<uint32>("WeatherVibe.Interval", 120u);

        // NEW: transition time (seconds)
        s_transMinSec = sConfigMgr->GetOption<uint32>("WeatherVibe.TransitionTime.Min", 0u);
        s_transMaxSec = sConfigMgr->GetOption<uint32>("WeatherVibe.TransitionTime.Max", 0u);

        s_jitterMin = sConfigMgr->GetOption<uint32>("WeatherVibe.Jitter.Zone.Min", 1u);
        s_jitterMax = sConfigMgr->GetOption<uint32>("WeatherVibe.Jitter.Zone.Max", 5u);
        s_debug = sConfigMgr->GetOption<bool>("WeatherVibe.Debug", false);
        s_seasonMode = ParseSeasonMode(sConfigMgr->GetOption<std::string>("WeatherVibe.Seasons", "auto"));

        // Transition bans
        s_bannedTransitions.clear();
        auto banKeys = sConfigMgr->GetKeysByString("WeatherVibe.Transition.NotAllowed");
        for (auto const& key : banKeys)
        {
            std::string val = sConfigMgr->GetOption<std::string>(key, "");
            if (val.empty())
                continue;
            uint32 a, b;
            if (ParseNotAllowed(val, a, b))
                s_bannedTransitions.emplace_back(a, b);
        }

        // Zone patterns
        s_zoneProfiles.clear();
        auto keys = sConfigMgr->GetKeysByString("WeatherVibe.Zone.");
        const std::string prefix = "WeatherVibe.Zone.";

        for (auto const& key : keys)
        {
            if (key.rfind(prefix, 0) != 0)
                continue;

            // Expect "WeatherVibe.Zone.<zoneId>[i]"
            size_t posAfterPrefix = prefix.size();

            // find '[' that starts the [i]
            size_t lb = key.find('[', posAfterPrefix);
            size_t rb = (lb != std::string::npos) ? key.find(']', lb) : std::string::npos;
            if (lb == std::string::npos || rb == std::string::npos || rb <= lb + 1)
                continue;

            // The zone id is the digits between prefix and '['
            std::string zoneStr = key.substr(posAfterPrefix, lb - posAfterPrefix);
            if (zoneStr.empty() || !std::all_of(zoneStr.begin(), zoneStr.end(), [](unsigned char c) { return std::isdigit(c); }))
                continue;

            // index is inside [ ... ]
            std::string idxStr = key.substr(lb + 1, rb - lb - 1);
            if (idxStr.empty() || !std::all_of(idxStr.begin(), idxStr.end(), [](unsigned char c) { return std::isdigit(c); }))
                continue;

            uint32 zoneId = static_cast<uint32>(std::strtoul(zoneStr.c_str(), nullptr, 10));
            int    patIdx = static_cast<int>(std::strtoul(idxStr.c_str(), nullptr, 10));

            std::string val = sConfigMgr->GetOption<std::string>(key, "");
            if (val.empty())
                continue;

            Pattern P;
            if (!ParsePatternArray(val, P))
                continue;

            ZoneProfile& Z = s_zoneProfiles[zoneId];
            if (static_cast<int>(Z.patterns.size()) <= patIdx)
                Z.patterns.resize(patIdx + 1);
            Z.patterns[patIdx] = P;
        }

        LOG_INFO("server.loading",
            "[mod_weather_vibe] config loaded: {} zones (interval={}s, transition={}..{}s, jitter={}..{}s, seasonMode={}, debug={}).",
            s_zoneProfiles.size(), s_interval, s_transMinSec, s_transMaxSec, s_jitterMin, s_jitterMax,
            (s_seasonMode == SeasonMode::Off ? "off" :
                s_seasonMode == SeasonMode::Auto ? "auto" :
                s_seasonMode == SeasonMode::Spring ? "spring" :
                s_seasonMode == SeasonMode::Summer ? "summer" :
                s_seasonMode == SeasonMode::Fall ? "fall" : "winter"),
            s_debug);
    }

    class ModWeatherVibeWorldScript : public WorldScript
    {
    public:
        ModWeatherVibeWorldScript() : WorldScript("ModWeatherVibeWorldScript"), m_timer(0) {}

        void OnStartup() override
        {
            LoadConfig();

            // Bootstrap each configured zone once so world isn't empty
            for (auto& kv : s_zoneProfiles)
            {
                uint32 zoneId = kv.first;
                ZoneProfile const& Z = kv.second;
                ZoneState& S = s_zoneState[zoneId];
                if (!Z.empty())
                {
                    int pi = PickPattern(zoneId, Z, /*fromType*/ 0);
                    StartPattern(zoneId, Z, S, pi);
                }
            }

            LOG_INFO("server.loading", "[mod_weather_vibe] initialized (array patterns + dwell + season bias + timed transitions).");
        }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            LoadConfig();
            // Do not forcibly reset current states; let them expire naturally
        }

        void OnUpdate(uint32 diff) override
        {
            if (!s_enable)
                return;

            m_timer += diff;
            if (m_timer < s_interval * IN_MILLISECONDS)
                return;
            m_timer = 0;

            // Tick each zone independently
            for (auto& kv : s_zoneProfiles)
            {
                uint32 zoneId = kv.first;
                ZoneProfile const& Z = kv.second;
                if (Z.empty())
                    continue;

                ZoneState& S = s_zoneState[zoneId];
                StepZone(zoneId, Z, S);
            }
        }

    private:
        uint32 m_timer;
    };
}

void Addmod_weather_vibeScripts()
{
    new ModWeatherVibeWorldScript();
}
