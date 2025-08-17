USE acore_world;

START TRANSACTION;

DELETE FROM `game_weather`
WHERE `zone` IN (
  -- EK / Kalimdor
  4,    -- Blasted Lands
  8,    -- Swamp of Sorrows
  14,   -- Durotar
  16,   -- Azshara
  17,   -- The Barrens
  40,   -- Westfall
  46,   -- Burning Steppes
  51,   -- Searing Gorge
  130,  -- Silverpine Forest
  331,  -- Ashenvale
  361,  -- Felwood
  400,  -- Thousand Needles
  406,  -- Stonetalon Mountains
  493,  -- Moonglade
  3430, -- Eversong Woods
  3433, -- Ghostlands
  3524, -- Azuremyst Isle
  3525, -- Bloodmyst Isle

  -- Outland
  3483, -- Hellfire Peninsula
  3518, -- Nagrand
  3519, -- Terokkar Forest
  3520, -- Shadowmoon Valley
  3522, -- Blade's Edge Mountains
  3523, -- Netherstorm
  3703, -- Shattrath City

  -- Northrend
  3537, -- Borean Tundra
  495,  -- Howling Fjord
  65,   -- Dragonblight
  394,  -- Grizzly Hills
  66,   -- Zul'Drak
  3711, -- Sholazar Basin
  2817, -- Crystalsong Forest
  67,   -- The Storm Peaks
  210,  -- Icecrown
  4197  -- Wintergrasp
);

COMMIT;

-- (Optional) Verify they've been removed:
-- SELECT zone FROM game_weather WHERE zone IN (4,8,14,16,17,40,46,51,130,331,361,400,406,493,3430,3433,3524,3525,3483,3518,3519,3520,3522,3523,3703,3537,495,65,394,66,3711,2817,67,210,4197);
