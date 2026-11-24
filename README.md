# InfoScreen

Departure board + weather forecast on a
[Waveshare ESP32-S3-Touch-LCD-2.8](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8)
(240x320, portrait).

- **Top half:** next 3 departures from a Swiss public transport stop, optionally
  only those passing a given stop, from [transport.opendata.ch](https://transport.opendata.ch).
- **Bottom half:** MeteoSwiss local forecast for a Swiss postcode: current
  temperature and icon, today's min/max/rain, and the next 12 hours (icons and
  rain chance per 3 h, hourly temperature line and rain bars).

## Setup

1. Install [PlatformIO](https://platformio.org/install/cli).
2. Copy the config and fill it in:
   ```sh
   cp .env.example .env
   ```
   Each key is documented in `.env.example`. `.env` is git-ignored.
3. Build, flash, monitor:
   ```sh
   pio run -t upload && pio device monitor
   ```

- `pio run -t upload` builds before uploading. Run `pio run -t clean` first if
  the build acts strangely.
- Quit the monitor with `Ctrl+C`.
- If the screen is upside down, set `SCREEN_ROTATION=2`.

### Finding your settings

- **`STATION_ID`**: search by name and take the `id`:
  ```sh
  curl -s "https://transport.opendata.ch/v1/locations?query=Bern+Bahnhof&type=station" | python3 -c 'import json,sys;[print(s["id"],s["name"]) for s in json.load(sys.stdin)["stations"]]'
  ```
- **`WEATHER_PLZ`**: the 4-digit postcode, e.g. `8001` for Zürich.

## Check the APIs without the board

Departures (same request and filter as the firmware; here Zürich, Bellevue via HB; set `ID` and `VIA` to your values):

```sh
ID=8576193 VIA=HB; curl -s "https://transport.opendata.ch/v1/stationboard?id=$ID&limit=10" | VIA=$VIA python3 -c 'import json,sys,time,os;v=os.environ["VIA"];n=time.time();r=[(j["category"]+j["number"],time.strftime("%H:%M",time.localtime(t)),"+%d"%d,j["to"]) for j in json.load(sys.stdin)["stationboard"] if not v or any(v in (p["station"]["name"] or "") for p in j["passList"]) for d in [j["stop"]["delay"] or 0] for t in [j["stop"]["departureTimestamp"]+d*60] if t>=n-30];[print(*x) for x in r[:3]]'
```

Weather (the API wants the postcode followed by `00`, here 8001 Zürich):

```sh
curl -s "https://app-prod-ws.meteoswiss-app.ch/v1/plzDetail?plz=800100" | python3 -c 'import json,sys;d=json.load(sys.stdin);print(d["currentWeather"]);print(d["forecast"][0])'
```

If these print data but the board doesn't show it, the problem is on the board
(WiFi, TLS, memory). The serial monitor logs `HTTP <code>` or `... JSON: ...`
errors.

## Fonts

By default all text uses the graphics library's built-in 6x8 pixel font,
scaled up for larger text, so there's nothing to download. It works, but looks
blocky when scaled.

For nicer text, put BDF bitmap fonts in a folder under `fonts/` and set
`FONT` in `.env` to its name. For each text style the build picks the largest
font in the folder that fits and prints what it chose; styles with nothing
fitting stay built-in. Fonts that ship many sizes work best.

[Terminus](https://terminus-font.sourceforge.net) 4.49.1 is included in
`fonts/terminus/` with every size the layout uses: set `FONT=terminus` in
`.env`. [Spleen](https://github.com/fcambus/spleen) also fits all but one size.

Font requirements:

- **BDF format.** It's the standard format for bitmap fonts, and most monospace
  bitmap fonts ship it (Terminus, Spleen, Cozette, Tamzen, Unifont, ...).
  TTF/OTF fonts are outlines, not bitmaps. Convert them first with e.g.
  `otf2bdf` or FontForge at the pixel size you want.
- **Monospace.** Every character must be the same width, both for the column
  layout and because each update overwrites the old text in place.
  Proportional fonts are skipped.
- **Size.** The layout has room for character cells up to 6x12 (small labels),
  8x14 (destinations), 8x16 (headers), 12x24 (departure rows) and 16x32
  (current temperature). Smaller files are used where nothing bigger fits.

## Notes

- The weather comes from the JSON endpoint the official MeteoSwiss app uses. It
  is not a documented public API and could change without notice.

## License

MIT, see `LICENSE`. The included Terminus font (`fonts/terminus/`) is
Copyright (C) 2020 Dimitar Toshkov Zhekov and licensed under the SIL Open Font
License 1.1, see `fonts/terminus/OFL.TXT`.
