/*
 * units.h - Metric or US: how a distance, an altitude, a speed and the map's scale bar are
 * written. One setting per device (docs/almanac.md, "Units"); the map's scale bar and
 * distances, the elevation readouts and the Almanac all follow it.
 *
 * Metric is what the firmware has always printed: unitsFmtDist() is byte-identical to
 * meshPosFmtDist() whenever the text fits `cap` (where it does not, meshPosFmtDist cuts it and
 * this writes ""), and unitsScaleBar() to mapScaleBar() plus the label the map draws
 * (tests/test_units.cpp links both and compares). US is feet, miles and mph, with the exact
 * conversions (1 ft = 0.3048 m, 1 mi = 1609.344 m, 1 mph = 0.44704 m/s).
 *
 * Every formatter writes the WHOLE NUL-terminated string, or "" when it does not fit in `cap` -
 * never cut short like snprintf, because a cut-short number is a different number that looks
 * right ("1,352ft" in 4 bytes would be "1,3", "500ft" in 3 "50"). It does nothing for a NULL
 * `out` or a cap of 0. A units value other than UNITS_US is metric.
 *
 * Pure: no Arduino/ESP-IDF headers, no heap, no static mutable state, C++11. COVEY's
 * covey_ui/almanac.py ports it line for line.
 */
#ifndef UNITS_H
#define UNITS_H

#include <stddef.h>

enum { UNITS_METRIC = 0, UNITS_US = 1 };

#define UNITS_M_PER_FT     0.3048
#define UNITS_M_PER_MI     1609.344
#define UNITS_MPS_PER_MPH  0.44704

/* A distance (negative reads as 0).
 *   metric: "850m" under 999.5 m, "1.4km" under 10 km, "12km" - meshPosFmtDist() exactly,
 *           including its "10.0km" for 9,950-9,999 m.
 *   US:     "850ft" under 999.5 ft (never "1000ft": 999.96 ft is "0.2mi"), "1.4mi" under
 *           9.95 mi, "12mi" (so 9.97 mi is "10mi", not "10.0mi"). */
void unitsFmtDist(double m, int units, char* out, size_t cap);

/* An altitude, or with withSign a difference, rounded to the whole metre / foot half away
 * from zero: metric "412m" "+120m" "-35m", US "1,352ft" "+394ft" "-115ft" (thousands comma,
 * US only - metric is "4392m"). Anything that rounds to 0 is "0m" / "0ft", never "+0" or
 * "-0". A negative value always carries its '-'; '+' only with withSign. Not finite: "--".
 * The magnitude is held to 1e12 (m or ft) - far past any real value, and inside int64. */
void unitsFmtAlt(double m, int units, bool withSign, char* out, size_t cap);

/* A speed, one decimal: metric "3.2km/h", US "2.0mph". Negative reads as 0 (a speed has no
 * sign), not finite is "--", and it is held to 1e9 of the unit. */
void unitsFmtSpeed(double metresPerSec, int units, char* out, size_t cap);

/* The map's scale bar: the longest "round" length that is at most maxPx pixels long.
 *   metric: 1/2/5 x 10^n m, 1 m .. 50,000 km - mapScaleBar() exactly (the same choice, the same
 *           pixels, the same fallback) - labelled "%dm" under 1000 m, else "%dkm" (whole km),
 *           as app_maps.cpp's drawBottomStrip has always drawn it.
 *   US:     1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000 ft, then 0.5, 1, 2, 5, 10, 20, 50,
 *           100, 200, 500 mi, labelled "500ft", "2000ft" (no comma: the strip is narrow),
 *           "0.5mi", "2mi".
 * A candidate fits when (int)(length / metresPerPx + 0.5) is 1..maxPx. When none does the
 * smallest (1 m / 1 ft) is shown with its pixels held to 1..maxPx - a bar drawn wrong is
 * visibly wrong, no bar is invisibly wrong (mapScaleBar's rule).
 * Returns the bar's length in metres, rounded, at least 1 (1 ft is 0.3 m); 0 = no bar (a
 * metresPerPx that is not > 0, or maxPx < 1), with *barPx = 0 and label "". barPx and label
 * may be NULL. */
int  unitsScaleBar(double metresPerPx, int maxPx, int units, int* barPx, char* label, size_t cap);

/* The setting's own row, the SAME words in the Maps menu and the Almanac's Settings (review
 * 2026-09-27: "Units: US (ft, mi)" in one, "Units: US (ft, mi, mph)" in the other):
 * "Units: US (ft, mi, mph)" / "Units: metric". A static string. */
const char* unitsSettingRow(int units);

#endif // UNITS_H
