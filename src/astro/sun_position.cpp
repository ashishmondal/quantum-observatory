// NOAA low-precision sun-position algorithm. Reference:
//   https://gml.noaa.gov/grad/solcalc/solareqns.PDF
//   https://en.wikipedia.org/wiki/Position_of_the_Sun
//
// Accuracy: ~0.5° in declination, ~1° in altitude/azimuth — well
// within the resolution of our 64×32 panel.

#include "sun_position.h"

#include <math.h>

namespace sun {

namespace {

constexpr float kDeg2Rad = 0.017453292519943295f;  // M_PI / 180
constexpr float kRad2Deg = 57.29577951308232f;     // 180 / M_PI

}  // namespace

Position compute(int32_t utc_epoch, float latitude_deg, float longitude_deg) {
  // Days (and fractional days) since J2000.0 = 2000-01-01 12:00 UTC.
  // J2000 epoch in Unix seconds = 946728000.
  const float n = (static_cast<float>(utc_epoch) - 946728000.0f) / 86400.0f;

  // Mean longitude of the sun, corrected for aberration (degrees).
  float L = 280.460f + 0.9856474f * n;
  L = fmodf(L, 360.0f);
  if (L < 0) L += 360.0f;

  // Mean anomaly (degrees).
  float g = 357.528f + 0.9856003f * n;
  g = fmodf(g, 360.0f);
  if (g < 0) g += 360.0f;
  const float g_rad = g * kDeg2Rad;

  // Ecliptic longitude (degrees).
  const float lambda =
      L + 1.915f * sinf(g_rad) + 0.020f * sinf(2.0f * g_rad);
  const float lambda_rad = lambda * kDeg2Rad;

  // Obliquity of the ecliptic (degrees).
  const float epsilon = 23.439f - 0.0000004f * n;
  const float eps_rad = epsilon * kDeg2Rad;

  // Right ascension (radians) and declination (radians).
  const float ra =
      atan2f(cosf(eps_rad) * sinf(lambda_rad), cosf(lambda_rad));
  const float dec = asinf(sinf(eps_rad) * sinf(lambda_rad));

  // Greenwich Mean Sidereal Time (degrees).
  // Approximation: GMST = 18.697374558 + 24.06570982441908 * days, in hours.
  float gmst_hours = 18.697374558f + 24.06570982441908f * n;
  gmst_hours = fmodf(gmst_hours, 24.0f);
  if (gmst_hours < 0) gmst_hours += 24.0f;
  const float gmst_deg = gmst_hours * 15.0f;

  // Local hour angle (degrees, then radians).
  float ha_deg = gmst_deg + longitude_deg - ra * kRad2Deg;
  ha_deg = fmodf(ha_deg + 180.0f, 360.0f);
  if (ha_deg < 0) ha_deg += 360.0f;
  ha_deg -= 180.0f;
  const float ha_rad = ha_deg * kDeg2Rad;

  const float lat_rad = latitude_deg * kDeg2Rad;

  // Altitude.
  const float sin_alt = sinf(lat_rad) * sinf(dec)
                      + cosf(lat_rad) * cosf(dec) * cosf(ha_rad);
  const float alt_rad = asinf(sin_alt);

  // Azimuth — measured from North, clockwise (0=N, 90=E, 180=S, 270=W).
  const float sin_az = -cosf(dec) * sinf(ha_rad);
  const float cos_az = sinf(dec) * cosf(lat_rad)
                     - cosf(dec) * sinf(lat_rad) * cosf(ha_rad);
  float az_rad = atan2f(sin_az, cos_az);
  float az_deg = az_rad * kRad2Deg;
  if (az_deg < 0) az_deg += 360.0f;

  return Position{alt_rad * kRad2Deg, az_deg};
}

}  // namespace sun
