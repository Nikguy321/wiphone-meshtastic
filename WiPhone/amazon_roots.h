/*
 * amazon_roots.h - the Amazon roots this firmware trusts, as string literals that concatenate
 * (isrg_roots.h's pattern): Menu > AI's web search pin (ai_net.cpp AI_TAVILY_ROOTS_PEM, D-176).
 *
 * WHY (measured 2026-10-19 from the Mac, openssl s_client -showcerts): api.tavily.com serves
 *   *.tavily.com (RSA-2048, 2026-06-01 .. 2026-12-15)
 *     <- Amazon RSA 2048 M01 (2022-08-23 .. 2030-08-23)
 *     <- Amazon Root CA 1, the CROSS certificate signed by Starfield Services Root Certificate
 *        Authority - G2 (2015-05-25 .. 2037-12-31; the server sends it as the third certificate)
 * and takes TLS 1.2 with ECDHE P-256 and ECDHE-RSA-AES128-GCM-SHA256 / -AES256-GCM-SHA384 (python
 * ssl, OpenSSL 3.5.8: maximum_version TLSv1_2, set_ecdh_curve('prime256v1'); the CBC suites and
 * every ECDSA suite are refused - an RSA leaf), so mbedTLS 2.16.7 and the curve wrap (tls_curves.h)
 * connect as they are. From the Mac: TCP ~76 ms, the TLS 1.2 handshake ~155 ms (5 runs).
 *
 *   Amazon Root CA 1 (self-signed)
 *                SHA-256 8E:CD:E6:88:4F:3D:87:B1:12:5B:A3:1A:C3:FC:B1:3D:70:16:DE:7F:57:CC:90:4F:E1:CB:97:C6:AE:98:19:6E
 *                RSA-2048, 2015-05-26 .. 2038-01-17. Its public key (SPKI SHA-256 fbe30180...0757d5a2) is the
 *                one in the cross certificate api.tavily.com sends, and M01's Authority Key Identifier
 *                (84:18:CC:85:...:0A:08) is its Subject Key Identifier: M01 anchors HERE.
 *   Starfield Services Root Certificate Authority - G2
 *                SHA-256 56:8D:69:05:A2:C8:87:08:A4:B3:02:51:90:ED:CF:ED:B1:97:4A:60:6A:13:C6:E5:29:0F:CB:2A:E6:3E:DA:B5
 *                RSA-2048, 2009-09-01 .. 2037-12-31. The cross certificate's issuer (its AKI
 *                9C:5F:00:DF:...:91:83 is this root's SKI): a server that sends the cross certificate
 *                anchors here even without Amazon Root CA 1 - the spec's backup pin.
 * Both exported from the macOS system keychain (security find-certificate -a -c ... -p
 * /System/Library/Keychains/SystemRootCertificates.keychain). Verified against the live chain with
 * python ssl (OpenSSL 3.5.8, a real handshake to api.tavily.com, check_hostname on, nothing sent):
 * Amazon Root CA 1 ALONE verifies, Starfield G2 ALONE verifies, the two together verify, and ISRG
 * Root X1 alone FAILS ("unable to get local issuer certificate"). tests/test_websearch.cpp decodes
 * each and checks these hashes; tests/check_ai_glue.py pins the set's define.
 * ⚠ The leaf expires 2026-12-15: pin the ROOTS, never the leaf; re-check the chain after it renews.
 * This build never checks certificate DATES (CONFIG_MBEDTLS_HAVE_TIME_DATE is unset - ai_net.cpp).
 * ⚠ NO OVERRIDE FILE (ai_net.cpp's reason: the key uploader has no password and can run on the
 * phone's own open hotspot). New roots are a firmware update.
 */
#ifndef AMAZON_ROOTS_H
#define AMAZON_ROOTS_H

// Amazon Root CA 1 - SHA-256 8E:CD:E6:...:98:19:6E
#define AMAZON_ROOT_CA1_PEM \
  "-----BEGIN CERTIFICATE-----\n" \
  "MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n" \
  "ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n" \
  "b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n" \
  "MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n" \
  "b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n" \
  "ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n" \
  "9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n" \
  "IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n" \
  "VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n" \
  "93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n" \
  "jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n" \
  "AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n" \
  "A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n" \
  "U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n" \
  "N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n" \
  "o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n" \
  "5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n" \
  "rqXRfboQnoZsG4q5WTP468SQvvG5\n" \
  "-----END CERTIFICATE-----\n"

// Starfield Services Root Certificate Authority - G2 - SHA-256 56:8D:69:...:3E:DA:B5
#define STARFIELD_SERVICES_ROOT_G2_PEM \
  "-----BEGIN CERTIFICATE-----\n" \
  "MIID7zCCAtegAwIBAgIBADANBgkqhkiG9w0BAQsFADCBmDELMAkGA1UEBhMCVVMx\n" \
  "EDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxJTAjBgNVBAoT\n" \
  "HFN0YXJmaWVsZCBUZWNobm9sb2dpZXMsIEluYy4xOzA5BgNVBAMTMlN0YXJmaWVs\n" \
  "ZCBTZXJ2aWNlcyBSb290IENlcnRpZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTA5\n" \
  "MDkwMTAwMDAwMFoXDTM3MTIzMTIzNTk1OVowgZgxCzAJBgNVBAYTAlVTMRAwDgYD\n" \
  "VQQIEwdBcml6b25hMRMwEQYDVQQHEwpTY290dHNkYWxlMSUwIwYDVQQKExxTdGFy\n" \
  "ZmllbGQgVGVjaG5vbG9naWVzLCBJbmMuMTswOQYDVQQDEzJTdGFyZmllbGQgU2Vy\n" \
  "dmljZXMgUm9vdCBDZXJ0aWZpY2F0ZSBBdXRob3JpdHkgLSBHMjCCASIwDQYJKoZI\n" \
  "hvcNAQEBBQADggEPADCCAQoCggEBANUMOsQq+U7i9b4Zl1+OiFOxHz/Lz58gE20p\n" \
  "OsgPfTz3a3Y4Y9k2YKibXlwAgLIvWX/2h/klQ4bnaRtSmpDhcePYLQ1Ob/bISdm2\n" \
  "8xpWriu2dBTrz/sm4xq6HZYuajtYlIlHVv8loJNwU4PahHQUw2eeBGg6345AWh1K\n" \
  "Ts9DkTvnVtYAcMtS7nt9rjrnvDH5RfbCYM8TWQIrgMw0R9+53pBlbQLPLJGmpufe\n" \
  "hRhJfGZOozptqbXuNC66DQO4M99H67FrjSXZm86B0UVGMpZwh94CDklDhbZsc7tk\n" \
  "6mFBrMnUVN+HL8cisibMn1lUaJ/8viovxFUcdUBgF4UCVTmLfwUCAwEAAaNCMEAw\n" \
  "DwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYEFJxfAN+q\n" \
  "AdcwKziIorhtSpzyEZGDMA0GCSqGSIb3DQEBCwUAA4IBAQBLNqaEd2ndOxmfZyMI\n" \
  "bw5hyf2E3F/YNoHN2BtBLZ9g3ccaaNnRbobhiCPPE95Dz+I0swSdHynVv/heyNXB\n" \
  "ve6SbzJ08pGCL72CQnqtKrcgfU28elUSwhXqvfdqlS5sdJ/PHLTyxQGjhdByPq1z\n" \
  "qwubdQxtRbeOlKyWN7Wg0I8VRw7j6IPdj/3vQQF3zCepYoUz8jcI73HPdwbeyBkd\n" \
  "iEDPfUYd/x7H4c7/I9vG+o1VTqkC50cRRj70/b17KSa7qWFiNyi2LSr2EIZkyXCn\n" \
  "0q23KXB56jzaYyWf/Wi3MOxw+3WKt21gZ7IeyLnp2KhvAotnDU0mV3HaIPzBSlCN\n" \
  "sSi6\n" \
  "-----END CERTIFICATE-----\n"

#endif // AMAZON_ROOTS_H
