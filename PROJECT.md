# ESP32-P4 ADSB Radar Display

A classic air traffic control radar display showing live aircraft within 50nm of Sydney, Australia, using the free ADSB.lol API.

![Project Status](https://img.shields.io/badge/status-production-brightgreen)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5.1-blue)
![Hardware](https://img.shields.io/badge/hardware-ESP32--P4-orange)

## Hardware

- **Board**: Waveshare ESP32-P4 WiFi6 Touch LCD
- **Display**: 800x800 round MIPI-DSI LCD (JD9365 controller)
- **RAM**: SPIRAM (PSRAM) for frame buffer
- **WiFi**: ESP32-C6 coprocessor via ESP-Hosted (SDIO)
- **Touch**: GT911 capacitive touch controller (not yet utilized)

## Features

### Current Implementation

**Visual Display:**
- Classic PPI (Plan Position Indicator) radar scope with rotating sweep
- 3 distance rings: 10nm, 25nm, 50nm (green, semi-transparent)
- Cardinal direction markers (N, E, S, W)
- Rotating sweep line with 30° fading trail (60-second full rotation)
- Color-coded aircraft blips by altitude:
  - **Yellow**: < 10,000 ft (low altitude - departures, arrivals, helicopters)
  - **Orange**: 10,000-25,000 ft (climbing/descending)
  - **White**: > 25,000 ft (cruising altitude)
- Aircraft labels showing callsign and altitude (hidden when > 20 aircraft for clarity)
- Status overlay showing aircraft count

**Data Processing:**
- Real-time ADSB data from api.adsb.lol
- 10-second polling interval with exponential backoff on errors
- Haversine distance calculation (accurate for short distances)
- True bearing calculation from home position
- Polar to Cartesian coordinate conversion
- Automatic pruning of stale aircraft (>60 seconds)
- Thread-safe aircraft storage (up to 64 aircraft)
- Position: -33.8127201, 151.2059618 (Sydney, Australia)
- Radius: 50 nautical miles

**Performance:**
- 60 Hz sweep animation (smooth rotation)
- Supports up to 64 aircraft tracked simultaneously
- Memory efficient: 195KB internal RAM, 28MB SPIRAM free
- TLS/SSL certificate verification for secure API access
- WiFi auto-reconnect on connection loss

## Technical Architecture

### Component Structure

```
radar_display/
├── main/
│   ├── main.c                 # Application initialization & orchestration
│   ├── radar_config.h         # Configuration constants (home position, colors, timing)
│   ├── wifi.c/h               # WiFi connectivity + NTP time sync
│   ├── adsb_client.c/h        # ADSB.lol API client (HTTP/TLS)
│   ├── aircraft_store.c/h     # Aircraft data management + coordinate conversion
│   └── radar_renderer.c/h     # LVGL-based radar visualization
├── components/
│   └── bsp_extra/             # Board support package extensions
└── managed_components/        # ESP-IDF managed dependencies
    ├── waveshare__esp32_p4_wifi6_touch_lcd_xc
    ├── lvgl__lvgl (v9.2.0)
    └── espressif__esp_hosted
```

### Data Flow

```
ADSB.lol API (HTTPS)
    ↓ (10-second polling)
adsb_client.c (HTTP client + JSON parser)
    ↓ (callback with raw aircraft data)
aircraft_store.c
    ├── Haversine distance calculation (nm)
    ├── Bearing calculation (0-360°)
    ├── Polar → Cartesian conversion
    └── Screen position (x, y pixels)
    ↓
radar_renderer.c
    ├── Create/update LVGL blip objects
    ├── Color by altitude
    ├── Position labels
    └── Update status overlay
    ↓
LVGL v9 (800x800 display)
    └── Hardware-accelerated rendering
```

### Key Algorithms

**Haversine Distance (Nautical Miles):**
```c
R = 3440.065 nm  // Earth radius in nautical miles
a = sin²(Δlat/2) + cos(lat1) × cos(lat2) × sin²(Δlon/2)
c = 2 × atan2(√a, √(1-a))
distance = R × c
```

**True Bearing:**
```c
y = sin(Δlon) × cos(lat2)
x = cos(lat1) × sin(lat2) - sin(lat1) × cos(lat2) × cos(Δlon)
bearing = atan2(y, x)  // Normalized to 0-360°
```

**Screen Coordinates:**
```c
pixels_per_nm = 390 / 50 = 7.8 px/nm
radius_px = distance_nm × pixels_per_nm
angle_rad = (bearing_deg - 90°) × π/180  // Rotate North to top
x = 400 + radius_px × cos(angle_rad)
y = 400 + radius_px × sin(angle_rad)
```

### Memory Management

- **Frame Buffer**: 800×800 pixels in SPIRAM (640KB)
- **Aircraft Storage**: 64 aircraft × ~100 bytes = 6.4KB
- **LVGL Objects**: Dynamic allocation in SPIRAM
- **HTTP Buffer**: 32KB for JSON responses
- **Thread Safety**: FreeRTOS mutex for aircraft store access

## Build & Flash

### Prerequisites

```bash
# ESP-IDF v5.5.1 installed at:
/Volumes/8TB/Projects/ESP32/esp-idf/

# WiFi credentials configured in:
main/radar_config.h
```

### Build Commands

```bash
cd /Volumes/8TB/Projects/ESP32/radar_display

# Activate ESP-IDF environment
source /Volumes/8TB/Projects/ESP32/esp-idf/export.sh

# Build project
idf.py build

# Flash to device
idf.py flash

# Monitor serial output
idf.py monitor

# Or all in one:
idf.py build flash monitor
```

### Configuration

Edit `main/radar_config.h` to customize:

```c
// Home position (Sydney, Australia)
#define HOME_LAT -33.8127201f
#define HOME_LON 151.2059618f

// Radar radius
#define RADAR_RADIUS_NM 50

// WiFi credentials
#define WIFI_SSID "your-network"
#define WIFI_PASSWORD "your-password"

// Colors (RGB)
#define COLOR_SWEEP_R 0x00
#define COLOR_SWEEP_G 0xFF
#define COLOR_SWEEP_B 0x00

// Timing
#define ADSB_POLL_INTERVAL_MS 10000  // 10 seconds
#define SWEEP_DEGREES_PER_FRAME 0.36f  // 60-second rotation
```

## API Reference

### ADSB.lol API

**Endpoint:**
```
https://api.adsb.lol/v2/point/{latitude}/{longitude}/{radius}
```

**Example:**
```
https://api.adsb.lol/v2/point/-33.8127201/151.2059618/50
```

**Response Format:**
```json
{
  "ac": [
    {
      "hex": "7c6b2d",
      "flight": "QFA123  ",
      "lat": -33.9461,
      "lon": 151.1792,
      "alt_baro": 35000,
      "gs": 456.7,
      "track": 128.4,
      ...
    }
  ],
  "now": 1735814400.0,
  "ctime": 1735814400123,
  "ptime": 245
}
```

**Rate Limits:** None specified (free API)
**Attribution:** Powered by https://adsb.lol

## Future Improvements

### 1. Touch Interface & Settings Screen

**Tap-to-Select Aircraft:**
- Touch detection using GT911 capacitive controller
- Highlight selected aircraft blip
- Show detailed popup:
  - Full callsign and hex code
  - Current altitude, speed, heading
  - Distance and bearing from home
  - Climb/descent rate
  - Aircraft type (if available from database)
  - Origin/destination (if available)

**Settings Screen (Touch Menu):**
- Access via touch gesture (e.g., long-press center, swipe from edge)
- Configurable options:
  - **Home Position**: Set custom lat/lon via touch keyboard or GPS
  - **Radar Radius**: 25nm, 50nm, 75nm, 100nm
  - **Display Mode**:
    - Classic green (current)
    - Modern blue
    - Night mode (red/amber)
  - **Label Display**: Always, Auto (<20 aircraft), Never
  - **Altitude Ranges**: Customize color thresholds
  - **WiFi Settings**: Change network without reflashing
  - **Brightness**: Adjust display backlight (0-100%)
  - **Update Interval**: 5s, 10s, 15s, 30s

**Implementation Notes:**
- Use LVGL's built-in touch input handling
- Create modal screen overlays for settings
- Store settings in NVS (non-volatile storage)
- Add "Reset to Defaults" option

### 2. Enhanced Data Display

**Flight Tracks:**
- Store historical positions (last 5-10 minutes)
- Draw dotted trail line showing aircraft path
- Fade older positions for visual clarity
- Useful for identifying approach patterns

**Altitude Display Improvements:**
- Show climb/descent arrows (↑↓) next to altitude
- Color-code rate of climb (green climbing, red descending)
- Show vertical speed in ft/min

**Multiple Home Positions:**
- Save up to 5 favorite locations
- Quick-switch between locations
- Useful for: Home, Work, Airport spotting locations

**Aircraft Categories:**
- Filter by type: Jets, Props, Helicopters, Military
- Toggle visibility per category
- Different symbols for different types (triangle for jets, square for props)

### 3. Advanced Filtering & Alerts

**Altitude Filtering:**
- Show only aircraft in specific altitude bands
- Example: Show only <10,000 ft for arrivals/departures
- Slider control for min/max altitude

**Distance Filtering:**
- Adjustable inner/outer rings
- Example: Focus on 10-25nm zone

**Audio Alerts:**
- Beep when aircraft enters specific zone
- Configurable alert zones (e.g., "Alert when aircraft < 5nm and < 2000ft")
- Different tones for different aircraft types

**Favorite Aircraft:**
- Watch list for specific callsigns (e.g., "QFA1", "VOZ")
- Highlight in different color
- Alert when detected

### 4. Network & Data Enhancements

**Multiple Data Sources:**
- Support for dump1090 (local receiver)
- OpenSky Network API (alternative source)
- ADSBexchange API
- User-selectable source in settings

**Offline Mode:**
- Record aircraft data to SD card
- Playback mode for reviewing past sessions
- Export to CSV/KML for analysis

**WiFi Improvements:**
- Captive portal for initial WiFi setup (no code changes needed)
- Show WiFi signal strength on display
- Auto-reconnect with visual indicator
- Support for WPA3 Enterprise

### 5. Performance & Visual Enhancements

**Frame Rate:**
- Increase sweep to 120 FPS for ultra-smooth animation
- Adaptive frame rate based on aircraft count

**3D Effects:**
- Pseudo-3D altitude representation (aircraft higher = lighter/smaller)
- Perspective tilt for more immersive view

**Weather Overlay:**
- Show weather radar overlay (if API available)
- Cloud coverage visualization
- Wind direction indicator

**Day/Night Mode:**
- Auto-switch based on sunset/sunrise times
- Dimmer colors at night to reduce eye strain
- Red-tinted display for night vision preservation

### 6. Data Logging & Statistics

**Session Statistics:**
- Total aircraft seen this session
- Busiest times of day
- Most common callsigns/airlines
- Highest/lowest altitude observed

**Database Integration:**
- SQLite on SD card for historical data
- Search by date/time
- View heatmaps of common flight paths

**Export Features:**
- Screenshot capture (save to SD card)
- Time-lapse recording (create video of 24-hour traffic)
- Export statistics to CSV

### 7. Hardware Additions

**GPS Module:**
- Auto-detect home position
- Portable radar (use anywhere)
- Automatic bearing correction

**SD Card:**
- Log all aircraft data
- Store settings
- Offline map tiles

**Battery:**
- Portable operation
- Power usage optimization
- Battery level indicator

**External Antenna:**
- Connect to local ADS-B receiver (dump1090)
- Better coverage, more aircraft
- Reduced internet dependency

## Implementation Priority

**Phase 1 (Easy Wins):**
1. Touch interface for aircraft selection
2. Settings stored in NVS (WiFi, brightness)
3. Day/night color schemes

**Phase 2 (Medium Complexity):**
1. Settings screen UI with LVGL
2. Flight tracks/history
3. Altitude filtering

**Phase 3 (Advanced Features):**
1. Multiple data sources (dump1090 support)
2. Audio alerts
3. Database logging

**Phase 4 (Hardware Extensions):**
1. GPS module integration
2. SD card logging
3. Battery operation

## License

MIT License - Free to use and modify

## Attribution

- **ADSB Data**: Powered by https://adsb.lol (free community-driven ADS-B network)
- **Display**: Waveshare ESP32-P4 800x800 Round LCD
- **Graphics**: LVGL v9.2.0 (https://lvgl.io)
- **WiFi**: ESP-Hosted by Espressif

## Credits

Developed for ESP32-P4 with Claude Code assistance.

**Key Components:**
- ESP-IDF v5.5.1
- LVGL v9.2.0 (embedded graphics library)
- cJSON (JSON parsing)
- mbedTLS (TLS/SSL)
- ESP-Hosted (WiFi over SDIO)

## Support

For issues or questions:
- Check serial monitor output (`idf.py monitor`)
- Verify WiFi credentials in `radar_config.h`
- Ensure ADSB.lol API is accessible: https://api.adsb.lol
- Heap statistics logged every 10 seconds

## Version History

**v1.0 (Current)** - January 2026
- ✅ Complete radar display with 50nm radius
- ✅ Live ADSB data from adsb.lol
- ✅ Color-coded altitude display
- ✅ Rotating sweep animation (60 FPS)
- ✅ Distance rings and cardinal markers
- ✅ Aircraft count status overlay
- ✅ Automatic stale aircraft pruning
- ✅ Thread-safe operation
- ✅ TLS certificate verification
- ✅ WiFi auto-reconnect

**Future Releases:**
- v1.1: Touch interface + settings screen
- v1.2: Flight tracks + enhanced filters
- v2.0: Multiple data sources + advanced features
