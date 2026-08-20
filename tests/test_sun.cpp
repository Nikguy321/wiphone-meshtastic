/*
 * test_sun.cpp — sun_times.cpp against facts that are true independent of the
 * implementation: known almanac anchors (loose tolerance), the geometry of
 * equinoxes and solstices, longitude shifting time linearly, and the ordering
 * dawn < rise < set < dusk everywhere it all exists. Plus the date helpers.
 */

#include "../WiPhone/sun_times.h"
#include <cstdio>
#include <cstdlib>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name) do { \
    checks++; \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

/* Times are UTC minutes normalized into 0..1440 of the requested UTC date —
 * for zones far from Greenwich an evening event lands on the ADJACENT UTC day
 * and wraps (Seattle's 19:16 PDT sunset is 02:16 UTC "tomorrow"). Spans must
 * unwrap: the day length is set-after-rise on a monotonic ladder. */
static int span(int a, int b) { return (b - a + 1440) % 1440; }
static bool near(int got, int want, int tol) { return abs(got - want) <= tol; }

int main() {
  printf("test_sun\n");

  const double SEA_LAT = 47.6062, SEA_LON = -122.3321;

  // ---- date helpers ---------------------------------------------------------
  {
    int y, m, d;
    sunUnixToDate(0, &y, &m, &d);
    CHECK(y == 1970 && m == 1 && d == 1, "epoch date");
    sunUnixToDate(1787184000u, &y, &m, &d);        // 2026-08-20 00:00 UTC
    CHECK(y == 2026 && m == 8 && d == 20, "2026-08-20 from unix");
    sunUnixToDate(1709164800u, &y, &m, &d);        // 2024-02-29
    CHECK(y == 2024 && m == 2 && d == 29, "leap day resolves");
  }

  // ---- almanac anchors (NOAA calculator, ±4 min slack for year drift) -------
  {
    SunTimes t;
    /* Seattle, 2026-09-15 (the hunt's season). NOAA-class answers in PDT
     * (UTC-7): sunrise ~06:49, sunset ~19:16. In UTC: 13:49, 02:16(+1d). */
    CHECK(sunTimesUtc(2026, 9, 15, SEA_LAT, SEA_LON, &t), "computes for Seattle Sep 15");
    CHECK(near(t.riseMin, 13 * 60 + 49, 4), "Seattle Sep 15 sunrise ~06:49 PDT");
    CHECK(near(t.setMin, 2 * 60 + 16, 6), "Seattle Sep 15 sunset ~19:16 PDT (02:16 UTC, wrapped)");
    CHECK(t.dawnMin != -1 && t.duskMin != -1, "civil twilight exists in September");
    /* Civil twilight at mid-latitudes runs ~28-34 min beyond the sun times. */
    int dawnLead = (t.riseMin - t.dawnMin + 1440) % 1440;
    int duskLag  = (t.duskMin - t.setMin + 1440) % 1440;
    CHECK(dawnLead >= 24 && dawnLead <= 38, "civil dawn ~30 min before sunrise");
    CHECK(duskLag >= 24 && duskLag <= 38, "civil dusk ~30 min after sunset");
  }

  // ---- geometry that must hold ----------------------------------------------
  {
    SunTimes t;
    // Equinox: day length ~12h07m (disc + refraction), at the equator too.
    sunTimesUtc(2026, 3, 20, SEA_LAT, SEA_LON, &t);
    CHECK(near(span(t.riseMin, t.setMin), 12 * 60 + 7, 6), "equinox day ~12h07 in Seattle");
    sunTimesUtc(2026, 3, 20, 0.0, SEA_LON, &t);
    CHECK(near(span(t.riseMin, t.setMin), 12 * 60 + 7, 4), "equinox day ~12h07 at the equator");

    // Solstices: June day longest, December shortest, and ordered sanely.
    SunTimes ju, de;
    sunTimesUtc(2026, 6, 21, SEA_LAT, SEA_LON, &ju);
    sunTimesUtc(2026, 12, 21, SEA_LAT, SEA_LON, &de);
    CHECK(span(ju.riseMin, ju.setMin) > 15 * 60, "June day in Seattle ~16h");
    CHECK(span(de.riseMin, de.setMin) < 9 * 60, "December day in Seattle ~8.5h");
    CHECK(span(ju.riseMin, ju.setMin) > span(de.riseMin, de.setMin) + 6 * 60,
          "solstice spread > 6h at 47N");

    // 15 degrees of longitude = one hour of clock, same latitude.
    SunTimes a, b;
    sunTimesUtc(2026, 9, 15, SEA_LAT, SEA_LON, &a);
    sunTimesUtc(2026, 9, 15, SEA_LAT, SEA_LON + 15.0, &b);
    CHECK(near((a.riseMin - b.riseMin + 1440) % 1440, 60, 2), "15 deg east = 60 min earlier");

    // Polar night: Utqiagvik in late December has no sunrise; twilight still does.
    sunTimesUtc(2026, 12, 21, 71.29, -156.79, &t);
    CHECK(t.riseMin == -1 && t.setMin == -1, "polar night: no sun events");
    CHECK(t.dawnMin != -1, "polar night: civil twilight still occurs");
  }

  // ---- ordering wherever everything exists ----------------------------------
  {
    SunTimes t;
    sunTimesUtc(2026, 10, 10, SEA_LAT, SEA_LON, &t);
    // All in UTC minutes; in Seattle these wrap midnight for dusk sometimes —
    // compare on an unwrapped ladder from dawn.
    int dawn = t.dawnMin;
    int rise = t.riseMin + (t.riseMin < dawn ? 1440 : 0);
    int set = t.setMin + (t.setMin < rise ? 1440 : 0);
    int dusk = t.duskMin + (t.duskMin < set ? 1440 : 0);
    CHECK(dawn < rise && rise < set && set < dusk, "dawn < rise < set < dusk");
  }

  // ---- refusals -------------------------------------------------------------
  {
    SunTimes t;
    CHECK(!sunTimesUtc(2026, 2, 30, SEA_LAT, SEA_LON, &t), "Feb 30 refused");
    CHECK(!sunTimesUtc(2026, 9, 15, 95.0, SEA_LON, &t), "lat > 90 refused");
    CHECK(!sunTimesUtc(1969, 9, 15, SEA_LAT, SEA_LON, &t), "pre-epoch refused");
  }

  if (failures) {
    printf("test_sun: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("test_sun: %d passed, 0 failed\n", checks);
  return 0;
}
