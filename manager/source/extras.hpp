/*
 * v0.7.5: "Get extras" - the optional tools, straight from their authors'
 * GitHub releases: Ultrahand Overlay, SaltyNX, ReverseNX-RT, and sys-clk or
 * Horizon OC.
 *
 * curl here uses libnx's TLS backend (the console's ssl service), so the
 * certificates are checked against the console's own trust store. The
 * latest release comes from api.github.com; its asset is downloaded to
 * config/switch-frame-tap/downloads and either moved into place (a single
 * .ovl) or unzipped over the SD card root - files under config/ that already
 * exist are kept (a tool's own settings), except its language files. Every
 * file is written as name.part and renamed, so a cut download never leaves a
 * half-written file in place.
 */
#pragma once
#include <switch.h>
#include <curl/curl.h>
#include <jansson.h>
#include <minizip/unzip.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <functional>

namespace extras {

    /* what: what is happening; frac: 0..1, or < 0 when unknown. False: stop. */
    using Progress = std::function<bool(const char *what, double frac)>;

    inline bool g_net = false;

    inline bool NetUp() {
        if (g_net) { return true; }
        if (R_FAILED(socketInitializeDefault())) { return false; }
        curl_global_init(CURL_GLOBAL_DEFAULT);
        g_net = true;
        return true;
    }

    inline void NetDown() {
        if (!g_net) { return; }
        curl_global_cleanup();
        socketExit();
        g_net = false;
    }

    namespace detail {
        struct Sink { std::string *body; FILE *file; const Progress *p; const char *what; };

        inline size_t Write(char *ptr, size_t sz, size_t n, void *ud) {
            Sink *s = static_cast<Sink *>(ud);
            const size_t len = sz * n;
            if (s->body != nullptr) { s->body->append(ptr, len); return len; }
            return std::fwrite(ptr, 1, len, s->file);
        }

        inline int Xfer(void *ud, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
            Sink *s = static_cast<Sink *>(ud);
            if (s->p == nullptr || !*s->p) { return 0; }
            return (*s->p)(s->what, total > 0 ? static_cast<double>(now) / static_cast<double>(total) : -1.0) ? 0 : 1;
        }
    }

    /* GET url into body (when non-null) or file */
    inline bool Get(const char *url, std::string *body, FILE *file, const Progress *p, const char *what, std::string *err) {
        CURL *c = curl_easy_init();
        if (c == nullptr) { *err = "could not start a download"; return false; }
        detail::Sink sink = { body, file, p, what };
        curl_slist *hdr = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "switch-frame-tap-manager");
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, detail::Write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, detail::Xfer);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &sink);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        const CURLcode rc = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(hdr);
        curl_easy_cleanup(c);
        if (rc == CURLE_OK) { return true; }
        if (rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_COULDNT_CONNECT || rc == CURLE_OPERATION_TIMEDOUT) {
            *err = "GitHub could not be reached - is the Switch online?";
        } else if (rc == CURLE_ABORTED_BY_CALLBACK) {
            *err = "cancelled";
        } else if (code != 0) {
            char b[96];
            std::snprintf(b, sizeof(b), "GitHub answered %ld", code);
            *err = b;
        } else {
            *err = curl_easy_strerror(rc);
        }
        return false;
    }

    struct Release { std::string tag, name, url; long long size = 0; };

    /* the newest release of repo, and its first asset `match` accepts */
    inline bool Latest(const char *repo, bool (*match)(const char *), Release *out, const Progress *p, std::string *err) {
        char url[160];
        std::snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", repo);
        std::string body;
        if (!Get(url, &body, nullptr, p, "Asking GitHub for the latest version", err)) { return false; }
        json_error_t je;
        json_t *root = json_loads(body.c_str(), 0, &je);
        if (root == nullptr) { *err = "GitHub's answer could not be read"; return false; }
        bool found = false;
        if (const char *tag = json_string_value(json_object_get(root, "tag_name"))) { out->tag = tag; }
        json_t *assets = json_object_get(root, "assets");
        for (size_t i = 0; i < json_array_size(assets) && !found; ++i) {
            json_t *a = json_array_get(assets, i);
            const char *name = json_string_value(json_object_get(a, "name"));
            const char *dl = json_string_value(json_object_get(a, "browser_download_url"));
            if (name != nullptr && dl != nullptr && match(name)) {
                out->name = name;
                out->url = dl;
                out->size = static_cast<long long>(json_integer_value(json_object_get(a, "size")));
                found = true;
            }
        }
        json_decref(root);
        if (!found) { *err = "its latest release has no file this app knows how to install"; }
        return found;
    }

    inline bool Exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

    /* every directory on the way to path's file */
    inline void MakeParents(const std::string &path) {
        for (size_t i = std::strlen("sdmc:/"); i < path.size(); ++i) {
            if (path[i] == '/') { mkdir(path.substr(0, i).c_str(), 0777); }
        }
    }

    /* a file of the tool's own settings, kept when it already exists */
    inline bool Keep(const char *rel) {
        return std::strncmp(rel, "config/", 7) == 0 && std::strstr(rel, "/lang/") == nullptr;
    }

    inline bool Replace(const std::string &part, const std::string &dest) {
        std::remove(dest.c_str());
        return std::rename(part.c_str(), dest.c_str()) == 0;
    }

    /* unzip over the SD card root; files at the zip's top level (a README)
     * are skipped */
    inline bool Unzip(const char *zip, const Progress *p, int *written, int *kept, std::string *err) {
        unzFile z = unzOpen(zip);
        if (z == nullptr) { *err = "the download is not a zip file"; return false; }
        unz_global_info gi = {};
        unzGetGlobalInfo(z, &gi);
        static char buf[0x10000];
        int idx = 0;
        bool ok = true;
        for (int r = unzGoToFirstFile(z); r == UNZ_OK && ok; r = unzGoToNextFile(z), ++idx) {
            char name[512];
            unz_file_info fi = {};
            if (unzGetCurrentFileInfo(z, &fi, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK) { ok = false; break; }
            if (p != nullptr && *p && !(*p)("Unpacking", gi.number_entry ? static_cast<double>(idx) / gi.number_entry : -1.0)) { *err = "cancelled"; ok = false; break; }
            const size_t nl = std::strlen(name);
            if (nl == 0 || std::strstr(name, "..") != nullptr) { continue; }
            const std::string dest = std::string("sdmc:/") + name;
            if (name[nl - 1] == '/') { MakeParents(dest); continue; }
            if (std::strchr(name, '/') == nullptr) { continue; }
            if (Keep(name) && Exists(dest.c_str())) { ++*kept; continue; }
            MakeParents(dest);
            if (unzOpenCurrentFile(z) != UNZ_OK) { *err = std::string("could not read ") + name; ok = false; break; }
            const std::string part = dest + ".part";
            FILE *f = std::fopen(part.c_str(), "wb");
            if (f == nullptr) { unzCloseCurrentFile(z); *err = std::string("could not write ") + name; ok = false; break; }
            int n;
            while ((n = unzReadCurrentFile(z, buf, sizeof(buf))) > 0) {
                if (std::fwrite(buf, 1, static_cast<size_t>(n), f) != static_cast<size_t>(n)) { n = -1; break; }
            }
            std::fclose(f);
            unzCloseCurrentFile(z);
            if (n < 0 || !Replace(part, dest)) { std::remove(part.c_str()); *err = std::string("could not write ") + name; ok = false; break; }
            ++*written;
        }
        unzClose(z);
        return ok;
    }

    /* ---- the tools ------------------------------------------------------ */

    struct Tool {
        const char *name;
        const char *repo;
        bool (*match)(const char *asset);
        const char *single_dest;      /* non-null: the asset is this one file, not a zip */
        bool sysmodule;               /* runs at boot: a restart starts it */
    };

    inline bool EndsWith(const char *s, const char *suf) {
        const size_t a = std::strlen(s), b = std::strlen(suf);
        return a >= b && std::strcmp(s + a - b, suf) == 0;
    }

    inline const Tool Ultrahand  = { "Ultrahand Overlay", "ppkantorski/Ultrahand-Overlay", [](const char *n) { return std::strcmp(n, "sdout.zip") == 0; }, nullptr, true };
    inline const Tool SaltyNX    = { "SaltyNX", "masagrator/SaltyNX", [](const char *n) { return std::strcmp(n, "SaltyNX.zip") == 0; }, nullptr, true };
    inline const Tool ReverseNX  = { "ReverseNX-RT", "masagrator/ReverseNX-RT", [](const char *n) { return EndsWith(n, ".ovl"); }, "sdmc:/switch/.overlays/ReverseNX-RT-ovl.ovl", false };
    inline const Tool SysClk     = { "sys-clk", "retronx-team/sys-clk", [](const char *n) { return std::strncmp(n, "sys-clk", 7) == 0 && EndsWith(n, ".zip"); }, nullptr, true };
    inline const Tool HorizonOC  = { "Horizon OC", "Horizon-OC/Horizon-OC", [](const char *n) { return std::strcmp(n, "dist.zip") == 0; }, nullptr, true };

    /* ---- what is installed ---------------------------------------------- */

    inline bool HasUltrahand() { return Exists("sdmc:/switch/.overlays/ovlmenu.ovl") && Exists("sdmc:/atmosphere/contents/420000000007E51A/exefs.nsp"); }
    inline bool HasSaltyNX()   { return Exists("sdmc:/atmosphere/contents/0000000000534C56/exefs.nsp"); }
    inline bool HasReverseNX() { return Exists("sdmc:/switch/.overlays/ReverseNX-RT-ovl.ovl"); }
    /* sys-clk and Horizon OC are the same sysmodule (00FF0000636C6BFF):
     * Horizon OC's kip or overlay tells them apart */
    inline bool HasHorizonOC() { return Exists("sdmc:/atmosphere/kips/hoc.kip") || Exists("sdmc:/switch/.overlays/horizon-oc-overlay.ovl"); }
    inline bool HasSysClk()    { return !HasHorizonOC() && (Exists("sdmc:/atmosphere/contents/00FF0000636C6BFF/exefs.nsp") || Exists("sdmc:/switch/.overlays/sys-clk-overlay.ovl")); }

    /* Hekate boots Atmosphere with only the kips its entry names: Horizon OC
     * needs "kip1=atmosphere/kips/hoc.kip", or the whole kips folder. Fusee
     * needs nothing.
     * True when hekate_ipl.ini exists and names neither. */
    inline bool HekateMissesHocKip() {
        FILE *f = std::fopen("sdmc:/bootloader/hekate_ipl.ini", "r");
        if (f == nullptr) { return false; }
        char line[256];
        bool any_entry = false, ok = false;
        while (std::fgets(line, sizeof(line), f)) {
            if (std::strncmp(line, "fss0=", 5) == 0 || std::strncmp(line, "pkg3=", 5) == 0) { any_entry = true; }
            if (std::strstr(line, "kip1=atmosphere/kips/*") != nullptr || std::strstr(line, "kip1=atmosphere/kips/hoc.kip") != nullptr) { ok = true; }
        }
        std::fclose(f);
        return any_entry && !ok;
    }

    /* the latest release of t, downloaded and in place; *done says what */
    inline bool Install(const Tool &t, const Progress &p, std::string *done, std::string *err) {
        if (!NetUp()) { *err = "the network could not be started"; return false; }
        Release rel;
        if (!Latest(t.repo, t.match, &rel, &p, err)) { return false; }
        mkdir("sdmc:/config", 0777);
        mkdir("sdmc:/config/switch-frame-tap", 0777);
        mkdir("sdmc:/config/switch-frame-tap/downloads", 0777);
        const std::string tmp = std::string("sdmc:/config/switch-frame-tap/downloads/") + rel.name + ".part";
        FILE *f = std::fopen(tmp.c_str(), "wb");
        if (f == nullptr) { *err = "could not write to the SD card"; return false; }
        char what[96];
        std::snprintf(what, sizeof(what), "Downloading %s %s", t.name, rel.tag.c_str());
        const bool got = Get(rel.url.c_str(), nullptr, f, &p, what, err);
        std::fclose(f);
        if (!got) { std::remove(tmp.c_str()); return false; }
        bool ok;
        int written = 0, kept = 0;
        if (t.single_dest != nullptr) {
            MakeParents(t.single_dest);
            ok = Replace(tmp, t.single_dest);
            written = ok ? 1 : 0;
            if (!ok) { *err = "could not move the download into place"; }
        } else {
            ok = Unzip(tmp.c_str(), &p, &written, &kept, err);
            std::remove(tmp.c_str());
        }
        if (ok) {
            char b[160];
            std::snprintf(b, sizeof(b), "%s %s installed (%d file%s%s)%s", t.name, rel.tag.c_str(), written, written == 1 ? "" : "s",
                          kept ? ", your settings kept" : "", t.sysmodule ? " - restart the console to start it" : "");
            *done = b;
        }
        return ok;
    }

}
