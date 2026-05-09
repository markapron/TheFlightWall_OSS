#pragma once

#include <math.h>

constexpr double kPi = 3.14159265358979323846;
inline double degreesToRadians(double deg) { return deg * kPi / 180.0; }
inline double radiansToDegrees(double rad) { return rad * 180.0 / kPi; }

inline double haversineKm(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371.0; // Earth radius in km
    const double dlat = degreesToRadians(lat2 - lat1);
    const double dlon = degreesToRadians(lon2 - lon1);
    const double a = sin(dlat / 2) * sin(dlat / 2) +
                     cos(degreesToRadians(lat1)) * cos(degreesToRadians(lat2)) *
                         sin(dlon / 2) * sin(dlon / 2);
    const double c = 2 * asin(sqrt(a));
    return R * c;
}

inline double computeBearingDeg(double lat1, double lon1, double lat2, double lon2)
{
    const double dlon = degreesToRadians(lon2 - lon1);
    const double lat1r = degreesToRadians(lat1);
    const double lat2r = degreesToRadians(lat2);
    const double x = sin(dlon) * cos(lat2r);
    const double y = cos(lat1r) * sin(lat2r) - sin(lat1r) * cos(lat2r) * cos(dlon);
    const double initial = atan2(x, y);
    const double deg = fmod((radiansToDegrees(initial) + 360.0), 360.0);
    return deg;
}

inline void centeredBoundingBox(double lat, double lon, double radiusKm,
                                double &latMin, double &latMax,
                                double &lonMin, double &lonMax)
{
    const double latDelta = radiusKm / 111.0;
    const double lonDelta = radiusKm / (111.0 * cos(degreesToRadians(lat)));
    latMin = lat - latDelta;
    latMax = lat + latDelta;
    lonMin = lon - lonDelta;
    lonMax = lon + lonDelta;
}

// Returns the US Eastern UTC offset in minutes: -240 (EDT) during daylight saving
// time or -300 (EST) otherwise. Applies US DST rules automatically:
//   start = 2nd Sunday of March  at 07:00 UTC (= 02:00 EST, spring forward)
//   end   = 1st Sunday of November at 06:00 UTC (= 02:00 EDT, fall back)
inline int getEasternOffsetMinutes(unsigned long utcEpoch)
{
    // Days from Unix epoch (1970-01-01) to Jan 1 of year y (Gregorian).
    auto daysToJan1 = [](int y) -> unsigned long {
        long n = (long)(y - 1);
        return (unsigned long)(365L*n + n/4L - n/100L + n/400L - 719162L);
    };

    // Sakamoto's day-of-week algorithm (0=Sun, 1=Mon, … 6=Sat).
    // kDowTab is inside the lambda (static-duration, no capture needed).
    auto dow = [](int y, int m, int d) -> int {
        static const int8_t kDowTab[12] = {0,3,2,5,0,3,5,1,4,6,2,4};
        if (m < 3) --y;
        return (y + y/4 - y/100 + y/400 + (int)kDowTab[m-1] + d) % 7;
    };

    // Approximate year then refine by ±1 to handle leap-year boundary.
    int year = (int)(1970UL + utcEpoch / 31556952UL);
    while (daysToJan1(year + 1) * 86400UL <= utcEpoch) ++year;
    while (daysToJan1(year)     * 86400UL >  utcEpoch) --year;

    bool leap = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);

    // Days since epoch to March 1 and November 1 of this year.
    unsigned long mar1 = daysToJan1(year) + 31UL + (leap ? 29UL : 28UL);
    unsigned long nov1 = mar1 + 245UL; // Mar31+Apr30+May31+Jun30+Jul31+Aug31+Sep30+Oct31

    // 2nd Sunday of March at 07:00 UTC.
    unsigned long dstStart =
        (mar1 + (unsigned long)((7 - dow(year, 3, 1)) % 7) + 7UL) * 86400UL + 7UL * 3600UL;

    // 1st Sunday of November at 06:00 UTC.
    unsigned long dstEnd =
        (nov1 + (unsigned long)((7 - dow(year, 11, 1)) % 7)) * 86400UL + 6UL * 3600UL;

    return (utcEpoch >= dstStart && utcEpoch < dstEnd) ? -240 : -300;
}
