#include <esp_task_wdt.h>
#include <Arduino.h>

#include <TFT_eSPI.h>
#include <SPI.h>

#include <WiFi.h>
#include <InfluxDbClient.h>

#include "Free_Fonts.h" // Include the header file attached to this sketch
#include "config.h"

TFT_eSPI tft = TFT_eSPI(); // Invoke custom library

/* RX on PM5003 which we transmit to */
const int PM_TX_GPIO = 17;

/* TX on PM5003 which we receive from */
const int PM_RX_GPIO = 16;

const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;

InfluxDBClient influxClient;
unsigned long influxReportingTime;

void setup_wifi();
void setup_display();
void testdrawtext(const char *text, uint16_t color);
void tftPrintTest();

//20 seconds WDT
#define WDT_TIMEOUT 20

void setup()
{
  // put your setup code here, to run once:
  Serial.begin(115200);
  Serial.println("Starting.");
  esp_task_wdt_init(WDT_TIMEOUT, true); //enable panic so ESP32 restarts
  esp_task_wdt_add(NULL); //add current thread to WDT watch
  // we shouldn't take longer than 20 seconds to initialize, but if influx
  // client later hangs due to unrecoverable WiFi issue, then we'll reset

  setup_wifi();
  configTzTime(LOCAL_TZ, "pool.ntp.org", "time.nis.gov");

  influxClient.setConnectionParamsV1(INFLUX_URL, INFLUX_DB, INFLUX_USER, INFLUX_PASSWORD);

  if (influxClient.validateConnection())
  {
    Serial.print("Connected to InfluxDB: ");
    Serial.println(influxClient.getServerUrl());
  }
  else
  {
    Serial.print("InfluxDB connection failed: ");
    Serial.println(influxClient.getLastErrorMessage());
  }

  setup_display();
  Serial2.begin(9600);
  Serial2.setTimeout(250);
}

void setup_display()
{
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);

  // Set "cursor" at top left corner of display (0,0) and select font 4
  // tft.setCursor(0, 0, 4);

  tft.setFreeFont(&FreeSans18pt7b);
  tft.drawString("PM2.5:", 0, 20, GFXFF);
  tft.drawString("PM10:", 0, 20 + tft.fontHeight(GFXFF), GFXFF);
}

void update_display(float pm2_5_aqi, float pm10_aqi, unsigned int pm2_5_color, unsigned int pm10_color)
{
  // tft.setCursor(0, 10, 6);
  // tft.setCursor(0, 20);
  int xpos = tft.width() / 2;
  tft.setTextPadding(tft.width() / 2);
  tft.setTextColor(pm2_5_color, TFT_BLACK);
  tft.drawFloat(pm2_5_aqi, 1, xpos, 20, GFXFF);
  tft.setTextColor(pm10_color, TFT_BLACK);
  tft.drawFloat(pm10_aqi, 1, xpos, 20 + tft.fontHeight(GFXFF), GFXFF);
  unsigned int overall;
  if (pm2_5_aqi > pm10_aqi)
  {
    overall = pm2_5_color;
  }
  else
  {
    overall = pm10_color;
  }
  int32_t ypos = 20 + 2 * tft.fontHeight(GFXFF);
  int width = 2 * tft.textWidth("M");
  char str[128];
  snprintf(str, sizeof(str), " ypos %d, tft width %d, box width %d, box height %d", ypos, tft.width(), width, tft.fontHeight(GFXFF));
  Serial.println(str);
  tft.fillRect(tft.width() - width, ypos, width, ypos + tft.fontHeight(GFXFF), overall);
}

struct pms_sample
{
  struct
  {
    uint16_t pm2_5_ugm3;
    uint16_t pm1_0_ugm3;
    uint16_t pm10_ugm3;
  } standard;
  struct
  {
    uint16_t pm2_5_ugm3;
    uint16_t pm1_0_ugm3;
    uint16_t pm10_ugm3;
  } atmospheric;
  struct
  {
    uint16_t gt0_3um_dl;
    uint16_t gt0_5um_dl;
    uint16_t gt1um_dl;
    uint16_t gt2_5um_dl;
    uint16_t gt5um_dl;
    uint16_t gt10um_dl;
  } bins;
};

/* Air Now AQI lookup tables for PM2.5 ug/m^3 per
https://www.airnow.gov/publications/air-quality-index/technical-assistance-document-for-reporting-the-daily-aqi
*/

// Colour order is RGB 5+6+5 bits each
#define TFT_COLOR(red, green, blue) (red << 11 | green << 5 | blue)

struct aqi_entry
{
  int aqi_lo;
  int aqi_hi;
  float breakpoint_lo;
  float breakpoint_hi;
  unsigned int aqi_color;
};

struct aqi_entry pm2_5_lut[] = {
    {0, 50, 0.0, 12.1, TFT_COLOR(0, 56, 0)},        // green
    {51, 100, 12.1, 35.5, TFT_COLOR(31, 63, 0)},    // yellow
    {101, 150, 35.5, 55.5, TFT_COLOR(31, 31, 0)},   // orange
    {151, 200, 55.5, 150.5, TFT_COLOR(31, 0, 0)},   // red
    {201, 300, 150.5, 250.5, TFT_COLOR(18, 16, 8)}, // purple
    {301, 400, 250.5, 350.5, TFT_COLOR(17, 0, 4)},  // maroon
    {401, 500, 350.5, 500.5, TFT_COLOR(17, 0, 4)},  // maroon
};

struct aqi_entry pm10_lut[] = {
    {0, 50, 0.0, 55, TFT_COLOR(0, 56, 0)},
    {51, 100, 55, 155, TFT_COLOR(31, 63, 0)},
    {101, 150, 155, 255, TFT_COLOR(31, 31, 0)},
    {151, 200, 255, 355, TFT_COLOR(31, 0, 0)},
    {201, 300, 355, 425, TFT_COLOR(18, 16, 8)}, // purple
    {301, 400, 425, 505, TFT_COLOR(17, 0, 4)},
    {401, 500, 505, 605, TFT_COLOR(17, 0, 4)},
};

float aqi_for_pm(struct aqi_entry *lut, int n_lut, uint16_t pmcount, unsigned int *aqi_color)
{
  for (int i = 0; i < n_lut; i++)
  {
    aqi_entry *e = &lut[i];
    if (pmcount >= e->breakpoint_lo && pmcount < e->breakpoint_hi)
    {
      *aqi_color = e->aqi_color;
      return e->aqi_lo + (e->aqi_hi - e->aqi_lo) / (e->breakpoint_hi - e->breakpoint_lo) * (pmcount - e->breakpoint_lo);
    }
  }
  *aqi_color = TFT_WHITE;
  return -1;
}

uint16_t decode_uint16(uint8_t *buf)
{
  return (*buf << 8 | *(buf + 1));
}

bool decode(uint8_t *buf, struct pms_sample *s)
{
  if (buf[0] != 0x42 || buf[1] != 0x4d)
  {
    return false;
  }
  uint16_t sum = 0;
  for (int i = 0; i < 30; i++)
  {
    sum += buf[i];
  }
  uint16_t check = decode_uint16(buf + 30);
  if (check != sum)
  {
    return false;
  }
  s->standard.pm1_0_ugm3 = decode_uint16(buf + 4);
  s->standard.pm2_5_ugm3 = decode_uint16(buf + 6);
  s->standard.pm10_ugm3 = decode_uint16(buf + 8);
  s->atmospheric.pm1_0_ugm3 = decode_uint16(buf + 10);
  s->atmospheric.pm2_5_ugm3 = decode_uint16(buf + 12);
  s->atmospheric.pm10_ugm3 = decode_uint16(buf + 14);
  s->bins.gt0_3um_dl = decode_uint16(buf + 16);
  s->bins.gt0_5um_dl = decode_uint16(buf + 18);
  s->bins.gt1um_dl = decode_uint16(buf + 20);
  s->bins.gt2_5um_dl = decode_uint16(buf + 22);
  s->bins.gt5um_dl = decode_uint16(buf + 24);
  s->bins.gt10um_dl = decode_uint16(buf + 26);
  return true;
}

void loop()
{
  // put your main code here, to run repeatedly:
  // delay(1000);
  uint8_t buf[128];
  size_t n = Serial2.readBytes(buf, sizeof(buf));
  if (n > 0)
  {
    for (size_t i = 0; i < n; i++)
    {
      char str[8];
      snprintf(str, sizeof(str), "%02x:", buf[i]);
      Serial.print(str);
    }
    char str[12];
    snprintf(str, sizeof(str), " %d read", n);
    Serial.println(str);

    struct pms_sample s;
    if (decode(buf, &s))
    {
      unsigned int pm2_5_color, pm10_color;
      float pm2_5_aqi = aqi_for_pm(pm2_5_lut, sizeof(pm2_5_lut) / sizeof(pm2_5_lut[0]), s.atmospheric.pm2_5_ugm3, &pm2_5_color);
      float pm10_aqi = aqi_for_pm(pm10_lut, sizeof(pm10_lut) / sizeof(pm10_lut[0]), s.atmospheric.pm10_ugm3, &pm10_color);
      char str[128];
      snprintf(str, sizeof(str), "AQI for pm2.5 %0.2f\nAQI for pm10 %0.2f\nstd pm1.0 %d, pm2.5 %d, pm10 %d\natm pm1.0 %d, pm2.5 %d, pm10 %d\n>0.3 %d, 0.5 %d, >1.0 %d, >2.5 %d, >5.0 %d, >10.0 %d",
               // LRAPA AQI %0.2f\n 0.5 * s.atmospheric.pm2_5_ugm3 - 0.66,
               pm2_5_aqi,
               pm10_aqi,
               s.standard.pm1_0_ugm3, s.standard.pm2_5_ugm3, s.standard.pm10_ugm3,
               s.atmospheric.pm1_0_ugm3, s.atmospheric.pm2_5_ugm3, s.atmospheric.pm10_ugm3,
               s.bins.gt0_3um_dl, s.bins.gt0_5um_dl, s.bins.gt1um_dl, s.bins.gt2_5um_dl, s.bins.gt5um_dl, s.bins.gt10um_dl);
      Serial.println(str);
      update_display(pm2_5_aqi, pm10_aqi, pm2_5_color, pm10_color);
      if (millis() > influxReportingTime)
      {
        Serial.println("logging to influx");
        Point measurement("particulates");
        measurement.addField("pm2_5_lrapa_aqi", pm2_5_aqi);
        measurement.addField("pm10_lrapa_aqi", pm10_aqi);
        measurement.addField("std_1_0_ugm3", s.standard.pm1_0_ugm3);
        measurement.addField("std_2_5_ugm3", s.standard.pm2_5_ugm3);
        measurement.addField("std_10_ugm3", s.standard.pm10_ugm3);
        measurement.addField("atm_1_0_ugm3", s.atmospheric.pm1_0_ugm3);
        measurement.addField("atm_2_5_ugm3", s.atmospheric.pm2_5_ugm3);
        measurement.addField("atm_10_ugm3", s.atmospheric.pm10_ugm3);
        measurement.addField("bins_gt0_3_um_dl", s.bins.gt0_3um_dl);
        measurement.addField("bins_gt0_5_um_dl", s.bins.gt0_5um_dl);
        measurement.addField("bins_gt1_um_dl", s.bins.gt1um_dl);
        measurement.addField("bins_gt2_5_um_dl", s.bins.gt2_5um_dl);
        measurement.addField("bins_gt5_um_dl", s.bins.gt5um_dl);
        measurement.addField("bins_gt10_um_dl", s.bins.gt10um_dl);
        measurement.addTag("location", "living room");
        influxClient.writePoint(measurement);
        influxReportingTime += 15000;
      }
      esp_task_wdt_reset(); // keep ESP alive
    }
  }
}

void setup_wifi()
{
  delay(10);
  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}
