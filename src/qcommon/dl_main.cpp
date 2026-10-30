#include <universal/q_shared.h>
#include "dl_main.h"
#include "qcommon.h"
#include <universal/com_files.h>
#ifdef KISAK_MP
#include <ui_mp/ui_mp.h>
#endif

#include <curl/curl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace
{
    struct Download
    {
        CURL *easy;
        FILE *file;
        char path[MAX_OSPATH];
        bool added;
        bool running;
    } download{};

    bool curlInitialized;
    CURLM *downloadMulti;

    void DL_Release(bool removePartial)
    {
        if (download.added)
            curl_multi_remove_handle(downloadMulti, download.easy);
        if (download.easy)
            curl_easy_cleanup(download.easy);
        if (download.file)
            fclose(download.file);
        if (removePartial && download.path[0])
            remove(download.path);
        download = {};
    }

    int DL_ProgressValue(curl_off_t value)
    {
        if (value <= 0)
            return 0;
        const int maximum = (std::numeric_limits<int>::max)();
        return value >= maximum ? maximum : static_cast<int>(value);
    }

    int DL_Progress(void *, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t)
    {
#ifdef KISAK_MP
        legacyHacks.cl_downloadCount = DL_ProgressValue(now);
        if (total > 0)
            legacyHacks.cl_downloadSize = DL_ProgressValue(total);
#endif
        return 0;
    }

    size_t DL_Write(char *data, size_t size, size_t count, void *)
    {
        if (size && count > (std::numeric_limits<size_t>::max)() / size)
            return 0;
        const size_t bytes = size * count;
        return fwrite(data, 1, bytes, download.file);
    }

    bool DL_HttpsUrl(const char *remoteName, bool *valid)
    {
        CURLU *url = curl_url();
        *valid = false;
        if (!url)
            return false;
        char *scheme = nullptr;
        bool https = false;
        if (curl_url_set(url, CURLUPART_URL, remoteName, 0) == CURLUE_OK
            && curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK)
        {
            https = strcmp(scheme, "https") == 0;
            *valid = https || strcmp(scheme, "http") == 0;
        }
        curl_free(scheme);
        curl_url_cleanup(url);
        return https;
    }
}

void __cdecl DL_CancelDownload()
{
    DL_Release(true);
}

void __cdecl DL_InitDownload()
{
    if (!curlInitialized)
    {
        curlInitialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
        if (!curlInitialized)
            Com_Printf(CON_CHANNEL_CLIENT, "Could not initialize HTTP downloads\n");
    }
    if (curlInitialized && !downloadMulti)
    {
        // Keep the multi handle for the process lifetime. Destroying it while
        // an OS DNS lookup is active joins that worker and can freeze a frame
        // on cancel. Easy handles/files are still released on every transfer.
        downloadMulti = curl_multi_init();
        if (downloadMulti)
        {
            CURLMcode result = curl_multi_setopt(downloadMulti, CURLMOPT_MAXCONNECTS, 4L);
#if LIBCURL_VERSION_NUM >= 0x081400
            if (result == CURLM_OK)
                result = curl_multi_setopt(downloadMulti, CURLMOPT_RESOLVE_THREADS_MAX, 4L);
#endif
            if (result != CURLM_OK)
            {
                // No transfer or resolver has started, so teardown is safe.
                curl_multi_cleanup(downloadMulti);
                downloadMulti = nullptr;
            }
        }
    }
}

int __cdecl DL_BeginDownload(char *localName, char *remoteName)
{
    DL_CancelDownload();
    if (!localName || !*localName || strlen(localName) >= sizeof(download.path)
        || !remoteName || !*remoteName)
        return 0;

    DL_InitDownload();
    if (!curlInitialized || !downloadMulti)
        return 0;
    bool validUrl;
    const bool https = DL_HttpsUrl(remoteName, &validUrl);
    if (!validUrl)
        return 0;

    // FS_CreatePath temporarily mutates the name while creating directories.
    // Do that before allocating handles, since a filesystem error can unwind
    // through the engine's nonlocal error handler.
    if (FS_CreatePath(localName))
        return 0;
    download.file = fopen(localName, "wb");
    if (!download.file)
        return 0;
    strcpy(download.path, localName);
    download.easy = curl_easy_init();
    if (!download.easy)
    {
        DL_Release(true);
        return 0;
    }

    CURLcode result = CURLE_OK;
    const auto option = [&](CURLoption name, auto value)
    {
        if (result == CURLE_OK)
            result = curl_easy_setopt(download.easy, name, value);
    };
    option(CURLOPT_URL, remoteName);
    option(CURLOPT_PROTOCOLS_STR, "http,https");
    // An HTTPS download must not follow a redirect to cleartext HTTP.
    option(CURLOPT_REDIR_PROTOCOLS_STR, https ? "https" : "http,https");
    option(CURLOPT_FOLLOWLOCATION, 1L);
    option(CURLOPT_MAXREDIRS, 5L);
    option(CURLOPT_FAILONERROR, 1L);
    option(CURLOPT_NOSIGNAL, 1L);
    option(CURLOPT_CONNECTTIMEOUT_MS, 30000L);
    option(CURLOPT_LOW_SPEED_LIMIT, 1L);
    option(CURLOPT_LOW_SPEED_TIME, 30L);
    option(CURLOPT_USERAGENT, "KisakCOD/1.0");
    option(CURLOPT_WRITEFUNCTION, DL_Write);
    option(CURLOPT_XFERINFOFUNCTION, DL_Progress);
    option(CURLOPT_NOPROGRESS, 0L);
    option(CURLOPT_SSL_VERIFYPEER, 1L);
    option(CURLOPT_SSL_VERIFYHOST, 2L);
    option(CURLOPT_PROXY_SSL_VERIFYPEER, 1L);
    option(CURLOPT_PROXY_SSL_VERIFYHOST, 2L);

    // libcurl reads the standard proxy variables itself, except uppercase
    // HTTP_PROXY. Honor that spelling too for desktop/cloud game sessions.
    if (!getenv("http_proxy") && !https)
    {
        if (const char *proxy = getenv("HTTP_PROXY"))
            option(CURLOPT_PROXY, proxy);
    }
    // Windows uses Schannel's trust store; other builds use libcurl's default
    // CA store. Preserve explicitly configured trust bundles on either backend.
    const char *caBundle = getenv("CURL_CA_BUNDLE");
    if (!caBundle || !*caBundle)
        caBundle = getenv("SSL_CERT_FILE");
    if (caBundle && *caBundle)
    {
        option(CURLOPT_CAINFO, caBundle);
        option(CURLOPT_PROXY_CAINFO, caBundle);
    }

    if (result != CURLE_OK
        || curl_multi_add_handle(downloadMulti, download.easy) != CURLM_OK)
    {
        DL_Release(true);
        return 0;
    }
    download.added = true;
    download.running = true;
#ifdef KISAK_MP
    legacyHacks.cl_downloadCount = 0;
#endif
    return 1;
}

int __cdecl DL_DownloadLoop()
{
    if (!download.running)
        return DL_FAILED;

    int running = 0;
    const CURLMcode multiResult = curl_multi_perform(downloadMulti, &running);
    CURLcode result = CURLE_FAILED_INIT;
    bool finished = multiResult != CURLM_OK;
    int remaining;
    while (CURLMsg *message = curl_multi_info_read(downloadMulti, &remaining))
    {
        if (message->msg == CURLMSG_DONE)
        {
            result = message->data.result;
            finished = true;
        }
    }
    if (!finished)
        return DL_CONTINUE;

    long responseCode = 0;
    curl_easy_getinfo(download.easy, CURLINFO_RESPONSE_CODE, &responseCode);
    bool success = multiResult == CURLM_OK && result == CURLE_OK
        && responseCode >= 200 && responseCode < 300;
    // fclose reports deferred disk/full-filesystem errors. Completion is only
    // exposed after the file is closed so the client can safely rename it.
    if (fclose(download.file) != 0)
    {
        success = false;
        result = CURLE_WRITE_ERROR;
    }
    else if (result == CURLE_OK && !success)
        result = CURLE_HTTP_RETURNED_ERROR;
    download.file = nullptr;
    DL_Release(!success);
    if (!success)
        Com_Printf(CON_CHANNEL_CLIENT, "HTTP download failed (%s, HTTP %ld)\n",
            curl_easy_strerror(result), responseCode);
    return success ? DL_DONE : DL_FAILED;
}

bool __cdecl DL_InProgress()
{
    return download.running;
}

bool __cdecl DL_DLIsMotd()
{
    // There is no MOTD download entry point. Every current caller starts a
    // server-requested map/mod download and must follow that completion path.
    return false;
}
