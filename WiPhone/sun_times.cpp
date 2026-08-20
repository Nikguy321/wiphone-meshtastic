/*
 * sun_times.cpp — the NOAA solar calculation (their spreadsheet's method,
 * which is Meeus simplified). See sun_times.h; proven by tests/test_sun.cpp.
 */

#include "sun_times.h"
#include <math.h>

#define D2R(x) ((x) * (M_PI / 180.0))
#define R2D(x) ((x) * (180.0 / M_PI))

// Days in month, leap-aware.
static int daysIn(int y, int m) {
  static const int D[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) {
    return 29;
  }
  return D[m - 1];
}

void sunUnixToDate(uint32_t unixSecs, int* year, int* month, int* day) {
  uint32_t days = unixSecs / 86400u;
  int y = 1970;
  for (;;) {
    uint32_t inYear = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366u : 365u;
    if (days < inYear) {
      break;
    }
    days -= inYear;
    y++;
  }
  int m = 1;
  while (days >= (uint32_t)daysIn(y, m)) {
    days -= daysIn(y, m);
    m++;
  }
  if (year)  *year = y;
  if (month) *month = m;
  if (day)   *day = (int)days + 1;
}

// Julian day number for a UTC calendar date (Fliegel-Van Flandern).
static double julianDay(int y, int m, int d) {
  int a = (14 - m) / 12;
  int yy = y + 4800 - a;
  int mm = m + 12 * a - 3;
  long jdn = d + (153L * mm + 2) / 5 + 365L * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
  return (double)jdn - 0.5;                  // midnight UTC of that date
}

/* Solar declination (radians) and the equation of time (minutes) for a julian
 * CENTURY value — straight from the NOAA spreadsheet columns. */
static void solarBasics(double T, double* declRad, double* eqTimeMin) {
  double L0 = fmod(280.46646 + T * (36000.76983 + 0.0003032 * T), 360.0);
  double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
  double e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
  double C = sin(D2R(M)) * (1.914602 - T * (0.004817 + 0.000014 * T))
           + sin(D2R(2 * M)) * (0.019993 - 0.000101 * T)
           + sin(D2R(3 * M)) * 0.000289;
  double trueLong = L0 + C;
  double omega = 125.04 - 1934.136 * T;
  double appLong = trueLong - 0.00569 - 0.00478 * sin(D2R(omega));
  double obliq0 = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
  double obliq = obliq0 + 0.00256 * cos(D2R(omega));
  *declRad = asin(sin(D2R(obliq)) * sin(D2R(appLong)));

  double y = tan(D2R(obliq / 2.0));
  y *= y;
  *eqTimeMin = 4.0 * R2D(y * sin(2 * D2R(L0))
                         - 2.0 * e * sin(D2R(M))
                         + 4.0 * e * y * sin(D2R(M)) * cos(2 * D2R(L0))
                         - 0.5 * y * y * sin(4 * D2R(L0))
                         - 1.25 * e * e * sin(2 * D2R(M)));
}

/* UTC minutes of the event where the sun's zenith crosses `zenithDeg`.
 * rising=true for the morning crossing. -1 when it never happens that day. */
static int eventUtcMin(double jd, double latDeg, double lonDeg,
                       double zenithDeg, bool rising) {
  // Evaluate at the day's solar noon — the spreadsheet's level of rigor.
  double T = (jd + 0.5 - 2451545.0) / 36525.0;
  double decl, eqTime;
  solarBasics(T, &decl, &eqTime);

  double cosH = (cos(D2R(zenithDeg)) - sin(D2R(latDeg)) * sin(decl))
                / (cos(D2R(latDeg)) * cos(decl));
  if (cosH > 1.0 || cosH < -1.0) {
    return -1;                               // polar: sun never reaches that depth
  }
  double haMin = 4.0 * R2D(acos(cosH));      // half-arc, in minutes of time
  double noonMin = 720.0 - 4.0 * lonDeg - eqTime;
  double t = rising ? (noonMin - haMin) : (noonMin + haMin);
  // Normalize into the date's own 0..1440 window.
  while (t < 0) t += 1440.0;
  while (t >= 1440.0) t -= 1440.0;
  return (int)lround(t);
}

bool sunTimesUtc(int year, int month, int day,
                 double latDeg, double lonDeg, SunTimes* out) {
  if (!out || year < 1970 || year > 2099 || month < 1 || month > 12 ||
      day < 1 || day > daysIn(year, month) ||
      latDeg > 89.9 || latDeg < -89.9 || lonDeg < -180.0 || lonDeg > 180.0) {
    return false;
  }
  double jd = julianDay(year, month, day);
  out->dawnMin = (int16_t)eventUtcMin(jd, latDeg, lonDeg, 96.0, true);    // civil dawn
  out->riseMin = (int16_t)eventUtcMin(jd, latDeg, lonDeg, 90.833, true);  // sunrise
  out->setMin  = (int16_t)eventUtcMin(jd, latDeg, lonDeg, 90.833, false); // sunset
  out->duskMin = (int16_t)eventUtcMin(jd, latDeg, lonDeg, 96.0, false);   // civil dusk
  return true;
}
