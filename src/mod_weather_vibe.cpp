/***************************************
 * mod_weather_vibe
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
#include <cstdarg>

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
        float clamp01(float v) const { return std::min(std::max(v, 0.0f), 1.0f); }
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
        time_t transitionStart = 0;        // epoch seconds
        time_t transitionEnd = 0;          // epoch seconds
        float  startGrade = 0.0f;          // grade at transition start
        uint32 nextTypeCode = 0;           // type of the next pattern
        bool   typeFlipPending = false;    // flip type mid-crossfade

        // --- Thunder prelude (in-dwell) ---
        bool   thunderPreludeActive = false;     // we're currently ramping rain before thunders
        int    thunderPreludeNextPatIndex = -1;  // the chosen thunders pattern index
        float  thunderTargetGrade = 0.0f;        // final thunders target after prelude

        // --- Storm outro (in-dwell) ---
        bool   stormOutroPrimed = false;         // outro started within current dwell
        int    stormOutroLockedNext = -1;        // pre-picked next pattern to apply at dwell end
    };

    // =========================
    // Settings / configuration
    // =========================
    static bool   s_enable = true;
    static uint32 s_interval = 120;        // seconds
    static uint32 s_jitterMin = 1;         // seconds
    static uint32 s_jitterMax = 5;         // seconds
    static bool   s_debug = false;

    // Time-based transition
    static uint32 s_transMinSec = 0;       // seconds
    static uint32 s_transMaxSec = 0;       // seconds

    enum class SeasonMode { Off, Auto, Spring, Summer, Fall, Winter };
    static SeasonMode s_seasonMode = SeasonMode::Auto;

    // Season multipliers by type index (0=fine,1=rain,2=snow,3=storm,4=thunders)
    // Defaults match original; can be overridden from .conf
    static float s_seasonMul[4][5] = {
        /* Spring */ { 0.90f, 1.25f, 0.60f, 1.00f, 1.10f },
        /* Summer */ { 1.20f, 0.90f, 0.20f, 1.20f, 1.30f },
        /* Fall   */ { 0.90f, 1.20f, 0.70f, 1.00f, 0.90f },
        /* Winter */ { 0.70f, 0.50f, 1.50f, 1.20f, 0.70f }
    };

    // Thunder Prelude params (always-on, IN-DWELL)
    static float  s_thPreRainMin = 0.45f;
    static float  s_thPreRainMax = 0.80f;
    static uint32 s_thPreDurMin = 30u;   // seconds
    static uint32 s_thPreDurMax = 90u;   // seconds
    static bool   s_thPreRaiseIfRaining = true;

    // Storm Outro params (always-on, IN-DWELL)
    static bool   s_stormOutroIncludeThunders = true;
    static float  s_stormOutroTarget = 0.32f; // 0..1; >0.27 keeps visuals visible
    static uint32 s_stormOutroDurMin = 20u;
    static uint32 s_stormOutroDurMax = 60u;

    // ===== CLIME VARIATION (NEW) =====
    static uint32 s_climeVarMinutes = 0u;     // 0 = disabled
    static float  s_climeWeightPct = 0.0f;   // e.g., 0.15 => ±15% on weight
    static float  s_climeIntensityAbs = 0.0f;   // e.g., 0.05 => shift ±0.05 on mn/mx

    struct ClimeState
    {
        bool  inited = false;
        float weightMul[5] = { 1,1,1,1,1 };   // multiplicative (>=0)
        float intShift[5] = { 0,0,0,0,0 };   // additive (can be ±)
        time_t nextUpdate = 0;
    };

    // ZoneId -> profile/state
    static std::unordered_map<uint32, ZoneProfile> s_zoneProfiles;
    static std::unordered_map<uint32, ZoneState>   s_zoneState;
    static std::unordered_map<uint32, ClimeState>  s_climeByZone;

    // =========================
    // Utilities / helpers
    // =========================

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

    // Read a season multiplier from config with a safe default and clamping
    static float ReadMul(char const* season, char const* type, float def)
    {
        std::string key = "WeatherVibe.Season.";
        key += season; key += "."; key += type; key += ".Mul";
        float v = sConfigMgr->GetOption<float>(key, def);
        return v < 0.0f ? 0.0f : v; // never negative
    }

    // ---------- CLIME VARIATION CORE ----------
    static void UpdateClimeIfDue(uint32 zoneId)
    {
        if (s_climeVarMinutes == 0u || (s_climeWeightPct <= 0.0f && s_climeIntensityAbs <= 0.0f))
            return;

        ClimeState& C = s_climeByZone[zoneId];
        time_t now = GameTime::GetGameTime().count();

        if (!C.inited || now >= C.nextUpdate)
        {
            // New random biases per type for the next window
            for (int ti = 0; ti < 5; ++ti)
            {
                if (s_climeWeightPct > 0.0f)
                {
                    float delta = RandomIn(-s_climeWeightPct, s_climeWeightPct);
                    C.weightMul[ti] = std::max(0.0f, 1.0f + delta);
                }
                else
                {
                    C.weightMul[ti] = 1.0f;
                }

                if (s_climeIntensityAbs > 0.0f)
                {
                    C.intShift[ti] = RandomIn(-s_climeIntensityAbs, s_climeIntensityAbs);
                }
                else
                {
                    C.intShift[ti] = 0.0f;
                }
            }

            // Next window (add a tiny jitter so zones don't flip together)
            C.nextUpdate = now + s_climeVarMinutes * 60u + RandomInUInt(0, 5);
            C.inited = true;

            if (s_debug)
                LOG_INFO("weather", "[WeatherVibe] Clime update zone {} (weight ±{:.2f}%, intensity ±{:.2f})",
                    zoneId, s_climeWeightPct * 100.0f, s_climeIntensityAbs);
        }
    }

    static float ClimeWeightMul(uint32 zoneId, int typeIdx)
    {
        if (s_climeVarMinutes == 0u || s_climeWeightPct <= 0.0f)
            return 1.0f;
        UpdateClimeIfDue(zoneId);
        auto it = s_climeByZone.find(zoneId);
        if (it == s_climeByZone.end()) return 1.0f;
        return std::max(0.0f, it->second.weightMul[typeIdx]);
    }

    static float ClimeIntensityShift(uint32 zoneId, int typeIdx)
    {
        if (s_climeVarMinutes == 0u || s_climeIntensityAbs <= 0.0f)
            return 0.0f;
        UpdateClimeIfDue(zoneId);
        auto it = s_climeByZone.find(zoneId);
        if (it == s_climeByZone.end()) return 0.0f;
        return it->second.intShift[typeIdx];
    }
    // ------------------------------------------

    static float SeasonAdjustedWeight(int seasonIdx, int typeIdx, float baseWeight)
    {
        if (baseWeight <= 0.0f) return 0.0f;
        if (seasonIdx < 0) return baseWeight;
        float mul = s_seasonMul[seasonIdx][typeIdx];
        float w = baseWeight * mul;
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

    // Push (type,grade) to core + debug broadcast
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

    static void DebugBroadcast(uint32 zoneId, char const* fmt, ...)
    {
        if (!s_debug) return;
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        WorldSessionMgr::Instance()->SendZoneText(zoneId, buf);
        LOG_INFO("weather", "{}", buf);
    }

    // Weighted random pick (season + clime biased)
    static int PickPattern(uint32 zoneId, ZoneProfile const& Z)
    {
        int seasonIdx = ResolveSeasonIndex();

        // Ensure clime state is up-to-date before we compute weights
        UpdateClimeIfDue(zoneId);

        float total = 0.0f;
        std::vector<float> eff;
        eff.reserve(Z.patterns.size());

        for (auto const& p : Z.patterns)
        {
            float w = p.enabled() ? SeasonAdjustedWeight(seasonIdx, TypeIndex(p.type), p.weight) : 0.0f;
            if (w > 0.0f)
                w *= ClimeWeightMul(zoneId, TypeIndex(p.type)); // multiplicative daily drift
            eff.push_back(w);
            total += w;
        }

        if (total <= 0.0001f)
        {
            // fallback: allow anything enabled using raw weights
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

    // =========================
    // Core engine: transitions
    // =========================

    static void BeginTransition(ZoneState& S, uint32 toType, float toGrade, uint32 durSec, bool flipTypeMidway, bool keepTypeUntilEnd)
    {
        // Configure a time-based cross-fade from current grade/type to target
        S.startGrade = S.grade;
        S.targetGrade = Clamp01(toGrade);
        S.nextTypeCode = toType;

        if (durSec == 0)
        {
            // Instant
            if (!keepTypeUntilEnd) S.typeCode = toType;
            S.grade = S.targetGrade;
            S.transitionActive = false;
            S.typeFlipPending = false;
        }
        else
        {
            time_t now = GameTime::GetGameTime().count();
            S.transitionStart = now;
            S.transitionEnd = now + durSec;
            S.transitionActive = true;
            S.typeFlipPending = flipTypeMidway && !keepTypeUntilEnd && (toType != S.typeCode);
        }
    }

    static void StartPattern(uint32 zoneId, ZoneProfile const& Z, ZoneState& S, int patIndex)
    {
        if (patIndex < 0 || patIndex >= static_cast<int>(Z.patterns.size()))
            return;

        Pattern const& P = Z.patterns[patIndex];
        S.patternIndex = patIndex;

        // Resolve global transition duration
        uint32 transMin = s_transMinSec;
        uint32 transMax = s_transMaxSec;
        if (transMax < transMin) std::swap(transMax, transMin);
        uint32 transDur = RandomInUInt(transMin, transMax);

        time_t now = GameTime::GetGameTime().count();

        // Compute the dwell window for this pattern upfront
        uint32 dwellMin = std::max<uint32>(1, P.minMin);
        uint32 dwellMax = std::max<uint32>(dwellMin, P.maxMin);
        uint32 dwellMinSec = dwellMin * 60u;
        uint32 dwellMaxSec = dwellMax * 60u;
        uint32 dwellSec = RandomInUInt(dwellMinSec, dwellMaxSec) + RandomJitterSec();

        // Default: schedule dwell for the new pattern
        S.dwellUntil = now + dwellSec;
        S.nextTickEligible = now + RandomJitterSec();

        // ---- Apply CLIME intensity shift when choosing the target grade ----
        float shift = ClimeIntensityShift(zoneId, TypeIndex(P.type));
        float adjMn = Clamp01(P.mn + shift);
        float adjMx = Clamp01(P.mx + shift);
        if (adjMx < adjMn) std::swap(adjMx, adjMn);
        // keep a tiny width to avoid a collapsed range
        if (adjMx < adjMn + 0.02f) adjMx = std::min(1.0f, adjMn + 0.02f);

        // Pick final target grade for this pattern from adjusted range
        float finalTarget = Clamp01(RandomIn(adjMn, adjMx));

        // --- Thunder Prelude (IN-DWELL): entering thunders → rain ramp first, within the same dwell
        if (P.type == 86u)
        {
            bool alreadyRaining = (S.inited && S.typeCode == 1u);
            bool strongEnough = (alreadyRaining && (!s_thPreRaiseIfRaining || S.grade + 1e-4f >= s_thPreRainMin));
            if (!strongEnough)
            {
                // Prelude: ramp to rain at preTarget for a short duration, inside this dwell
                float preTarget = Clamp01(RandomIn(s_thPreRainMin, s_thPreRainMax));
                uint32 preDur = RandomInUInt(s_thPreDurMin, s_thPreDurMax);

                // Store the thunders target we will go to after the ramp
                S.thunderTargetGrade = finalTarget;
                S.thunderPreludeActive = true;
                S.thunderPreludeNextPatIndex = patIndex; // informational

                if (!S.inited)
                {
                    // Bootstrap directly into rain prelude
                    S.typeCode = 1u;
                    S.grade = preTarget;
                    S.transitionActive = false;
                    S.typeFlipPending = false;
                    S.inited = true;
                    PushToCore(zoneId, S.typeCode, S.grade);
                }
                else
                {
                    BeginTransition(S, /*toType=*/1u, /*toGrade=*/preTarget, /*durSec=*/preDur,
                        /*flipTypeMidway=*/true, /*keepTypeUntilEnd=*/false);
                }

                DebugBroadcast(zoneId, "[WeatherVibe] Zone %u prelude (in-dwell) → rain ramp before thunders (→%.2f for %us)",
                    zoneId, preTarget, preDur);
                return; // After the ramp finishes, we'll cross-fade to thunders without changing dwell
            }
            // else: already raining sufficiently; fall through to direct thunders cross-fade inside dwell
        }

        // --- Standard pattern application (or direct thunders if no prelude needed)
        if (!S.inited)
        {
            // First-time bootstrap: snap to selected pattern
            S.typeCode = P.type;
            S.grade = finalTarget;
            S.targetGrade = S.grade;
            S.transitionActive = false;
            S.typeFlipPending = false;
            S.inited = true;
            PushToCore(zoneId, S.typeCode, S.grade);
        }
        else
        {
            BeginTransition(S, /*toType=*/P.type, /*toGrade=*/finalTarget, /*durSec=*/transDur,
                /*flipTypeMidway=*/true, /*keepTypeUntilEnd=*/false);
            if (!S.transitionActive)
                PushToCore(zoneId, S.typeCode, S.grade);
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
            int pi = PickPattern(zoneId, Z);
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

                // Chain (IN-DWELL): Thunder Prelude completion → cross-fade to thunders within the same dwell
                if (S.thunderPreludeActive)
                {
                    S.thunderPreludeActive = false;
                    // Cross-fade into thunders with normal transition time; dwellUntil remains unchanged
                    uint32 transMin = s_transMinSec, transMax = s_transMaxSec;
                    if (transMax < transMin) std::swap(transMax, transMin);
                    uint32 transDur = RandomInUInt(transMin, transMax);

                    BeginTransition(S, /*toType=*/86u, /*toGrade=*/Clamp01(S.thunderTargetGrade),
                        /*durSec=*/transDur, /*flipTypeMidway=*/true, /*keepTypeUntilEnd=*/false);

                    DebugBroadcast(zoneId, "[WeatherVibe] Zone %u prelude complete → cross-fade to thunders over %us",
                        zoneId, transDur);

                    if (!S.transitionActive)
                        PushToCore(zoneId, S.typeCode, S.grade);

                    return; // still in same dwell
                }
            }

            // Push if meaningful change
            if (std::fabs(S.grade - oldGrade) > 0.005f || S.typeCode != oldType)
                PushToCore(zoneId, S.typeCode, S.grade);

            return;
        }

        // --- IN-DWELL STORM OUTRO PRIMER ---
        if (now < S.dwellUntil && S.patternIndex >= 0 && S.patternIndex < static_cast<int>(Z.patterns.size()))
        {
            bool canOutroType = (S.typeCode == 3u) || (s_stormOutroIncludeThunders && S.typeCode == 86u);
            if (canOutroType && !S.stormOutroPrimed)
            {
                uint32 maxOutro = RandomInUInt(s_stormOutroDurMin, s_stormOutroDurMax);
                time_t remaining = S.dwellUntil - now;

                if (remaining > 5 && remaining <= static_cast<time_t>(maxOutro))
                {
                    // Pre-pick next pattern and ensure it's calmer (not storm/thunders)
                    int next = PickPattern(zoneId, Z);
                    if (next >= 0)
                    {
                        uint32 nextType = Z.patterns[next].type;
                        bool calmer = (nextType != 3u && nextType != 86u);
                        if (calmer)
                        {
                            BeginTransition(S, /*toType=*/S.typeCode,
                                /*toGrade=*/Clamp01(s_stormOutroTarget),
                                /*durSec=*/static_cast<uint32>(remaining),
                                /*flipTypeMidway=*/false, /*keepTypeUntilEnd=*/true);
                            S.stormOutroPrimed = true;
                            S.stormOutroLockedNext = next;
                            DebugBroadcast(zoneId, "[WeatherVibe] Zone %u outro (in-dwell) → easing %s to %.2f over %lds",
                                zoneId, TypeName(S.typeCode), s_stormOutroTarget, long(remaining));
                        }
                    }
                }
            }

            // Keep dwelling
            return;
        }

        // --- Dwell elapsed: pick (or use locked) next pattern ---
        int next = (S.stormOutroPrimed && S.stormOutroLockedNext >= 0)
            ? S.stormOutroLockedNext
            : PickPattern(zoneId, Z);

        S.stormOutroPrimed = false;
        S.stormOutroLockedNext = -1;

        if (next < 0)
            next = S.patternIndex; // fallback to previous

        StartPattern(zoneId, Z, S, next);

        // If we snapped instantly just now, ensure a push occurred:
        if (!S.transitionActive && (std::fabs(S.grade - oldGrade) > 0.005f || S.typeCode != oldType))
            PushToCore(zoneId, S.typeCode, S.grade);
    }

    // =========================
    // Config parsing / loading
    // =========================

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

        // Transition time (seconds)
        s_transMinSec = sConfigMgr->GetOption<uint32>("WeatherVibe.TransitionTime.Min", 0u);
        s_transMaxSec = sConfigMgr->GetOption<uint32>("WeatherVibe.TransitionTime.Max", 0u);

        s_jitterMin = sConfigMgr->GetOption<uint32>("WeatherVibe.Jitter.Zone.Min", 1u);
        s_jitterMax = sConfigMgr->GetOption<uint32>("WeatherVibe.Jitter.Zone.Max", 5u);
        s_debug = sConfigMgr->GetOption<bool>("WeatherVibe.Debug", false);
        s_seasonMode = ParseSeasonMode(sConfigMgr->GetOption<std::string>("WeatherVibe.Seasons", "auto"));

        // Season multipliers (configurable). Types: Fine,Rain,Snow,Storm,Thunders
        s_seasonMul[0][0] = ReadMul("Spring", "Fine", s_seasonMul[0][0]);
        s_seasonMul[0][1] = ReadMul("Spring", "Rain", s_seasonMul[0][1]);
        s_seasonMul[0][2] = ReadMul("Spring", "Snow", s_seasonMul[0][2]);
        s_seasonMul[0][3] = ReadMul("Spring", "Storm", s_seasonMul[0][3]);
        s_seasonMul[0][4] = ReadMul("Spring", "Thunders", s_seasonMul[0][4]);

        s_seasonMul[1][0] = ReadMul("Summer", "Fine", s_seasonMul[1][0]);
        s_seasonMul[1][1] = ReadMul("Summer", "Rain", s_seasonMul[1][1]);
        s_seasonMul[1][2] = ReadMul("Summer", "Snow", s_seasonMul[1][2]);
        s_seasonMul[1][3] = ReadMul("Summer", "Storm", s_seasonMul[1][3]);
        s_seasonMul[1][4] = ReadMul("Summer", "Thunders", s_seasonMul[1][4]);

        s_seasonMul[2][0] = ReadMul("Fall", "Fine", s_seasonMul[2][0]);
        s_seasonMul[2][1] = ReadMul("Fall", "Rain", s_seasonMul[2][1]);
        s_seasonMul[2][2] = ReadMul("Fall", "Snow", s_seasonMul[2][2]);
        s_seasonMul[2][3] = ReadMul("Fall", "Storm", s_seasonMul[2][3]);
        s_seasonMul[2][4] = ReadMul("Fall", "Thunders", s_seasonMul[2][4]);

        s_seasonMul[3][0] = ReadMul("Winter", "Fine", s_seasonMul[3][0]);
        s_seasonMul[3][1] = ReadMul("Winter", "Rain", s_seasonMul[3][1]);
        s_seasonMul[3][2] = ReadMul("Winter", "Snow", s_seasonMul[3][2]);
        s_seasonMul[3][3] = ReadMul("Winter", "Storm", s_seasonMul[3][3]);
        s_seasonMul[3][4] = ReadMul("Winter", "Thunders", s_seasonMul[3][4]);

        // Thunder Prelude params (always-on, IN-DWELL)
        s_thPreRainMin = Clamp01(sConfigMgr->GetOption<float>("WeatherVibe.ThunderPrelude.RainIntensity.Min", 0.45f));
        s_thPreRainMax = Clamp01(sConfigMgr->GetOption<float>("WeatherVibe.ThunderPrelude.RainIntensity.Max", 0.80f));
        if (s_thPreRainMax < s_thPreRainMin) std::swap(s_thPreRainMax, s_thPreRainMin);
        s_thPreDurMin = sConfigMgr->GetOption<uint32>("WeatherVibe.ThunderPrelude.Duration.Min", 30u);
        s_thPreDurMax = sConfigMgr->GetOption<uint32>("WeatherVibe.ThunderPrelude.Duration.Max", 90u);
        if (s_thPreDurMax < s_thPreDurMin) std::swap(s_thPreDurMax, s_thPreDurMin);
        s_thPreRaiseIfRaining = sConfigMgr->GetOption<bool>("WeatherVibe.ThunderPrelude.RaiseIfRaining", true);

        // Storm Outro params (always-on, IN-DWELL)
        s_stormOutroIncludeThunders = sConfigMgr->GetOption<bool>("WeatherVibe.StormOutro.IncludeThunders", true);
        s_stormOutroTarget = Clamp01(sConfigMgr->GetOption<float>("WeatherVibe.StormOutro.TargetGrade", 0.32f));
        s_stormOutroDurMin = sConfigMgr->GetOption<uint32>("WeatherVibe.StormOutro.Duration.Min", 20u);
        s_stormOutroDurMax = sConfigMgr->GetOption<uint32>("WeatherVibe.StormOutro.Duration.Max", 60u);
        if (s_stormOutroDurMax < s_stormOutroDurMin) std::swap(s_stormOutroDurMax, s_stormOutroDurMin);

        // CLIME VARIATION (optional)
        s_climeVarMinutes = sConfigMgr->GetOption<uint32>("WeatherVibe.Clime.VariationTime.Minutes", 0u);
        s_climeWeightPct = sConfigMgr->GetOption<float>("WeatherVibe.Clime.WeightJitter.Pct", 0.0f);
        s_climeIntensityAbs = sConfigMgr->GetOption<float>("WeatherVibe.Clime.IntensityJitter.Abs", 0.0f);
        // basic clamps
        if (s_climeWeightPct < 0.0f) s_climeWeightPct = 0.0f;
        if (s_climeWeightPct > 2.0f) s_climeWeightPct = 2.0f; // be sane
        if (s_climeIntensityAbs < 0.0f) s_climeIntensityAbs = 0.0f;
        if (s_climeIntensityAbs > 1.0f) s_climeIntensityAbs = 1.0f;

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
            "[mod_weather_vibe] config loaded: {} zones (interval={}s, transition={}..{}s, jitter={}..{}s, seasonMode={}, debug={}, climeVar={}min).",
            s_zoneProfiles.size(), s_interval, s_transMinSec, s_transMaxSec, s_jitterMin, s_jitterMax,
            (s_seasonMode == SeasonMode::Off ? "off" :
                s_seasonMode == SeasonMode::Auto ? "auto" :
                s_seasonMode == SeasonMode::Spring ? "spring" :
                s_seasonMode == SeasonMode::Summer ? "summer" :
                s_seasonMode == SeasonMode::Fall ? "fall" : "winter"),
            s_debug, s_climeVarMinutes);
    }

    // =========================
    // World script integration
    // =========================

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
                    int pi = PickPattern(zoneId, Z);
                    StartPattern(zoneId, Z, S, pi);
                }
            }

            LOG_INFO("server.loading", "[mod_weather_vibe] initialized (array patterns + dwell + season bias + timed transitions + in-dwell prelude/outro + clime variation).");
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
