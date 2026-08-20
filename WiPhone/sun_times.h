/*
 * sun_times.h — sunrise, sunset and civil twilight for a date and place.
 *
 * Pure offline math (the NOAA solar-position calculation): no network, no
 * tables, nothing to sync — exactly what a hunting phone needs, because "how
 * long until legal light ends" is the most-asked question of a hunting day and
 * the answer must not depend on having had internet.
 *
 * Coordinates come from the same place everything else here gets them: a
 * waypoint COVEY shared, or the user's pin. Accuracy is the NOAA spreadsheet's
 * (about ±1 minute at mid-latitudes) — fine for planning; the LAW's definition
 * of shooting hours is the almanac's, so treat the last minute as the lawyers
 * would, not as this file would.
 *
 * Self-contained, no Arduino headers; tests/test_sun.cpp proves it on the host.
 */
#ifndef SUN_TIMES_H
#define SUN_TIMES_H

#include <stdint.h>

typedef struct {
  /* Minutes after UTC midnight, normalized to 0..1439; -1 = the event does not
   * happen that day (polar day/night). ⚠ For zones far from Greenwich an
   * evening event belongs to the ADJACENT UTC date and wraps — Seattle's
   * 19:16 PDT sunset reads 02:16 UTC. Converting to local wall time mod 1440
   * is always right; day-length math must unwrap ((set-rise+1440)%1440).
   * Civil dawn/dusk (sun 6° below the horizon) are the usual legal-light
   * bounds. */
  int16_t dawnMin;      // civil dawn (first legal light, typically)
  int16_t riseMin;      // sunrise (upper limb, refraction-corrected: 90.833°)
  int16_t setMin;       // sunset
  int16_t duskMin;      // civil dusk (last legal light, typically)
} SunTimes;

/* Compute for a UTC calendar date at lat/lon (degrees, east-positive).
 * Returns false only for nonsense input (bad date, |lat| > 89.9). */
bool sunTimesUtc(int year, int month, int day,
                 double latDeg, double lonDeg, SunTimes* out);

/* Split a unix timestamp into its UTC calendar date (helper for callers that
 * only have time_t-style seconds; valid 1970..2099). */
void sunUnixToDate(uint32_t unixSecs, int* year, int* month, int* day);

#endif // SUN_TIMES_H
