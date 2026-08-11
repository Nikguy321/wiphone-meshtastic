# Woods backplate — order list

**Compiled 2026-08-11.** Two vendors only: **Adafruit** (3 items, one order) and **Amazon**
(everything else). Prices and stock were checked live on 2026-08-11 — re-check before ordering.

Design context is in [`woods-backplate.md`](woods-backplate.md); the wiring is in
[`woods-backplate.svg`](woods-backplate.svg). **Read the power section there before buying** — the
architecture changed on 2026-08-11.

## Already on hand — do not re-order

| Part | Note |
|---|---|
| HopeRF RFM95W (SX1276, 915 MHz) ×2 | bare `ANA` pad, no u.FL on the module |
| HGLRC M100 Mini GPS | 3.3–5 V in, UART, 4 pads |
| 1S LiPo, larger capacity | must have its own protection board |
| WiPhone Header Breakout (screw terminals) | breaks out all ten signals needed |

---

## Order 1 — Adafruit  (~$28 + shipping)

| # | Part | Product ID | Price | Stock checked 2026-08-11 |
|---|---|---|---|---|
| 1 | **SMA to u.FL/IPEX RF Adapter Cable** | [851](https://www.adafruit.com/product/851) | $3.95 | ✅ In stock |
| 2 | **PowerBoost 1000C** — 5 V LiPo charger + boost | [2465](https://www.adafruit.com/product/2465) | $19.95 | ✅ 99 in stock |
| 3 | **TLV62569 3.3 V Buck Breakout** (1.2 A, **EN pin**) | [4711](https://www.adafruit.com/product/4711) | $3.95 | ✅ In stock |

### 1 — Adafruit 851, the antenna pigtail
RG178, 15 cm, with a **panel-mount SMA connector on the end** — so this is the pigtail *and* the
bulkhead in one part, which is exactly what a 3D-printed back cover wants. Drill the cover for its
nut.

⚠ It is **SMA, not RP-SMA.** The whip below must be **SMA male**. They are not compatible and
Adafruit sells an RP-SMA version of this same cable — order the right one.

⚠ This is the joint COVEY's deafness investigation currently suspects. Strain-relieve it where it
crosses the battery and make sure the u.FL is properly **clicked**, not resting.

### 2 — PowerBoost 1000C, the whole battery half
This replaces the IP5306 module idea, and it is a better part for this job:

- **5.2 V output, not 5.0.** That matters here: the phone's own USB only reaches the internal 5 V
  node at ~4.6–4.7 V after the fuse, diode and limiter, so **5.2 V means the pack reliably wins**
  the hand-off and a flashing cable never fights it.
- **1 A LiPo charger with its own microUSB jack** — the "own USB-in on the charger circuit" you
  wanted, built in.
- **Smart load-sharing** — Adafruit: it "will automatically switch over to the USB power when
  available, instead of continuously charging/draining the battery… fine for use as a 'UPS'".
- **EN pin** — tie low to kill the pack output completely. That is your flashing kill-switch if you
  ever want one, and a front-panel switch if you want that.
- **No IP5306 no-load shutdown.** This was the biggest risk in the IP5306 route: those modules cut
  the boost under ~45–100 mA load. The TPS61090 has no such behaviour. This alone justifies the
  price difference.
- 23 × 45 × 10 mm, 6 g. **Do not solder the included USB-A jack** — you are not charging other
  devices, and leaving it off saves height in the case.
- Low-battery LED trips at 3.2 V; JST for the cell; breakouts for EN / BAT / GND / 5V / LBO.

⚠ **It will not run without a LiPo attached.** Not optional.
⚠ 1 A total. The phone's charger plus the plate's ~100 mA (drawn at 5.2 V) can approach that while
charging hard. It current-limits rather than failing, but do not add more 5 V load.

### 3 — Adafruit 4711, the plate's 3.3 V rail
Input 3.4–5.5 V, output 3.3 V at up to 1.2 A, 90–95% efficient, **15.2 × 10.0 × 2.9 mm**. Adafruit:
"There's also an ENable pin, tie it low to shut down the output completely."

**Feed it from the PowerBoost's 5.2 V, not from VBAT.** Its input minimum is 3.4 V, so straight off
a 1S cell it would be in dropout by the bottom of the discharge — exactly when you are furthest
from the truck.

**The EN gate** (stops the plate back-powering the ESP32 through its GPIO clamp diodes while the
phone is switched off but the pack is still on):

    phone's 3.3V pogo pin ─────────────┬──── EN
                                   4.7 kΩ
                                       └──── GND

⚠ Adafruit's breakouts normally pull EN up to VIN through ~100 kΩ; with 4.7 kΩ down that sits at
~0.23 V when the phone is off, safely under the threshold. **Measure EN's open-circuit voltage and
confirm the pull-up before trusting the divider** — if it is much stiffer than 100 kΩ, drop the
4.7 kΩ accordingly.

---

## Order 2 — Amazon

Searches rather than permalinks: these listings rotate constantly, so match the **spec**, not the
seller. All were present and Prime-eligible on 2026-08-11.

| # | What | Search / spec to match | ~Price |
|---|---|---|---|
| 4 | **915 MHz whip antenna** | `915MHz LoRa antenna SMA male 3dBi` — must be **SMA male**. 2-packs ~$10–12; many bundle a spare u.FL pigtail | ~$11 |
| 5 | **u.FL / IPEX SMT receptacles** | `U.FL IPEX SMT receptacle connector PCB mount` — optional, see below | ~$8 |
| 6 | **Ceramic capacitor assortment** | `ceramic capacitor assortment kit` — you need 2 × 100 nF, 1 × 10 µF, 1 × 22–47 µF | ~$12 |
| 7 | **Resistor assortment** | need 1 × 4.7 kΩ (the EN pull-down) and optionally 1 × 10 kΩ (RFM RESET pull-up) | ~$10 |
| 8 | **Polyfuse, ~1 A hold** | `PPTC resettable fuse 1A assortment` — one in the external battery leg | ~$8 |
| 9 | **JST-PH 2-pin pigtails** | so the pack can be split off for separate charging | ~$8 |
| 10 | **Silicone hookup wire, 26–30 AWG** | thin and flexible — it has to survive the case closing | ~$15 |

Items 6–10 are commodity assortments you will reuse. If you already have them, the Amazon order is
just the antenna (and the u.FL receptacles if you want them).

### On item 5 — you may not need it
Two ways to get from the RFM95W's bare `ANA` pad to the pigtail:

- **(a) With a receptacle.** Solder a u.FL SMT jack to `ANA` + adjacent GND; the 851 clips on.
- **(b) Without one.** Cut the u.FL plug off the 851 and solder the coax centre to `ANA`, braid to
  GND. **One fewer RF joint, and no connector to work loose** — and given COVEY's u.FL is a ranked
  suspect in its own deafness investigation, that is not a small argument.

**Recommendation: buy the receptacles anyway (they are cheap) and decide at the bench.** The reason
is diagnostic, not mechanical: with a jack fitted you can A/B the whip against the stock plate's
known-good FPC antenna, which is a far better test than COVEY has ever had. If the plate ever seems
deaf, that swap answers it in a minute. Solder direct only once you are done experimenting.

⚠ Keep the `ANA`-to-connector run **under ~5 mm**, 50 Ω, grounded both sides, either way.

---

## Not being ordered, and why

- **Pololu D24V5F3** — was the 3.3 V regulator pick at $8.95; the Adafruit TLV62569 does the same
  job with an EN pin for $3.95 and keeps the order to two vendors.
- **IP5306 module** — superseded by the PowerBoost 1000C. Amazon's are unlabelled clones where the
  no-load shutdown behaviour is unverifiable, and that behaviour is the failure mode that would
  strand you in the woods.
- **Anything from DigiKey or Mouser** — both are behind bot detection I will not work around, so
  nothing here depends on them. The canonical u.FL receptacle (Hirose `U.FL-R-SMT-1(10)`) is a
  DigiKey part and was showing backordered; the Amazon generics are the practical substitute.
- **A separate SMA bulkhead connector** — the 851 already has one.
- **Level shifters** — not needed. ESP32, RFM95W and the GPS UART are all 3.3 V logic.

## Before you order, two bench checks worth an hour

1. **Confirm the 5 V pin takes power in.** Plate off, no USB: feed the 5 V pin from a supply at 5 V
   with a 200 mA limit and watch for charging current. This validates the whole power architecture,
   which is inferred from WiPhone's docs plus the existence of the Mega pack — not measured.
2. **Confirm `ENABLE_DAUGHTER_33V` is not gating the 3.3 V pogo pin.** Firmware never asserts it
   (`WiPhone.ino:1222` is commented out) and the stock LoRa plate works anyway, so it is almost
   certainly free — but the EN gate above depends on that pin being live.
