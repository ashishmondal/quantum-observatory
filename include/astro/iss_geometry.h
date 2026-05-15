// ISS look-angle geometry.
//
// Given the observer (fixed lat/lon on Earth's surface, assumed sea
// level — the panel's resolution makes height-above-ellipsoid noise
// invisible) and the live ISS sub-satellite point + orbital altitude,
// returns the azimuth + elevation the observer would point a
// telescope toward right now.
//
// This is the closed-form spherical-Earth solution: convert both
// points to ECEF, take the difference, rotate into the observer's
// East-North-Up (ENU) frame, then read off azimuth = atan2(E, N) and
// elevation = asin(U / |range|). No TLE math, no SGP4 — the orbital
// state we need (where the station IS right now) is supplied by HA's
// pass-through of wheretheiss.at; this file only handles the
// observer-frame projection HA can't do generically (HA doesn't know
// where the panel is).
//
// Earth is treated as a sphere of radius 6371 km. The accuracy budget
// is set by the panel: bearing is rendered as 3 digits (1°
// quantisation), elevation as 1-2 digits — well looser than the
// ~0.3° error a spherical-vs-WGS84 simplification introduces at
// ISS altitude.
//
// Pure function, no global state. Cost: 3 sin + 3 cos + 1 sqrt + 1
// asin + 1 atan2 (~150 µs on RP2040 soft-float) — call once per
// frame at most.
//
// (added in phase 7.1++ when the iss_state contract moved from
//  HA-computed visibility/look-angles to raw HA pass-through with
//  on-device geometry)

#pragma once

#include <stdint.h>

namespace iss_geom {

struct LookAngles {
  float azimuth_deg;    // 0..360, compass convention (0=N, 90=E, 180=S, 270=W)
  float elevation_deg;  // -90..+90, 0 = horizon, +90 = zenith
};

// Compute the observer-frame look angles toward an ISS sub-satellite
// point at the given orbital altitude.
//
// All angles in degrees. observer_lon / iss_lon use +E / -W convention
// (matches both wheretheiss.at and our config.h LONGITUDE_DEG).
// altitude_km is height above Earth's surface (~400 for ISS).
LookAngles look_angles(float observer_lat_deg, float observer_lon_deg,
                       float iss_lat_deg,      float iss_lon_deg,
                       float altitude_km);

}  // namespace iss_geom
