# AzerothCore Module: Weather Vibe 

(**Work In Progess**)


Bring your world to life with **mod_weather_vibe**. This module gives each zone a
distinct **mood**—misty mornings in Elwynn, a **gloomy** Duskwood that rumbles to life,
**biting** Wintergrasp squalls, and **rolling thunderheads** over Stranglethorn. Weather
no longer just _flips_; it **evolves** naturally over time with small shifts,
occasional bursts, and regional spillovers that make the world feel **alive** and
**immersive**. 

TODO: 
```
- Extending logic or eventually even adding dynamic weather models
- Adding weather fronts moving through entire world
- Adding and testing zones which are normally locked for weather changes
- [Optimize performance] do not update weather when there are no players/real players in the zone
```

You can enable all zones by running the scripts mentioned below, they are part of the default config but i havent 
test them yet. There is enable script which adds the missing zones, and disable which deletes the zones we just
added. 

Nevertheless the default configuration contains all weather profiles all zones, default and none defaults.

## Enable weather for all zones

Default zones
```sql
mysql> SELECT zone FROM game_weather
35 rows in set (0.00 sec)
```

```yamel
1     Dun Morogh
3     Badlands
10    Duskwood
11    Wetlands
12    Elwynn Forest
15    Dustwallow Marsh
28    Western Plaguelands
33    Stranglethorn Vale
36    Alterac Mountains
38    Loch Modan
41    Deadwind Pass
44    Redridge Mountains
45    Arathi Highlands
47    The Hinterlands
85    Tirisfal Glades
139   Eastern Plaguelands
141   Teldrassil
148   Darkshore
215   Mulgore
267   Hillsbrad Foothills
357   Feralas
405   Desolace
440   Tanaris
490   Un'Goro Crater
618   Winterspring
796   Scarlet Monastery
1377  Silithus
1977  Zul'Gurub
2017  Stratholme
2597  Alterac Valley
3358  Arathi Basin
3428  Temple of Ahn'Qiraj
3429  Ruins of Ahn'Qiraj
3521  Zangarmarsh
4080  Isle of Quel'Danas
```

SQL ENABLE all zones script
[SQL_ENABLE_ALL_ZONES_FOR_WEATHER.sql](https://github.com/hermensbas/mod_weather_vibe/blob/main/SQL_ENABLE_ALL_ZONES_FOR_WEATHER.sql)

SQL DISABLE the newly added zones script
[SQL_DISABLE_ALL_ZONES_FOR_WEATHER.sql](https://github.com/hermensbas/mod_weather_vibe/blob/main/SQL_DISABLE_ALL_ZONES_FOR_WEATHER.sql)

## Installation

1. **Clone the module**

```bash
    cd /path/to/azerothcore/modules
    git clone https://github.com/<your-org>/mod-weather-vibe.git
```
   
3. **Rebuild**

```bash
    cd /path/to/azerothcore
    mkdir -p build && cd build
    cmake ..
    make -j"$(nproc)"
```

4. **Install config**

```bash
    cp /path/to/azerothcore/modules/mod-weather-vibe/conf/mod_weather_vibe.conf.dist \
       /path/to/azerothcore/modules/mod-weather-vibe/conf/mod_weather_vibe.conf
```

5. **Modidfy your worldserver.conf**
To prevent the core Weather from constantly overwriting this module’s applied patterns, **set a large core interval**:
```bash
    # worldserver.conf
    ChangeWeatherInterval = 86400000   # 24 hours 
```

6. **Restart**
```bash
    ./worldserver
```


## Overview

**What you configure per zone:** a list of 4–10 small **patterns**:
- **type**: fine/rain/snow/storm/thunders (0/1/2/3/86)
- **intensity range**: `minIntensity..maxIntensity` (0.0–1.0)
- **dwell time**: `minMinutes..maxMinutes`
- **description**: shown to players if debug is enabled

**What the mod does:**
- Picks a pattern with weighted (and season-biased) selection.
- Chooses a random target grade in the pattern’s range and **eases** toward it.
- Holds until the dwell window elapses, then picks another (respecting transition bans).
- Adds **jitter** per zone so updates aren’t synchronized.
---

## Core Weather Mapping (AzerothCore)

Intensity (“grade”) is mapped by the core as:

- `< 0.27` → **FINE** *(even if type ≠ fine; avoid such ranges for non-fine types)*
- `0.27–0.39` → **LIGHT**
- `0.40–0.69` → **MEDIUM**
- `0.70–1.00` → **HEAVY**

**Types:**  
`0=fine, 1=rain, 2=snow, 3=storm (sand/ash/blizzard), 86=thunders`

---

## Configuration (Global)

Create/edit `mod_weather_vibe.conf`.  
Use **ini** syntax; comments start with `#`.

    #######################################################################
    #  mod_weather_vibe — GLOBAL environment weather controller (zone-wide)
    #
    #  CONFIG PER-ZONE ENTRIES (array form):
    #    WeatherVibe.Zone.<ZoneId>[i] = [type, weight, minIntensity, maxIntensity, minMinutes, maxMinutes, "description"]
    #
    #      type:            0=fine, 1=rain, 2=snow, 3=storm (sand/ash/blizzard), 86=thunders
    #      weight:          relative selection weight (0 disables this pattern)
    #      min/maxIntensity: 0.00–1.00; pick ≥0.27 for visible non-fine effects
    #      min/maxMinutes:   dwell time window for the pattern (minutes, min ≤ max)
    #      "description":    free text shown if WeatherVibe.Debug=1 (quotes required)
    #
    #  CORE WEATHER BANDS (AzerothCore intensity → state)
    #      < 0.27    → FINE           (even if type ≠ fine)
    #      0.27–0.39 → LIGHT
    #      0.40–0.69 → MEDIUM
    #      0.70–1.00 → HEAVY
    #######################################################################
    
    # --- Global toggles & cadence ---
    WeatherVibe.Enable = 1
    WeatherVibe.Interval = 120             # engine tick cadence (seconds)
    
    # --- Transitions & biasing ---
    # 0.0 = off (instant jumps), 1.0 = smoothest possible transitions
    WeatherVibe.Transition.Smoothness = 0.50
    
    # Seasons: "auto" | "off" | "spring" | "summer" | "fall" | "winter"
    WeatherVibe.Seasons = "auto"
    
    # Broadcast pattern apply messages to players in-zone
    WeatherVibe.Debug = 0
    
    # Forbid direct transitions (add more indices as needed)
    # Example: disallow snow → rain
    WeatherVibe.Transition.NotAllowed[0] = [2, 1]
    
    # Per-zone desynchronization jitter (seconds) to prevent mass flips
    WeatherVibe.Jitter.Zone.Min = 1
    WeatherVibe.Jitter.Zone.Max = 5

---

## Configuration (Per Zone)

Each zone defines one or more patterns. Typical zones use **4–10**.

**Format**

    # <Zone Name> — vibe note
    WeatherVibe.Zone.<ZoneId>[i] = [type, weight, minIntensity, maxIntensity, minMinutes, maxMinutes, "description"]

**Example — Elwynn Forest (12):**

    # Elwynn Forest — green & mild; passing showers
    WeatherVibe.Zone.12[0] = [0, 0.60, 0.05, 0.22, 2, 4, "fine — bright breaks"]
    WeatherVibe.Zone.12[1] = [0, 0.10, 0.15, 0.26, 1, 3, "fine — hazy/overcast window"]
    WeatherVibe.Zone.12[2] = [1, 0.18, 0.30, 0.55, 2, 5, "rain — showery light/med"]
    WeatherVibe.Zone.12[3] = [3, 0.08, 0.28, 0.60, 2, 4, "storm — gusty squalls"]
    WeatherVibe.Zone.12[4] = [86,0.04, 0.35, 0.70, 1, 2, "thunders — brief rumbles"]

**Example — Force perpetual snow (Dun Morogh, 1):**

    # Dun Morogh — force perpetual snow; floor at light (≥ 0.27)
    WeatherVibe.Zone.1[0] = [2, 0.44, 0.28, 0.39, 2, 5, "snow — light flurries (baseline)"]
    WeatherVibe.Zone.1[1] = [2, 0.28, 0.38, 0.55, 2, 5, "snow — steady light/medium"]
    WeatherVibe.Zone.1[2] = [2, 0.14, 0.55, 0.69, 2, 4, "snow — moderate"]
    WeatherVibe.Zone.1[3] = [2, 0.08, 0.70, 0.85, 1, 3, "snow — heavy bursts"]
    WeatherVibe.Zone.1[4] = [2, 0.06, 0.85, 1.00, 1, 2, "snow — near-blizzard"]

---

## Global Settings Reference

| Setting                                | Description                                                                                | Default | Valid Values |
|----------------------------------------|--------------------------------------------------------------------------------------------|---------|--------------|
| `WeatherVibe.Enable`                   | Master switch.                                                                             | `1`     | `0` / `1`    |
| `WeatherVibe.Interval`                 | Engine tick cadence in seconds.                                                            | `120`   | ≥ 1          |
| `WeatherVibe.Transition.Smoothness`    | Intensity easing factor; 0.0 instant, 1.0 smoothest.                                       | `0.50`  | `0.0–1.0`    |
| `WeatherVibe.Seasons`                  | Season bias: `"auto"`, `"off"`, `"spring"`, `"summer"`, `"fall"`, `"winter"`.              | `"auto"`| string       |
| `WeatherVibe.Debug`                    | Broadcast debug messages to players in-zone on pattern apply.                              | `0`     | `0` / `1`    |
| `WeatherVibe.Transition.NotAllowed[n]` | Disallow direct transition: `[fromType, toType]`.                                          | —       | arrays       |
| `WeatherVibe.Jitter.Zone.Min`          | Per-zone random offset minimum (seconds) added to dwell/tick.                              | `1`     | ≥ 0          |
| `WeatherVibe.Jitter.Zone.Max`          | Per-zone random offset maximum (seconds).                                                  | `5`     | ≥ min        |

---

## Design Tips

- **Make effects visible:** For non-fine types, set `minIntensity ≥ 0.30`. Core treats `< 0.27` as fine.
- **Dwell windows:** 1–5 min feels natural; use shorter during testing.
- **Weights:** Zero disables; seasonal bias multiplies weight but does not resurrect zero.
- **Transitions:** Add bans for immersion (e.g., `[2,1]` snow→rain).
- **Seasons:** Force `"winter"` or `"summer"` to stress-test vibes.

---

## Debugging

Enable zone broadcasts:

    WeatherVibe.Debug = 1

You’ll see messages like:

    [WeatherVibe] Zone 1: snow — steady light/medium (grade 0.42, ~3m)

Faster testing:

    WeatherVibe.Interval = 10
    # And use 1–2 minute dwell windows in your patterns

If logs say `config loaded: 0 zones`, your keys are wrong.  
Correct shape is: `WeatherVibe.Zone.<ZoneId>[i] = [ ... ]` (digits until `[` are parsed as the zone id).

If visuals don’t change:
- The zone may not support weather (DBC).
- Your non-fine intensities might be `< 0.27` (renders as fine).
- You’re still within the dwell window; shorten min/max minutes.

---

## License

This module is released under the **GNU AGPL v3**, consistent with AzerothCore.























