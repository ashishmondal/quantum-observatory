// Implementation of include/iss_geometry.h.

#include "iss_geometry.h"

#include <math.h>

namespace iss_geom {

namespace {

constexpr float kDeg2Rad     = 0.017453292519943295f;  // M_PI / 180
constexpr float kRad2Deg     = 57.29577951308232f;     // 180 / M_PI
constexpr float kEarthR_km   = 6371.0f;                // mean spherical Earth

}  // namespace

LookAngles look_angles(float observer_lat_deg, float observer_lon_deg,
                       float iss_lat_deg,      float iss_lon_deg,
                       float altitude_km) {
  const float phi_o = observer_lat_deg * kDeg2Rad;
  const float lam_o = observer_lon_deg * kDeg2Rad;
  const float phi_s = iss_lat_deg      * kDeg2Rad;
  const float lam_s = iss_lon_deg      * kDeg2Rad;

  const float cos_phi_o = cosf(phi_o);
  const float sin_phi_o = sinf(phi_o);
  const float cos_lam_o = cosf(lam_o);
  const float sin_lam_o = sinf(lam_o);
  const float cos_phi_s = cosf(phi_s);
  const float sin_phi_s = sinf(phi_s);
  const float cos_lam_s = cosf(lam_s);
  const float sin_lam_s = sinf(lam_s);

  // ECEF positions (units: km). Observer assumed at sea level — the
  // panel can't resolve the difference between sea level and the
  // user's actual altitude.
  const float Ox = kEarthR_km * cos_phi_o * cos_lam_o;
  const float Oy = kEarthR_km * cos_phi_o * sin_lam_o;
  const float Oz = kEarthR_km * sin_phi_o;

  const float Rs = kEarthR_km + altitude_km;
  const float Sx = Rs * cos_phi_s * cos_lam_s;
  const float Sy = Rs * cos_phi_s * sin_lam_s;
  const float Sz = Rs * sin_phi_s;

  // Range vector observer → satellite, ECEF frame.
  const float Vx = Sx - Ox;
  const float Vy = Sy - Oy;
  const float Vz = Sz - Oz;

  // Rotate ECEF → ENU at the observer (standard ENU rotation matrix).
  const float E =                    -sin_lam_o * Vx +              cos_lam_o * Vy;
  const float N = -sin_phi_o * cos_lam_o * Vx - sin_phi_o * sin_lam_o * Vy + cos_phi_o * Vz;
  const float U =  cos_phi_o * cos_lam_o * Vx + cos_phi_o * sin_lam_o * Vy + sin_phi_o * Vz;

  const float range = sqrtf(Vx * Vx + Vy * Vy + Vz * Vz);

  LookAngles out;
  // Guard against degenerate range (observer exactly at satellite —
  // physically impossible, but cheap to defend).
  if (range <= 0.0f) {
    out.azimuth_deg   = 0.0f;
    out.elevation_deg = 90.0f;
    return out;
  }

  out.elevation_deg = asinf(U / range) * kRad2Deg;

  float az = atan2f(E, N) * kRad2Deg;
  if (az < 0.0f) az += 360.0f;
  out.azimuth_deg = az;
  return out;
}

}  // namespace iss_geom
