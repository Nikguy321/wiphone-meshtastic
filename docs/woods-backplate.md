# Stretch goal — the "woods" backplate

**Status: DESIGN NOTES ONLY. Nothing built, nothing ordered, no firmware written.**
Captured 2026-08-10 so the reasoning is not lost. Diagram: [`woods-backplate.svg`](woods-backplate.svg).

A fatter, higher-capacity backplate for time in the woods: the LoRa radio, a GPS, a real
915 MHz whip on an SMA bulkhead, and an expanded battery — on a screw-terminal plate instead
of the stock LoRa one.

## Parts on hand

| Part | Notes |
|---|---|
| HopeRF **RFM95W** (SX1276, 915 MHz), ×2 | Same module the stock LoRa plate uses. Bare **`ANA`** pad — no u.FL on the module itself |
| **HGLRC M100 Mini** GPS | u-blox, UART, 4 pads: `GND / 5V / TX / RX` |
| Larger 1S battery | |
| Screw-terminal backplate | No LoRa chip on it — the RFM95W would be added |

## What the stock LoRa plate (v2.0) actually carries

Read off a photo of the real board, so treat placement as certain and **values as unknown**.

| Ref | Part | Almost certainly |
|---|---|---|
| U1 | RFM95W (marked RF96) | the radio |
| **J5** | **IPEX MHF / u.FL jack** | the antenna connector the module lacks |
| C1 | one capacitor | supply decoupling / bulk |
| R1, R2 | pair below the module | SPI series damping, or a NSS/RESET pull-up |
| R6 | sits between the module and J5 | in or beside the **RF path** — most likely a 0 Ω link |
| R7, R11 | pair by the antenna slot | 0 Ω build options / pull-ups |
| — | FPC antenna on 3M 300LSE + coax to J5 | stock antenna |
| — | "PCB Slot" cutout | cable exit for an external antenna |

⚠ **The resistor VALUES have not been established.** They are unmarked 0402/0603. R6's placement
makes "0 Ω RF link" the obvious read, and obvious is not verified — a non-zero series part in a
50 Ω path would be a matching component that must be copied exactly. Get the v2.0 schematic
(WiPhone published this as open hardware) or measure R1/R2/R6/R7/R11 with the plate off the phone.

## ⚡ The one non-obvious design decision: leave DIO0 unconnected

`Hardware.h` has **`RFM95_INT 38`**, and the pogo header calls GPIO 38 **`DB_RXD`**. The stock
plate's silkscreen lists `INT`, so v2.0 really does tie the radio's DIO0 to that pin.

That looks fatal for the GPS, because **GPIO 34–39 are input-only on the ESP32**, making GPIO 38
the only pin on the header that can serve as a UART RX. The GPS must have it.

**It is free.** `mesh_phy.cpp` drives CS and **polls** — it never calls `attachInterrupt` and never
references `RFM95_INT`. Only the legacy `lora.cpp` (RadioHead) uses the interrupt, and that path is
gated off whenever `MESHTASTIC_PHY` is defined in `config.h`.

So on the woods plate: **do not populate the DIO0 → GPIO 38 connection**, and route the GPS TX
there instead. Everything else about the radio wiring can be copied from v2.0 verbatim. This is
the single deliberate deviation from the reference design, and it is only safe because the
Meshtastic PHY polls — **if the PHY is ever changed to be interrupt-driven, this plate breaks.**

## Pin map

| Signal | Pin | Notes |
|---|---|---|
| SPI MISO | GPIO 12 | |
| SPI MOSI | GPIO 13 | |
| SPI SCK | GPIO 14 | |
| RFM95W NSS | GPIO 27 | `RFM95_CS` |
| RFM95W RESET | — | `RFM95_RST -1`; not connected on the stock plate either |
| RFM95W DIO0 | — | **deliberately omitted**, see above |
| GPS TX → ESP32 RX | GPIO 38 | input-only pin; the GPS must have this one |
| GPS RX ← ESP32 TX | GPIO 32 | |
| RFM95W power | 3.3 V | |
| GPS power | 5 V | ⚠ see below |
| External battery | VB+ / GND | ⚠ see below |

No level shifters: the ESP32, the RFM95W and the GPS UART are all 3.3 V logic. The GPS takes 5 V
on its power pin only.

## Measure these three things before committing to a PCB

1. **Is the header's 5 V live on BATTERY, or only when USB is plugged in?** This one could sink the
   whole idea — a USB-only 5 V rail means the GPS is dead exactly when you are in the woods, which
   is the entire point of it. Fallbacks: feed the M100 from 3.3 V (confirm its regulator accepts
   it) or boost from VB+.
2. **Can the 3.3 V rail take RFM95W TX peaks?** ~120 mA at +20 dBm. Scope the rail mid-transmit.
3. **Does the screw-terminal plate break out all ten signals?** MISO, MOSI, SCK, CS, 38, 32, 5 V,
   3.3 V, GND, VB+. Count them before designing around them.

## 🔴 The battery, and why the safe option is not the obvious one

**Paralleling two packs at unequal charge is what destroyed both packs' protection FETs in July
2026** and caused the whole no-boot saga. Voltage-matching before every connection works, but it is
a ritual that has to be performed correctly *every single time*, forever.

**Preferred architecture — do not hard-parallel the cells at all:**

    external pack → boost to 5 V → the phone's own charge input

The onboard charger IC then manages the internal cell exactly as it would from a power bank. No
cell paralleling, no matched-voltage step, no shared-protection failure mode, and it can be
hot-plugged in the field. Costs a small boost module and some efficiency.

**If hard-paralleling onto VB+ anyway:** match within ~50 mV before connecting, every time; both
packs 1S and same chemistry, each with its own protection board; a polyfuse in the external leg;
and a connector rather than solder so the packs can be split for charging.

**A bigger pack charges slower, not harder.** Charge current is fixed by the phone's charger.

## Antenna

`ANA` → u.FL/IPEX MHF jack → pigtail → SMA bulkhead → 915 MHz whip. Keep the ANA-to-connector run
short (~5 mm), 50 Ω, grounded both sides. **Never key up with no antenna fitted** — it kills the PA.

⚠ COVEY runs this exact topology and its pigtail is currently a *ranked suspect* in the "heard zero
nodes several miles from home" investigation. Buy a decent pigtail, strain-relieve it where it
crosses the battery, and make sure the u.FL is properly **clicked**, not resting. If the woods
plate ever seems deaf, that joint is the first place to look — and unlike COVEY, this plate has a
known-good FPC antenna to swap against, which is a far better diagnostic than COVEY has ever had.

## Minimum extra parts

- 1 × u.FL / IPEX MHF jack
- 1 × u.FL → SMA bulkhead pigtail
- 1 × 915 MHz SMA whip
- 2 × 100 nF (RFM95W 3.3 V, GPS 5 V)
- 1 × 10 µF (RFM95W 3.3 V bulk)
- 1 × 47–100 µF (GPS 5 V bulk)
- 1 × polyfuse + connector (external battery leg)
- optional 10 kΩ pull-up on RESET

Note v2.0 gets away with a **single** capacitor because the module carries its own decoupling; the
above is deliberately more conservative than the reference.

## Firmware not yet written

Nothing reads the GPS today. A UART on GPIO 38/32 plus an NMEA parser would be new work, as would
anything that puts a position on the mesh. Out of scope until the plate exists.
