#pragma once

// NDK curl+mbedTLS has no Android system CA store. Chat copies Mozilla
// cacert.pem and sets CURL_CA_BUNDLE; every curl handle must pass that
// path as CURLOPT_CAINFO. Desktop builds leave the native trust store.

#include <curl/curl.h>

#include <cstdlib>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace polymech::kbot {

inline void apply_android_curl_ca(CURL* curl)
{
    if (!curl)
        return;
#if defined(__ANDROID__)
    const char* ca = std::getenv("CURL_CA_BUNDLE");
    if (!ca || !ca[0])
        ca = std::getenv("SSL_CERT_FILE");
    if (ca && ca[0]) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
        __android_log_print(ANDROID_LOG_INFO, "TanitRealtime", "curl ca bundle %s", ca);
    } else {
        __android_log_print(ANDROID_LOG_ERROR, "TanitRealtime", "curl ca bundle unset");
    }
#else
    (void)curl;
#endif
}

} // namespace polymech::kbot
