/*
 * godaddy_roots.h - the GoDaddy root this firmware trusts, as a string literal that concatenates
 * (isrg_roots.h's pattern): Menu > AI's Chutes pin (ai_net.cpp AI_CHUTES_ROOTS_PEM, D-175).
 *
 * WHY (measured 2026-10-18 from the Mac, openssl s_client -showcerts): llm.chutes.ai serves
 *   *.chutes.ai (RSA-2048, 2025-11-30 .. 2026-11-30)
 *     <- Go Daddy Secure Certificate Authority - G2 (2011-05-03 .. 2031-05-03)
 *     <- Go Daddy Root Certificate Authority - G2 (self-signed; the server sends it too)
 * and takes TLS 1.2 with ECDHE P-256 and ECDHE-RSA-AES128-GCM-SHA256 (openssl -tls1_2 -groups P-256),
 * so mbedTLS 2.16.7 and the curve wrap (tls_curves.h) connect as they are.
 *
 *   Go Daddy Root Certificate Authority - G2
 *                SHA-256 45:14:0B:32:47:EB:9C:C8:C5:B4:F0:D7:B5:30:91:F7:32:92:08:9E:6E:5A:63:E2:74:9D:D3:AC:A9:19:8E:DA
 *                RSA-2048, 2009-09-01 .. 2037-12-31. Exported from the macOS system keychain
 *                (security find-certificate -c ... -p SystemRootCertificates.keychain) and
 *                byte-identical (DER) to the root llm.chutes.ai sent on 2026-10-18.
 *                tests/test_chutes.cpp decodes it and checks this hash.
 * ⚠ THE WILDCARD EXPIRES 2026-11-30, and CT logs already show *.chutes.ai from Let's Encrypt and
 * Google Trust Services: the CA may change at the renewal. So the Chutes pin is a ROOT SET - this
 * one, ISRG X1 + X2 + Root YR + Root YE (isrg_roots.h: the Gen-Y pair for a short Let's Encrypt
 * chain, review 2026-10-18) and GTS R1 + R4 (gts_roots.h). GoDaddy's next hierarchy, "GoDaddy
 * TLS Root CA - R1", is NOT in the macOS system keychain (2026-10-18), so it is not pinned: one that
 * no trust store vouches for is not second-sourced. Re-check the chain after 2026-11-30.
 * This build never checks certificate DATES (CONFIG_MBEDTLS_HAVE_TIME_DATE is unset - ai_net.cpp).
 * ⚠ NO OVERRIDE FILE (ai_net.cpp's reason: the key uploader has no password and can run on the
 * phone's own open hotspot). New roots are a firmware update.
 */
#ifndef GODADDY_ROOTS_H
#define GODADDY_ROOTS_H

// Go Daddy Root Certificate Authority - G2 - SHA-256 45:14:0B:...:19:8E:DA
#define GODADDY_ROOT_G2_PEM \
  "-----BEGIN CERTIFICATE-----\n" \
  "MIIDxTCCAq2gAwIBAgIBADANBgkqhkiG9w0BAQsFADCBgzELMAkGA1UEBhMCVVMx\n" \
  "EDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxGjAYBgNVBAoT\n" \
  "EUdvRGFkZHkuY29tLCBJbmMuMTEwLwYDVQQDEyhHbyBEYWRkeSBSb290IENlcnRp\n" \
  "ZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTA5MDkwMTAwMDAwMFoXDTM3MTIzMTIz\n" \
  "NTk1OVowgYMxCzAJBgNVBAYTAlVTMRAwDgYDVQQIEwdBcml6b25hMRMwEQYDVQQH\n" \
  "EwpTY290dHNkYWxlMRowGAYDVQQKExFHb0RhZGR5LmNvbSwgSW5jLjExMC8GA1UE\n" \
  "AxMoR28gRGFkZHkgUm9vdCBDZXJ0aWZpY2F0ZSBBdXRob3JpdHkgLSBHMjCCASIw\n" \
  "DQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAL9xYgjx+lk09xvJGKP3gElY6SKD\n" \
  "E6bFIEMBO4Tx5oVJnyfq9oQbTqC023CYxzIBsQU+B07u9PpPL1kwIuerGVZr4oAH\n" \
  "/PMWdYA5UXvl+TW2dE6pjYIT5LY/qQOD+qK+ihVqf94Lw7YZFAXK6sOoBJQ7Rnwy\n" \
  "DfMAZiLIjWltNowRGLfTshxgtDj6AozO091GB94KPutdfMh8+7ArU6SSYmlRJQVh\n" \
  "GkSBjCypQ5Yj36w6gZoOKcUcqeldHraenjAKOc7xiID7S13MMuyFYkMlNAJWJwGR\n" \
  "tDtwKj9useiciAF9n9T521NtYJ2/LOdYq7hfRvzOxBsDPAnrSTFcaUaz4EcCAwEA\n" \
  "AaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYE\n" \
  "FDqahQcQZyi27/a9BUFuIMGU2g/eMA0GCSqGSIb3DQEBCwUAA4IBAQCZ21151fmX\n" \
  "WWcDYfF+OwYxdS2hII5PZYe096acvNjpL9DbWu7PdIxztDhC2gV7+AJ1uP2lsdeu\n" \
  "9tfeE8tTEH6KRtGX+rcuKxGrkLAngPnon1rpN5+r5N9ss4UXnT3ZJE95kTXWXwTr\n" \
  "gIOrmgIttRD02JDHBHNA7XIloKmf7J6raBKZV8aPEjoJpL1E/QYVN8Gb5DKj7Tjo\n" \
  "2GTzLH4U/ALqn83/B2gX2yKQOC16jdFU8WnjXzPKej17CuPKf1855eJ1usV2GDPO\n" \
  "LPAvTK33sefOT6jEm0pUBsV/fdUID+Ic/n4XuKxe9tQWskMJDE32p2u0mYRlynqI\n" \
  "4uJEvlz36hz1\n" \
  "-----END CERTIFICATE-----\n"

#endif // GODADDY_ROOTS_H
