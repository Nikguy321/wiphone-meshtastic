#!/usr/bin/env python3
"""check_wifi_restore.py - nothing outside Networks.cpp may bring the WiFi station back by itself.

Every site that switched the radio off, or lent it to a hotspot, gives it back through
wifiRestoreStation() (WiPhone/Networks.cpp, deciding in WiPhone/wifi_policy.h). Until 0.9.79
each site asked its own subset of the owner's state - the Game Boy's exit the off switch alone,
the loop's retry neither the game nor the switch, the Settings toggles nothing at all (a bare
esp_wifi_start(), which restarts whatever mode the DRIVER last had) - and between them they put
WiFi back on behind "off" after every game on a phone with no saved network.

So this fails the suite on the spellings that start or rejoin the station, anywhere in
WiPhone/*.{cpp,h,ino} except Networks.cpp (which owns them):

    esp_wifi_start(      a bare driver start: the DRIVER's last mode, maybe AP
    esp_wifi_connect(    a join on whatever config the driver holds
    WiFi.reconnect(      the same
    WiFi.begin()         (no arguments) the same, and it starts the station first
    WiFi.mode(WIFI_STA)  also WIFI_MODE_STA / WIFI_AP_STA / WIFI_MODE_APSTA
    WiFi.enableSTA(true)

Comments and string/char literals are blanked first (a guard that trips on its own
documentation is one people learn to ignore - check_menu_keys.py found that out). The linker
wrappers in cpu_clock.cpp (`__wrap_esp_wifi_start`, `__real_esp_wifi_start`) are not calls
anyone makes and do not match: the pattern needs a word boundary before `esp_wifi_`.
A self-test runs first, so a pattern that has quietly stopped matching fails loudly.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent / "WiPhone"
OWNER = "Networks.cpp"

BANNED = [
    (re.compile(r"\besp_wifi_start\s*\("), "a bare esp_wifi_start()"),
    (re.compile(r"\besp_wifi_connect\s*\("), "a bare esp_wifi_connect()"),
    (re.compile(r"\bWiFi\s*\.\s*reconnect\s*\("), "WiFi.reconnect()"),
    (re.compile(r"\bWiFi\s*\.\s*begin\s*\(\s*\)"), "WiFi.begin() with no arguments"),
    (re.compile(r"\bWiFi\s*\.\s*mode\s*\(\s*WIFI_(?:MODE_)?(?:STA|AP_?STA)\s*\)"), "WiFi.mode(<a STA mode>)"),
    (re.compile(r"\bWiFi\s*\.\s*enableSTA\s*\(\s*true\s*\)"), "WiFi.enableSTA(true)"),
]


def strip_code(t):
    """Blank comments and string/char literals (raw strings too), keeping offsets and lines."""
    out, i, n = [], 0, len(t)

    def blank(s):
        return "".join(c if c == "\n" else " " for c in s)

    while i < n:
        if t.startswith("//", i):
            j = t.find("\n", i)
            j = n if j < 0 else j
            out.append(blank(t[i:j]))
            i = j
        elif t.startswith("/*", i):
            j = t.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(blank(t[i:j]))
            i = j
        elif t.startswith('R"', i) and (i == 0 or not (t[i - 1].isalnum() or t[i - 1] == "_")):
            m = re.match(r'R"([^()\\ ]{0,16})\(', t[i:])
            if m:
                end = t.find(")" + m.group(1) + '"', i + m.end())
                j = n if end < 0 else end + len(m.group(1)) + 2
                out.append(blank(t[i:j]))
                i = j
            else:
                out.append(t[i])
                i += 1
        elif t[i] in "\"'":
            q, j = t[i], i + 1
            while j < n and t[j] != q and t[j] != "\n":
                j += 2 if t[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(blank(t[i:j]))
            i = j
        else:
            out.append(t[i])
            i += 1
    return "".join(out)


def findings(text):
    code = strip_code(text)
    hits = []
    for rx, what in BANNED:
        for m in rx.finditer(code):
            hits.append((code.count("\n", 0, m.start()) + 1, what))
    return sorted(hits)


def selftest():
    bad = {
        "esp_wifi_start();": 1,
        "if (esp_wifi_start () != ESP_OK) {}": 1,
        "WiFi.reconnect();": 1,
        "WiFi.begin();": 1,
        "WiFi . begin ( );": 1,
        "WiFi.mode(WIFI_STA);": 1,
        "WiFi.mode(WIFI_MODE_STA);": 1,
        "WiFi.mode(WIFI_AP_STA);": 1,
        "WiFi.enableSTA(true);": 1,
        "esp_wifi_connect();": 1,
    }
    good = [
        "WiFi.begin(ssid, pwd);",               # the one join, in connectToWiFi (and allowed anyway)
        "WiFi.mode(WIFI_OFF);",
        "WiFi.mode(WIFI_AP);",
        "extern \"C\" esp_err_t __real_esp_wifi_start(void);",
        "esp_err_t __wrap_esp_wifi_start(void) {",
        "// WiFi.mode(WIFI_STA); WiFi.reconnect();",
        "/* a bare esp_wifi_start() restarts the driver's last mode */",
        "say(\"esp_wifi_start() failed\");",
        "log_e(\"WiFi.begin() refused\");",
        "char c = '\"'; // WiFi.begin()",
        "static const char P[] = R\"HTML(<p>WiFi.begin()</p>)HTML\";",
    ]
    ok = True
    for src, want in bad.items():
        if len(findings(src)) != want:
            print(f"  SELF-TEST FAIL: expected a finding in: {src}")
            ok = False
    for src in good:
        if findings(src):
            print(f"  SELF-TEST FAIL: false positive in: {src}")
            ok = False
    multi = "a();\n/* one\n two */\nWiFi.reconnect();\n"
    if findings(multi) != [(4, "WiFi.reconnect()")]:
        print("  SELF-TEST FAIL: line numbers drift across a block comment")
        ok = False
    return ok


def main():
    if not selftest():
        return 1
    files = sorted(list(ROOT.glob("*.cpp")) + list(ROOT.glob("*.h")) + list(ROOT.glob("*.ino")))
    bad = 0
    for f in files:
        if f.name == OWNER:
            continue
        for line, what in findings(f.read_text(errors="replace")):
            print(f"  {f.relative_to(ROOT.parent)}:{line}: {what} - give the radio back through "
                  f"wifiRestoreStation() (Networks.h), which asks the owner first")
            bad += 1
    if bad:
        print(f"  {bad} site(s) bring the WiFi station back without asking (see wifi_policy.h)")
        return 1
    print(f"  ok  no site outside {OWNER} starts or rejoins the station by itself "
          f"({len(files) - 1} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
