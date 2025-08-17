USE acore_world;

INSERT INTO `game_weather` (
  `zone`,
  `spring_rain_chance`, `spring_snow_chance`, `spring_storm_chance`,
  `summer_rain_chance`, `summer_snow_chance`, `summer_storm_chance`,
  `fall_rain_chance`,   `fall_snow_chance`,   `fall_storm_chance`,
  `winter_rain_chance`, `winter_snow_chance`, `winter_storm_chance`,
  `ScriptName`
)
SELECT
  v.zone,
  v.spring_rain_chance, v.spring_snow_chance, v.spring_storm_chance,
  v.summer_rain_chance, v.summer_snow_chance, v.summer_storm_chance,
  v.fall_rain_chance,   v.fall_snow_chance,   v.fall_storm_chance,
  v.winter_rain_chance, v.winter_snow_chance, v.winter_storm_chance,
  v.ScriptName
FROM (
  -- Blasted Lands — arid; dry storms
  SELECT
    4  AS zone,
    5  AS spring_rain_chance, 0  AS spring_snow_chance, 25 AS spring_storm_chance,
    0  AS summer_rain_chance, 0  AS summer_snow_chance, 30 AS summer_storm_chance,
    5  AS fall_rain_chance,   0  AS fall_snow_chance,   25 AS fall_storm_chance,
    5  AS winter_rain_chance, 0  AS winter_snow_chance, 20 AS winter_storm_chance,
    '' AS ScriptName
  UNION ALL
  -- Swamp of Sorrows — humid; frequent rain
  SELECT 8, 45,0,10, 35,0,10, 55,0,10, 35,0,10, ''
  UNION ALL
  -- Durotar — dry; sand/wind storms
  SELECT 14, 0,0,20, 0,0,25, 0,0,20, 0,0,15, ''
  UNION ALL
  -- Azshara — coastal; storms & showers
  SELECT 16, 15,0,15, 10,0,20, 15,0,20, 10,0,15, ''
  UNION ALL
  -- The Barrens — arid; dust squalls
  SELECT 17, 5,0,15, 0,0,20, 5,0,15, 5,0,10, ''
  UNION ALL
  -- Westfall — windy plains; squalls
  SELECT 40, 20,0,15, 15,0,15, 25,0,15, 15,0,10, ''
  UNION ALL
  -- Burning Steppes — volcanic; violent storms
  SELECT 46, 0,0,30, 0,0,35, 0,0,30, 0,0,25, ''
  UNION ALL
  -- Searing Gorge — ash storms
  SELECT 51, 0,0,30, 0,0,35, 0,0,30, 0,0,25, ''
  UNION ALL
  -- Silverpine Forest — bleak; drizzle & squalls
  SELECT 130, 30,0,10, 25,0,10, 35,0,10, 20,5,10, ''
  UNION ALL
  -- Ashenvale — temperate forest; showers
  SELECT 331, 30,0,10, 20,0,10, 35,0,10, 15,5,10, ''
  UNION ALL
  -- Felwood — tainted; grim rain & storms
  SELECT 361, 25,0,15, 15,0,15, 30,0,15, 20,5,15, ''
  UNION ALL
  -- Thousand Needles — canyon winds; dry storms
  SELECT 400, 0,0,25, 0,0,30, 0,0,25, 0,0,20, ''
  UNION ALL
  -- Stonetalon Mountains — rocky; wind & rain
  SELECT 406, 20,0,15, 15,0,20, 20,0,15, 15,5,15, ''
  UNION ALL
  -- Moonglade — calm; occasional snow/fog
  SELECT 493, 10,5,5, 5,0,5, 10,5,5, 5,15,5, ''
  UNION ALL
  -- Eversong Woods — bright; light showers
  SELECT 3430, 25,0,10, 20,0,10, 25,0,10, 20,0,10, ''
  UNION ALL
  -- Ghostlands — gloomy; passing storms
  SELECT 3433, 30,0,15, 25,0,10, 30,0,15, 25,0,10, ''
  UNION ALL
  -- Azuremyst Isle — maritime; light rain
  SELECT 3524, 25,0,10, 20,0,10, 30,0,10, 20,5,10, ''
  UNION ALL
  -- Bloodmyst Isle — maritime; frequent showers
  SELECT 3525, 30,0,10, 25,0,10, 35,0,10, 25,5,10, ''
  UNION ALL

  -- Outland
  -- Hellfire Peninsula — violent fronts; ash & thunder
  SELECT 3483, 0,0,40, 0,0,45, 0,0,40, 0,0,35, ''
  UNION ALL
  -- Nagrand — bright plains; breezy storms
  SELECT 3518, 15,0,15, 10,0,20, 20,0,15, 10,0,15, ''
  UNION ALL
  -- Terokkar Forest — forest rains & storms
  SELECT 3519, 25,0,10, 20,0,10, 25,0,10, 20,0,10, ''
  UNION ALL
  -- Shadowmoon Valley — fel-charged storms
  SELECT 3520, 0,0,45, 0,0,50, 0,0,45, 0,0,40, ''
  UNION ALL
  -- Blade's Edge Mountains — harsh winds & storms
  SELECT 3522, 15,0,20, 10,0,25, 15,0,20, 10,0,20, ''
  UNION ALL
  -- Netherstorm — arcane supercells
  SELECT 3523, 0,0,50, 0,0,55, 0,0,50, 0,0,45, ''
  UNION ALL
  -- Shattrath City — mostly fair (indoor-heavy)
  SELECT 3703, 10,0,5, 10,0,5, 10,0,5, 10,0,5, ''

  UNION ALL
  -- Northrend
  -- Borean Tundra — snowy plains; squalls
  SELECT 3537, 0,50,10, 0,30,10, 0,45,10, 0,70,10, ''
  UNION ALL
  -- Howling Fjord — snow + coastal storms
  SELECT 495, 10,40,10, 15,20,10, 10,40,10, 5,60,10, ''
  UNION ALL
  -- Dragonblight — frigid; steady snow
  SELECT 65, 0,60,10, 0,30,10, 0,55,10, 0,75,10, ''
  UNION ALL
  -- Grizzly Hills — dense forest; rain & some snow
  SELECT 394, 25,10,10, 30,0,10, 30,10,10, 10,40,10, ''
  UNION ALL
  -- Zul'Drak — glacial plateau; stormy snow
  SELECT 66, 0,65,10, 0,35,10, 0,60,10, 0,80,10, ''
  UNION ALL
  -- Sholazar Basin — tropical basin; storms
  SELECT 3711, 40,0,15, 35,0,20, 45,0,15, 30,0,10, ''
  UNION ALL
  -- Crystalsong Forest — quiet snow; calm fronts
  SELECT 2817, 0,40,10, 0,20,10, 0,35,10, 0,60,10, ''
  UNION ALL
  -- The Storm Peaks — high alpine; severe snow
  SELECT 67, 0,70,10, 0,40,10, 0,65,10, 0,85,10, ''
  UNION ALL
  -- Icecrown — brutal snow; blizzard fronts
  SELECT 210, 0,70,10, 0,40,10, 0,65,10, 0,85,10, ''
  UNION ALL
  -- Wintergrasp — windswept battlefield; heavy snow
  SELECT 4197, 0,60,10, 0,30,10, 0,55,10, 0,75,10, ''
) AS v
LEFT JOIN `game_weather` gw
  ON gw.zone = v.zone
WHERE gw.zone IS NULL;
