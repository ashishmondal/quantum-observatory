// Implementation of include/ds3231.h. See header for rationale.
//
// Uses the Arduino Wire1 API (earlephilhower core supports configurable
// pins via setSDA/setSCL/setClock) rather than raw pico-sdk i2c calls,
// to stay consistent with the Adafruit libraries already in the build.
//
// BCD encoding/decoding and the days-from-civil math are inline below
// rather than pulled into separate helpers — this is the only file that
// needs them. (The inverse, civil_from_days, lives in time_of_day.cpp.)

#include "ds3231.h"

#include <Arduino.h>
#include <Wire.h>

#include "config.h"

namespace ds3231 {

namespace {

constexpr uint8_t REG_TIME   = 0x00;  // start of 7-byte time block
constexpr uint8_t REG_STATUS = 0x0F;  // OSF in bit 7
constexpr uint8_t REG_TEMP   = 0x11;  // signed int8 °C; reg 0x12 has 0.25° fraction (ignored)

inline uint8_t bcd2dec(uint8_t b) noexcept {
  return static_cast<uint8_t>((b >> 4) * 10u + (b & 0x0Fu));
}
inline uint8_t dec2bcd(uint8_t d) noexcept {
  return static_cast<uint8_t>(((d / 10u) << 4) | (d % 10u));
}

// Howard Hinnant's days_from_civil (forward of civil_from_days in
// time_of_day.cpp). Returns days since 1970-01-01 for a proleptic
// Gregorian (y, m, d). Pure integer.
//   https://howardhinnant.github.io/date_algorithms.html#days_from_civil
int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d) noexcept {
  y -= (m <= 2);
  const int32_t  era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);                  // [0, 399]
  const uint32_t doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;  // [0, 365]
  const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;              // [0, 146096]
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

// Civil breakdown — duplicate of tod::date_from_local_epoch's internals
// but local to this file so we don't need to expose a second variant
// in the time_of_day API (which only deals with date, not h/m/s).
void civil_from_epoch(int32_t epoch, int32_t* y, uint32_t* mo, uint32_t* d,
                      uint32_t* h, uint32_t* mi, uint32_t* s) noexcept {
  int32_t days = epoch / 86400;
  int32_t sod  = epoch - days * 86400;
  if (sod < 0) { sod += 86400; --days; }
  *h  = static_cast<uint32_t>(sod / 3600);
  *mi = static_cast<uint32_t>((sod % 3600) / 60);
  *s  = static_cast<uint32_t>(sod % 60);

  days += 719468;
  const int32_t  era = (days >= 0 ? days : days - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(days - era * 146097);
  const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  const int32_t  yr  = static_cast<int32_t>(yoe) + era * 400;
  const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const uint32_t mp  = (5u * doy + 2u) / 153u;
  const uint32_t dy  = doy - (153u * mp + 2u) / 5u + 1u;
  const uint32_t mn  = mp < 10u ? mp + 3u : mp - 9u;
  *y  = yr + (mn <= 2u ? 1 : 0);
  *mo = mn;
  *d  = dy;
}

// Day-of-week per RFC: 1=Sun..7=Sat (DS3231 vendor convention). We don't
// rely on this on read (we recompute from the date), but we have to write
// *something* sensible. 1970-01-01 was a Thursday → weekday index 5.
uint8_t dow_from_days(int32_t days_since_epoch) noexcept {
  int32_t w = (days_since_epoch + 4) % 7;  // 0=Sun..6=Sat
  if (w < 0) w += 7;
  return static_cast<uint8_t>(w + 1);      // 1=Sun..7=Sat
}

}  // namespace

void begin() {
  Wire1.setSDA(PIN_RTC_SDA);
  Wire1.setSCL(PIN_RTC_SCL);
  Wire1.setClock(RTC_I2C_HZ);
  Wire1.begin();
  // Intentionally do NOT touch the control or status registers here.
  // FR-9.6 requires us to observe the oscillator-stop flag on first
  // boot; clearing it eagerly would defeat that.
}

bool read(int32_t* epoch_local) {
  // Set register pointer to 0x00.
  Wire1.beginTransmission(DS3231_I2C_ADDR);
  Wire1.write(REG_TIME);
  if (Wire1.endTransmission(false) != 0) {  // repeated start
    return false;
  }
  if (Wire1.requestFrom(static_cast<uint8_t>(DS3231_I2C_ADDR),
                        static_cast<uint8_t>(7)) != 7) {
    return false;
  }
  uint8_t raw[7];
  for (int i = 0; i < 7; ++i) raw[i] = static_cast<uint8_t>(Wire1.read());

  const uint8_t sec = bcd2dec(raw[0] & 0x7F);
  const uint8_t min = bcd2dec(raw[1] & 0x7F);
  // We assume 24h mode (chip default). If bit 6 is ever set we'd have
  // to do the 12h dance (HARDWARE.md notes); easier to just keep the
  // chip in 24h forever via write().
  const uint8_t hour = bcd2dec(raw[2] & 0x3F);
  // raw[3] = day-of-week — recomputed on read, ignored.
  const uint8_t day  = bcd2dec(raw[4] & 0x3F);
  const uint8_t mon  = bcd2dec(raw[5] & 0x1F);  // bit 7 = century, ignore
  const uint16_t yr  = 2000u + bcd2dec(raw[6]);

  const int32_t days = days_from_civil(static_cast<int32_t>(yr), mon, day);
  const int32_t epoch = days * 86400
                      + static_cast<int32_t>(hour) * 3600
                      + static_cast<int32_t>(min)  * 60
                      + static_cast<int32_t>(sec);
  if (epoch_local) *epoch_local = epoch;
  return true;
}

bool write(int32_t epoch_local) {
  int32_t  y;
  uint32_t mo, d, h, mi, s;
  civil_from_epoch(epoch_local, &y, &mo, &d, &h, &mi, &s);
  if (y < 2000 || y > 2099) return false;  // BCD year is two digits

  const int32_t days = days_from_civil(y, mo, d);
  const uint8_t dow  = dow_from_days(days);

  Wire1.beginTransmission(DS3231_I2C_ADDR);
  Wire1.write(REG_TIME);
  Wire1.write(dec2bcd(static_cast<uint8_t>(s)));
  Wire1.write(dec2bcd(static_cast<uint8_t>(mi)));
  Wire1.write(dec2bcd(static_cast<uint8_t>(h)));   // 24h mode (bit 6 = 0)
  Wire1.write(dow);                                // already 1..7
  Wire1.write(dec2bcd(static_cast<uint8_t>(d)));
  Wire1.write(dec2bcd(static_cast<uint8_t>(mo)));  // century bit cleared
  Wire1.write(dec2bcd(static_cast<uint8_t>(y - 2000)));
  return Wire1.endTransmission() == 0;
}

bool oscillator_stopped() {
  Wire1.beginTransmission(DS3231_I2C_ADDR);
  Wire1.write(REG_STATUS);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom(static_cast<uint8_t>(DS3231_I2C_ADDR),
                        static_cast<uint8_t>(1)) != 1) {
    return false;
  }
  const uint8_t status = static_cast<uint8_t>(Wire1.read());
  return (status & 0x80u) != 0;
}

void clear_oscillator_stopped() {
  Wire1.beginTransmission(DS3231_I2C_ADDR);
  Wire1.write(REG_STATUS);
  if (Wire1.endTransmission(false) != 0) return;
  if (Wire1.requestFrom(static_cast<uint8_t>(DS3231_I2C_ADDR),
                        static_cast<uint8_t>(1)) != 1) {
    return;
  }
  const uint8_t status = static_cast<uint8_t>(Wire1.read());
  Wire1.beginTransmission(DS3231_I2C_ADDR);
  Wire1.write(REG_STATUS);
  Wire1.write(static_cast<uint8_t>(status & 0x7Fu));
  Wire1.endTransmission();
}

bool read_temp_c(int8_t* celsius) {
  Wire1.beginTransmission(DS3231_I2C_ADDR);
  Wire1.write(REG_TEMP);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom(static_cast<uint8_t>(DS3231_I2C_ADDR),
                        static_cast<uint8_t>(1)) != 1) {
    return false;
  }
  // Reg 0x11 is signed two's-complement int8; the Wire1 buffer hands
  // it back as a uint8_t, so reinterpret. Range −64..+127 °C.
  const uint8_t raw = static_cast<uint8_t>(Wire1.read());
  if (celsius) *celsius = static_cast<int8_t>(raw);
  return true;
}

}  // namespace ds3231
