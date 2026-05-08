#include "FtpsTransport.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <sstream>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/hex.hpp>
#include <boost/endian/conversion.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/detail/md5.hpp>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <miniz.h>

// Bambu's closed-source BambuTunnel header — only used here for the
// static_asserts below; PrinterFileSystem itself routes through the
// transport interface.
#define BAMBU_DYNAMIC
#include "BambuTunnel.h"

// If Bambu ever renumbers these, the FTPS transport's return-code mapping
// would silently desync from PrinterFileSystem's `Bambu_*` comparisons.
// Break the build instead of corrupting download/list flows.
static_assert((int) IPrinterFileTransport::OK           == (int) Bambu_success,      "OK == Bambu_success");
static_assert((int) IPrinterFileTransport::STREAM_END   == (int) Bambu_stream_end,   "STREAM_END == Bambu_stream_end");
static_assert((int) IPrinterFileTransport::WOULD_BLOCK  == (int) Bambu_would_block,  "WOULD_BLOCK == Bambu_would_block");
static_assert((int) IPrinterFileTransport::BUFFER_LIMIT == (int) Bambu_buffer_limit, "BUFFER_LIMIT == Bambu_buffer_limit");

using nlohmann::json;

// Bambu printer LAN-only mode FTPS layout (X1 / X1C / P1 series). The printer
// exposes implicit FTPS on port 990; the data root holds three folders we
// browse: /timelapse/, /video/, /model/. P1P/P1S have an extra "sdcard/"
// prefix which we'll need to thread through later (see PROGRESS notes); for
// now hardcode X1C-style layout.

namespace {

constexpr int CTRL_TYPE = 0x3001; // matches PrinterFileSystem

enum CmdType {
    LIST_INFO             = 0x0001,
    SUB_FILE              = 0x0002,
    FILE_DEL              = 0x0003,
    FILE_DOWNLOAD         = 0x0004,
    FILE_UPLOAD           = 0x0005,
    REQUEST_MEDIA_ABILITY = 0x0007,
    TASK_CANCEL           = 0x1000,
};

enum Result {
    SUCCESS               = 0,
    CONTINUE              = 1,
    ERROR_PIPE            = 3,
    FILE_NO_EXIST         = 10,
    FILE_OPEN_ERR         = 13,
    STORAGE_UNAVAILABLE   = 17,
};

// Unix-style `LIST` line parser. Bambu's embedded ftpd doesn't support MLSD;
// it answers `LIST` with `ls -l`-shaped output:
//
//   -rw-r--r--   1 root  root   1234567 Apr 25 12:34 filename.3mf
//   -rw-r--r--   1 root  root   1234567 Apr 25  2024 olderfile.3mf
//   drwxr-xr-x   2 root  root      4096 Apr 25 12:34 subdir
//
// Returns false for directory entries or unparseable rows.
bool parse_list_line(std::string const& raw_line,
                     std::string&       name,
                     std::uint64_t&     size,
                     std::time_t&       mtime)
{
    std::string line = raw_line;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    if (line.empty() || line[0] == 'd' || line[0] == 'l') return false;
    if (line[0] != '-') return false;

    // Tokenize on whitespace, but the filename (token 8) may contain spaces, so
    // capture the offset of the 9th token's start instead of split-and-rejoin.
    std::vector<std::string> toks;
    std::size_t              i = 0;
    std::size_t              name_start = std::string::npos;
    while (i < line.size() && toks.size() < 8) {
        while (i < line.size() && std::isspace((unsigned char) line[i])) ++i;
        std::size_t start = i;
        while (i < line.size() && !std::isspace((unsigned char) line[i])) ++i;
        if (start == i) break;
        toks.emplace_back(line.substr(start, i - start));
    }
    while (i < line.size() && std::isspace((unsigned char) line[i])) ++i;
    name_start = i;
    if (toks.size() < 8 || name_start >= line.size()) return false;

    name = line.substr(name_start);
    // Reject control characters in filenames. A filename containing \r or \n
    // would inject a second FTP control command if echoed into a `DELE`
    // CURLOPT_QUOTE call. Bambu's filesystem doesn't allow these in
    // practice, but the printer is a trust boundary we don't fully control.
    for (char c : name) {
        if (c == '\r' || c == '\n' || c == '\0')
            return false;
    }
    size = std::strtoull(toks[4].c_str(), nullptr, 10);

    // Date: "Mon DD HH:MM" (current year) or "Mon DD YYYY".
    static char const* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
    int   mon = -1;
    for (int m = 0; m < 12; ++m) if (toks[5] == months[m]) { mon = m; break; }
    int day  = std::atoi(toks[6].c_str());
    int hour = 0, minute = 0, year = 0;
    bool has_time = toks[7].find(':') != std::string::npos;
    if (has_time) {
        hour   = std::atoi(toks[7].c_str());
        auto c = toks[7].find(':');
        if (c != std::string::npos) minute = std::atoi(toks[7].c_str() + c + 1);
        // Standard `ls -l` rule: when the year is omitted, the file is within
        // the last ~6 months. If (mon,day) lies in the future relative to
        // today, it actually refers to the previous year. Compare in local
        // time because Bambu's busybox `ls` prints the printer's local time,
        // not UTC.
        std::time_t now = std::time(nullptr);
        std::tm*    nt  = std::localtime(&now);
        if (nt) {
            year = nt->tm_year + 1900;
            if (mon > nt->tm_mon || (mon == nt->tm_mon && day > nt->tm_mday))
                year -= 1;
        } else {
            year = 1970;
        }
    } else {
        year = std::atoi(toks[7].c_str());
    }

    mtime = 0;
    if (mon >= 0 && day > 0 && year > 0) {
        std::tm tm{};
        tm.tm_year  = year - 1900;
        tm.tm_mon   = mon;
        tm.tm_mday  = day;
        tm.tm_hour  = hour;
        tm.tm_min   = minute;
        tm.tm_isdst = -1;
        // mktime treats the broken-down time as host-local. Bambu's `ls`
        // emits printer-local time; we don't know the printer's TZ, so
        // approximate by treating it as the slicer host's TZ. That matches
        // when the user is colocated with their printer (the common case);
        // worst case the displayed mtime is off by hours, never by days.
        mtime = std::mktime(&tm);
    }
    return true;
}

// MLSD response line (kept for firmwares that support it):
//   type=file;size=NN;modify=YYYYMMDDHHMMSS;UNIX.mode=...; filename
bool parse_mlsd_line(std::string const& line,
                     std::string&       name,
                     std::uint64_t&     size,
                     std::time_t&       mtime)
{
    auto sp = line.find(' ');
    if (sp == std::string::npos) return false;

    std::string facts = line.substr(0, sp);
    name              = line.substr(sp + 1);
    while (!name.empty() && (name.back() == '\r' || name.back() == '\n'))
        name.pop_back();
    if (name == "." || name == "..") return false;

    bool is_file = false;
    size  = 0;
    mtime = 0;

    std::string tok;
    std::stringstream ss(facts);
    while (std::getline(ss, tok, ';')) {
        auto eq = tok.find('=');
        if (eq == std::string::npos) continue;
        std::string k = tok.substr(0, eq);
        std::string v = tok.substr(eq + 1);
        std::transform(k.begin(), k.end(), k.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (k == "type") {
            is_file = (v == "file");
        } else if (k == "size") {
            size = std::strtoull(v.c_str(), nullptr, 10);
        } else if (k == "modify") {
            // YYYYMMDDHHMMSS in UTC
            if (v.size() >= 14) {
                std::tm tm{};
                tm.tm_year = std::atoi(v.substr(0,  4).c_str()) - 1900;
                tm.tm_mon  = std::atoi(v.substr(4,  2).c_str()) - 1;
                tm.tm_mday = std::atoi(v.substr(6,  2).c_str());
                tm.tm_hour = std::atoi(v.substr(8,  2).c_str());
                tm.tm_min  = std::atoi(v.substr(10, 2).c_str());
                tm.tm_sec  = std::atoi(v.substr(12, 2).c_str());
#ifdef _WIN32
                mtime = _mkgmtime(&tm);
#else
                mtime = timegm(&tm);
#endif
            }
        }
    }
    return is_file;
}

size_t curl_write_to_string(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t curl_write_to_sink(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* sink = static_cast<std::function<bool(unsigned char const*, std::size_t)>*>(userdata);
    std::size_t n = size * nmemb;
    if (!(*sink)(reinterpret_cast<unsigned char const*>(ptr), n))
        return 0; // abort transfer
    return n;
}

// Progress callback: returning non-zero aborts the transfer. Without this,
// libcurl only re-checks the write callback when bytes arrive, so a stalled
// download could keep Close() blocked for the whole CURLOPT_LOW_SPEED_TIME
// window. The progress callback fires on a small internal timer regardless
// of data flow, so m_quit is honored within ~1s of being set.
int curl_abort_on_quit(void* clientp,
                       curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                       curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
{
    auto* quit = static_cast<std::atomic<bool>*>(clientp);
    return (quit && quit->load()) ? 1 : 0;
}

} // namespace

FtpsTransport::FtpsTransport(std::string url) : m_url(std::move(url))
{
    // Parse ftps://user:pass@host:port/ via libcurl's URL parser.
    CURLU* u = curl_url();
    if (u && curl_url_set(u, CURLUPART_URL, m_url.c_str(), 0) == CURLUE_OK) {
        char* host = nullptr;
        char* user = nullptr;
        char* pass = nullptr;
        if (curl_url_get(u, CURLUPART_HOST, &host, 0) == CURLUE_OK && host) {
            m_host = host;
            curl_free(host);
        }
        if (curl_url_get(u, CURLUPART_USER, &user, CURLU_URLDECODE) == CURLUE_OK && user) {
            m_user = user;
            curl_free(user);
        }
        if (curl_url_get(u, CURLUPART_PASSWORD, &pass, CURLU_URLDECODE) == CURLUE_OK && pass) {
            m_pass = pass;
            curl_free(pass);
        }
    }
    if (u) curl_url_cleanup(u);
}

FtpsTransport::~FtpsTransport() { FtpsTransport::Close(); }

int FtpsTransport::Open()
{
    if (m_open) return 0;
    if (m_host.empty() || m_pass.empty()) {
        BOOST_LOG_TRIVIAL(error) << "FtpsTransport: bad URL, missing host or password";
        return -1;
    }

    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    // Probe with a directory listing of "/" to confirm credentials + TLS.
    std::string url = "ftps://" + m_host + ":990/";
    std::string sink;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, m_user.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, m_pass.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL, (long) CURLUSESSL_ALL);
    // TLS verification disabled because Bambu printers ship a self-signed
    // certificate whose CN doesn't match the LAN IP. Safe here only because
    // FtpsTransport is constructed exclusively from MediaFilePanel's
    // ftps://bblp:<code>@<lan-ip>:990/ URL — never on attacker-controlled
    // input. Don't lift these settings into a generic helper.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FTP_USE_EPSV, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_DIRLISTONLY, 1L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        BOOST_LOG_TRIVIAL(error) << "FtpsTransport::Open ftps://" << m_host
                                 << ":990 failed: " << curl_easy_strerror(rc);
        curl_easy_cleanup(curl);
        return -1;
    }

    // Keep the handle for reuse on the worker thread.
    m_curl = curl;
    m_open = true;
    m_quit = false;
    m_worker = std::thread([this] { WorkerLoop(); });
    BOOST_LOG_TRIVIAL(info) << "FtpsTransport::Open ftps://" << m_host << ":990 OK";
    return 0;
}

int FtpsTransport::StartStream(int /*ctrl_type*/) { return 0; }

int FtpsTransport::SendMessage(int /*ctrl_type*/, char const* data, int len)
{
    if (!m_open) return -1;
    PendingRequest pr;
    pr.body.assign(data, data + len);
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_requests.push_back(std::move(pr));
    }
    m_cond.notify_all();
    return 0;
}

int FtpsTransport::ReadSample(Sample& out)
{
    bool drained = false;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (!m_pending_samples.empty()) {
            m_current_sample = std::move(m_pending_samples.front());
            m_pending_samples.pop_front();
            out.buffer = m_current_sample.data();
            out.size   = m_current_sample.size();
            drained    = true;
        }
    }
    if (drained) {
        // Wake any worker thread blocked in Emit() because the queue was full.
        m_cond.notify_all();
        return OK;
    }
    // Closed transport with nothing left to deliver: tell the recv thread
    // to bail out via Reconnect rather than spinning on WOULD_BLOCK.
    if (!m_open) return STREAM_END;
    return WOULD_BLOCK;
}

void FtpsTransport::Close()
{
    if (!m_open && !m_worker.joinable()) return;
    m_quit = true;
    m_open = false;
    m_cond.notify_all();
    if (m_worker.joinable())
        m_worker.join();
    if (m_curl) {
        curl_easy_cleanup(static_cast<CURL*>(m_curl));
        m_curl = nullptr;
    }
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_requests.clear();
        m_pending_samples.clear();
        m_current_sample.clear();
        m_zip_cache.clear();
    }
}

void FtpsTransport::SetLogger(LogFn fn, void* context)
{
    m_log_fn  = fn;
    m_log_ctx = context;
}

void FtpsTransport::FreeLogMsg(char const* /*msg*/) {}

void FtpsTransport::WorkerLoop()
{
    while (!m_quit) {
        PendingRequest pr;
        {
            std::unique_lock<std::mutex> l(m_mutex);
            m_cond.wait(l, [this] { return m_quit || !m_requests.empty(); });
            if (m_quit) return;
            pr = std::move(m_requests.front());
            m_requests.pop_front();
        }
        // Handlers parse JSON and call .get<T>() / iterate arrays without
        // exhaustive shape checks. A malformed reply mustn't terminate the
        // process — drop the request and keep the worker alive instead.
        try {
            Dispatch(pr.body);
        } catch (std::exception const& e) {
            BOOST_LOG_TRIVIAL(warning) << "FtpsTransport::Dispatch threw: " << e.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "FtpsTransport::Dispatch threw unknown exception";
        }
    }
}

void FtpsTransport::Dispatch(std::string const& body)
{
    // body is "<json>\n\n<optional binary param>". For the commands we handle
    // here, only the json head is relevant.
    std::string head = body;
    auto split = body.find("\n\n");
    if (split != std::string::npos)
        head = body.substr(0, split);

    json root;
    try {
        root = json::parse(head);
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "FtpsTransport::Dispatch invalid JSON: " << head;
        return;
    }

    int cmd = root.value("cmdtype", 0);
    int seq = root.value("sequence", 0);
    json req = root.value("req", json::object());

    switch (cmd) {
    case LIST_INFO:             HandleListInfo(seq, req); break;
    case FILE_DEL:              HandleFileDel(seq, req); break;
    case FILE_DOWNLOAD:         HandleFileDownload(seq, req); break;
    case REQUEST_MEDIA_ABILITY: HandleMediaAbility(seq, req); break;
    case SUB_FILE:              HandleSubFile(seq, req); break;
    case TASK_CANCEL:           HandleTaskCancel(seq, req); break;
    case FILE_UPLOAD:           HandleUnsupported(seq, cmd); break;
    default:                    HandleUnsupported(seq, cmd); break;
    }
}

std::string FtpsTransport::FolderForType(std::string const& type)
{
    // X1C LAN-only layout: models at FTP root, no /video/ folder, recordings
    // all under /timelapse/. See memory: project_x1c_ftps_layout.
    if (type == "model")     return "/";
    if (type == "timelapse") return "/timelapse/";
    if (type == "video")     return "/timelapse/";
    return "/";
}

std::string FtpsTransport::EncodeFtpPath(std::string const& path)
{
    // Percent-encode any byte outside RFC 3986's unreserved set. '/' is a
    // path separator and is left intact. Bambu filenames frequently contain
    // characters libcurl's URL parser would otherwise mangle (literal '%',
    // ';', non-ASCII bytes from unicode model titles).
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (unsigned char c : path) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                       || (c >= '0' && c <= '9')
                       || c == '-' || c == '_' || c == '.' || c == '~'
                       || c == '/';
        if (unreserved) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

void FtpsTransport::Emit(std::string const& json_line, unsigned char const* data, std::size_t data_len)
{
    std::vector<unsigned char> buf;
    buf.reserve(json_line.size() + 2 + data_len);
    buf.insert(buf.end(), json_line.begin(), json_line.end());
    buf.push_back('\n');
    buf.push_back('\n');
    if (data && data_len)
        buf.insert(buf.end(), data, data + data_len);
    {
        // Block the worker until the consumer drains the queue or the
        // transport is being torn down. Without this a multi-MB SUB_FILE
        // batch can buffer the whole .gcode.3mf as 256 KB chunks ahead of
        // the recv thread's drain.
        std::unique_lock<std::mutex> l(m_mutex);
        m_cond.wait(l, [this] {
            return m_quit || m_pending_samples.size() < kMaxPendingSamples;
        });
        if (m_quit) return;
        m_pending_samples.push_back(std::move(buf));
    }
    m_cond.notify_all();
}

void FtpsTransport::EmitError(int seq, int cmdtype, int result)
{
    json r;
    r["cmdtype"]  = cmdtype;
    r["sequence"] = seq;
    r["result"]   = result;
    r["reply"]    = json::object();
    Emit(r.dump());
}

void FtpsTransport::HandleListInfo(int seq, json const& req)
{
    std::string type = req.value("type", "model");
    std::string folder = FolderForType(type);

    std::string raw;
    int rc = FtpsMlsd(folder, raw);
    if (rc != 0) {
        EmitError(seq, LIST_INFO, STORAGE_UNAVAILABLE);
        return;
    }

    json files = json::array();
    std::stringstream ss(raw);
    std::string line;
    while (std::getline(ss, line)) {
        std::string   name;
        std::uint64_t size  = 0;
        std::time_t   mtime = 0;
        if (!parse_list_line(line, name, size, mtime) &&
            !parse_mlsd_line(line, name, size, mtime))
            continue;
        // The model "folder" is the FTP root, which may contain unrelated
        // files; filter to .3mf to match what Bambu's API would have returned.
        if (type == "model") {
            auto dot = name.rfind('.');
            if (dot == std::string::npos ||
                !boost::iequals(name.substr(dot), std::string(".3mf")))
                continue;
        }
        json f;
        f["name"] = name;
        f["size"] = size;
        f["time"] = static_cast<std::int64_t>(mtime);
        // Always set a path. Bambu's wire format allows timelapse to have an
        // empty path, but doing so routes thumbnail requests through the
        // "OldThumbnail" path (req.files=[names]) which we don't synthesize
        // for. With a path set, the receiver uses VideoThumbnail
        // (req.paths=[<path>#thumbnail]) which we serve from
        // /timelapse/thumbnail/<basename>.jpg.
        f["path"] = folder + name;
        files.push_back(std::move(f));
    }

    json r;
    r["cmdtype"]  = LIST_INFO;
    r["sequence"] = seq;
    r["result"]   = SUCCESS;
    json reply;
    reply["file_lists"] = std::move(files);
    r["reply"]          = std::move(reply);
    Emit(r.dump());
}

void FtpsTransport::HandleFileDel(int seq, json const& req)
{
    std::vector<std::string> targets;
    if (req.contains("paths")) {
        for (auto& p : req["paths"]) targets.push_back(p.get<std::string>());
    } else if (req.contains("delete")) {
        // timelapse: names only — no folder context, assume /timelapse/
        for (auto& n : req["delete"]) targets.push_back("/timelapse/" + n.get<std::string>());
    }
    // Sticky failure: surface the first non-success error rather than
    // overwriting it with a later success. ERROR_PIPE wins over
    // FILE_NO_EXIST because a transport failure on any item is more
    // diagnostic than a missing-file report.
    int last_err = 0;
    for (auto& path : targets) {
        int rc = FtpsDelete(path);
        if (rc == 0) continue;
        int mapped = (rc == -2) ? FILE_NO_EXIST : ERROR_PIPE;
        if (last_err != ERROR_PIPE) last_err = mapped;
    }
    EmitError(seq, FILE_DEL, last_err);
}

void FtpsTransport::HandleMediaAbility(int seq, json const& /*req*/)
{
    json r;
    r["cmdtype"]  = REQUEST_MEDIA_ABILITY;
    r["sequence"] = seq;
    r["result"]   = SUCCESS;
    json reply;
    reply["storage"] = json::array({"model", "timelapse", "video"});
    r["reply"]       = std::move(reply);
    Emit(r.dump());
}

void FtpsTransport::HandleFileDownload(int seq, json const& req)
{
    std::string path;
    if (req.contains("path"))      path = req["path"].get<std::string>();
    else if (req.contains("file")) path = "/timelapse/" + req["file"].get<std::string>();
    else { EmitError(seq, FILE_DOWNLOAD, FILE_NO_EXIST); return; }

    // RAM-download paths (mem:/) aren't supported in LAN-only mode.
    if (path.rfind("mem:/", 0) == 0) {
        EmitError(seq, FILE_DOWNLOAD, FILE_NO_EXIST);
        return;
    }

    std::int64_t total = 0;
    std::int64_t offset = 0;
    constexpr std::size_t CHUNK = 64 * 1024;
    std::vector<unsigned char> buffer;
    buffer.reserve(CHUNK);

    auto sink = std::function<bool(unsigned char const*, std::size_t)>(
        [&](unsigned char const* data, std::size_t n) -> bool {
            buffer.insert(buffer.end(), data, data + n);
            // Emit while strictly greater than CHUNK so we always retain at
            // least one byte for the final SUCCESS sample. Otherwise the
            // receiver could see prog.size == prog.total on a CONTINUE chunk
            // (which has no md5) and finalize the download before our SUCCESS
            // reaches it. See PROGRESS.md for the chunk/seq protocol.
            while (buffer.size() > CHUNK) {
                json r;
                r["cmdtype"]  = FILE_DOWNLOAD;
                r["sequence"] = seq;
                r["result"]   = CONTINUE;
                json reply;
                reply["offset"]   = offset;
                reply["size"]     = (std::int64_t) CHUNK;
                reply["total"]    = total ? total : (std::int64_t)(offset + buffer.size());
                reply["file_md5"] = ""; // unverified
                r["reply"]        = std::move(reply);
                Emit(r.dump(), buffer.data(), CHUNK);
                offset += CHUNK;
                buffer.erase(buffer.begin(), buffer.begin() + CHUNK);
            }
            return !m_quit;
        });

    std::string md5_hex;
    int rc = FtpsRetrToOfs(path, sink, total, md5_hex);
    if (rc != 0) {
        EmitError(seq, FILE_DOWNLOAD, FILE_NO_EXIST);
        return;
    }

    // Final chunk (any tail in `buffer`) + SUCCESS marker. The receiver
    // computes md5 locally and compares against `file_md5` here.
    std::int64_t tail = (std::int64_t) buffer.size();
    json r;
    r["cmdtype"]  = FILE_DOWNLOAD;
    r["sequence"] = seq;
    r["result"]   = SUCCESS;
    json reply;
    reply["offset"]   = offset;
    reply["size"]     = tail;
    reply["total"]    = offset + tail;
    reply["file_md5"] = md5_hex;
    r["reply"]        = std::move(reply);
    Emit(r.dump(), buffer.empty() ? nullptr : buffer.data(), buffer.size());
}

void FtpsTransport::HandleSubFile(int seq, json const& req)
{
    bool        is_zip = req.value("zip", false);
    std::string first_path;
    if (req.contains("paths") && !req["paths"].empty())
        first_path = req["paths"][0].get<std::string>();
    else if (req.contains("files") && !req["files"].empty())
        first_path = req["files"][0].get<std::string>();

    if (is_zip && !first_path.empty()) {
        // FetchModel ("Print" button) and ModelMetadata (thumbnails/metadata
        // for the Storage list) both send `paths: [<fileA>#a, <fileA>#b,
        // <fileB>#a, ...]` with `zip: true`. The receiver routes data by the
        // `path` field in our reply, so we need one response per *unique*
        // file. We RETR each file's full .gcode.3mf, cache it, and emit it as
        // a sequence of CONTINUE chunks plus a terminal marker (the last
        // file's terminal marker carries result=SUCCESS to free the seq).
        std::vector<std::string> unique_fps;
        if (req.contains("paths") && req["paths"].is_array()) {
            for (auto& p : req["paths"]) {
                if (!p.is_string()) continue;
                std::string s    = p.get<std::string>();
                auto        h    = s.find('#');
                std::string fp_  = h == std::string::npos ? s : s.substr(0, h);
                if (std::find(unique_fps.begin(), unique_fps.end(), fp_) == unique_fps.end())
                    unique_fps.push_back(std::move(fp_));
            }
        }
        if (unique_fps.empty()) {
            EmitError(seq, SUB_FILE, FILE_NO_EXIST);
            return;
        }

        for (std::size_t fi = 0; fi < unique_fps.size(); ++fi) {
            std::string const& fp        = unique_fps[fi];
            bool               last_file = (fi + 1 == unique_fps.size());

            // Try cache before fetching. Any mutation of m_zip_cache below
            // (emplace_back / erase) invalidates iterators, so we don't keep
            // a `cit` around — re-resolve after the mutation is done.
            bool cache_hit = std::any_of(m_zip_cache.begin(), m_zip_cache.end(),
                                         [&fp](auto const& p) { return p.first == fp; });
            std::vector<unsigned char> full_local; // populated only on miss
            if (!cache_hit) {
                auto sink = std::function<bool(unsigned char const*, std::size_t)>(
                    [&](unsigned char const* data, std::size_t n) -> bool {
                        full_local.insert(full_local.end(), data, data + n);
                        return !m_quit;
                    });
                std::int64_t total = 0;
                std::string  md5_unused;
                int          rc = FtpsRetrToOfs(fp, sink, total, md5_unused);
                if (rc != 0) {
                    if (last_file) {
                        // Surface a terminal error so the seq frees.
                        EmitError(seq, SUB_FILE, FILE_NO_EXIST);
                        return;
                    }
                    // Skip this file; keep going so the others still emit.
                    continue;
                }
                // Move into cache (LRU). Evict on entry count first, then on
                // total bytes — but never evict the entry we just inserted,
                // since the chunk loop below reads from it.
                m_zip_cache.emplace_back(fp, std::move(full_local));
                while (m_zip_cache.size() > kZipCacheMaxEntries)
                    m_zip_cache.erase(m_zip_cache.begin());
                auto total_bytes = [this] {
                    std::size_t n = 0;
                    for (auto const& e : m_zip_cache) n += e.second.size();
                    return n;
                };
                while (m_zip_cache.size() > 1 && total_bytes() > kZipCacheMaxBytes)
                    m_zip_cache.erase(m_zip_cache.begin());
            }
            // Re-resolve to get a stable reference to the canonical bytes for
            // the chunk loop, regardless of whether we hit cache or just
            // inserted (and possibly shifted entries via eviction).
            auto cit = std::find_if(m_zip_cache.begin(), m_zip_cache.end(),
                                    [&fp](auto const& p) { return p.first == fp; });
            if (cit == m_zip_cache.end()) {
                if (last_file) {
                    EmitError(seq, SUB_FILE, FILE_NO_EXIST);
                    return;
                }
                continue;
            }
            std::vector<unsigned char> const& src = cit->second;

            // Chunk-emit this file. Intermediate chunks: result=CONTINUE,
            // continue=true. Per-file terminal: continue=false. The very last
            // file's terminal also carries result=SUCCESS to release the seq.
            constexpr std::size_t CHUNK   = 256 * 1024;
            std::size_t           emitted = 0;
            while (src.size() - emitted > CHUNK) {
                json r;
                r["cmdtype"]  = SUB_FILE;
                r["sequence"] = seq;
                r["result"]   = CONTINUE;
                json reply;
                reply["path"]     = fp;
                reply["size"]     = (std::int64_t) CHUNK;
                reply["continue"] = true;
                r["reply"]        = std::move(reply);
                Emit(r.dump(), src.data() + emitted, CHUNK);
                emitted += CHUNK;
            }
            std::size_t tail = src.size() - emitted;
            json        r;
            r["cmdtype"]  = SUB_FILE;
            r["sequence"] = seq;
            r["result"]   = last_file ? SUCCESS : CONTINUE;
            json reply;
            reply["path"]     = fp;
            reply["size"]     = (std::int64_t) tail;
            reply["continue"] = false;
            r["reply"]        = std::move(reply);
            Emit(r.dump(), tail ? src.data() + emitted : nullptr, tail);
        }
        return;
    }

    // Non-zip multi-path SUB_FILE: VideoThumbnail (paths end in #thumbnail)
    // or ModelThumbnail (paths end in #Metadata/plate_N.png inside the zip).
    // Receiver routes data by the reply.path field, so emit one response per
    // requested path; only the last carries result=SUCCESS.
    //
    // Skipping rule for empty bytes: PrinterFileSystem's translator returns
    // FILE_SIZE_ERR on size==0, which (via the c-callback wrapper) overrides
    // root.result. For an INTERMEDIATE chunk this would convert our CONTINUE
    // into FILE_SIZE_ERR and HandleResponse would CancelRequest the seq —
    // killing every emit that follows. So skip emitting empty-bytes
    // intermediates entirely; the missing files get their FF_THUMNAIL set by
    // the FinishThumbnail cascade once the last (real) emit terminates.
    if (!is_zip && req.contains("paths") && req["paths"].is_array() && !req["paths"].empty()) {
        auto const& paths = req["paths"];
        for (std::size_t i = 0; i < paths.size(); ++i) {
            if (!paths[i].is_string()) continue;
            std::string p   = paths[i].get<std::string>();
            bool        last = (i + 1 == paths.size());
            auto        hash = p.find('#');
            std::string before = hash == std::string::npos ? p : p.substr(0, hash);
            std::string sub    = hash == std::string::npos ? std::string() : p.substr(hash + 1);

            std::vector<unsigned char> bytes;
            std::string                mimetype;

            if (sub == "thumbnail" &&
                before.size() > 4 &&
                boost::iequals(before.substr(before.size() - 4), std::string(".mp4"))) {
                // VideoThumbnail: RETR /timelapse/thumbnail/<basename>.jpg.
                auto sl       = before.find_last_of('/');
                std::string base = before.substr(sl == std::string::npos ? 0 : sl + 1);
                base.resize(base.size() - 4);
                std::string jpg = "/timelapse/thumbnail/" + base + ".jpg";
                auto sink = std::function<bool(unsigned char const*, std::size_t)>(
                    [&](unsigned char const* d, std::size_t n) -> bool {
                        bytes.insert(bytes.end(), d, d + n);
                        return !m_quit;
                    });
                std::int64_t total = 0;
                std::string  md5_unused;
                if (FtpsRetrToOfs(jpg, sink, total, md5_unused) != 0)
                    bytes.clear();
                if (!bytes.empty()) mimetype = "image/jpeg";
            } else if (!sub.empty()) {
                // ModelThumbnail: extract the requested entry from the cached
                // .gcode.3mf zip (populated by the preceding ModelMetadata).
                auto it = std::find_if(m_zip_cache.begin(), m_zip_cache.end(),
                                       [&before](auto const& cp) { return cp.first == before; });
                if (it != m_zip_cache.end()) {
                    mz_zip_archive zip;
                    std::memset(&zip, 0, sizeof(zip));
                    if (mz_zip_reader_init_mem(&zip, it->second.data(), it->second.size(), 0)) {
                        int idx = mz_zip_reader_locate_file(&zip, sub.c_str(), nullptr, 0);
                        if (idx >= 0) {
                            mz_zip_archive_file_stat st;
                            // Cap the uncompressed size we'll allocate. These
                            // are PNG/JPG plate previews and SHOULD be well
                            // under a megabyte; the cap defends against a
                            // corrupt or hostile zip that declares a 4 GB
                            // entry and triggers a giant allocation/abort.
                            constexpr std::uint64_t kMaxThumbnailBytes = 32u * 1024u * 1024u;
                            if (mz_zip_reader_file_stat(&zip, idx, &st)
                                && st.m_uncomp_size > 0
                                && st.m_uncomp_size <= kMaxThumbnailBytes) {
                                bytes.resize((std::size_t) st.m_uncomp_size);
                                if (!mz_zip_reader_extract_to_mem(&zip, idx, bytes.data(), bytes.size(), 0))
                                    bytes.clear();
                            }
                        }
                        mz_zip_reader_end(&zip);
                    }
                }
                if (!bytes.empty()) {
                    auto dot = sub.find_last_of('.');
                    std::string ext = dot == std::string::npos ? "" : sub.substr(dot + 1);
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if (ext == "jpg" || ext == "jpeg") mimetype = "image/jpeg";
                    else if (!ext.empty())             mimetype = "image/" + ext;
                    else                                mimetype = "image/png";
                }
            }

            // Skip empty-bytes intermediates (see comment above the loop).
            // Empty + last is fine: the receiver returns FILE_SIZE_ERR but
            // root.result was already SUCCESS, so HandleResponse won't cancel
            // the seq — it just frees the slot and cascades.
            if (bytes.empty() && !last) continue;

            json r;
            r["cmdtype"]  = SUB_FILE;
            r["sequence"] = seq;
            r["result"]   = last ? SUCCESS : CONTINUE;
            json reply;
            reply["path"]      = p;
            reply["thumbnail"] = p;
            if (bytes.empty()) {
                reply["size"] = 0;
            } else {
                reply["mimetype"] = mimetype;
                reply["size"]     = (std::int64_t) bytes.size();
                reply["continue"] = false;
            }
            r["reply"] = std::move(reply);
            Emit(r.dump(), bytes.empty() ? nullptr : bytes.data(), bytes.size());
        }
        return;
    }

    // OldThumbnail (req["files"]) or empty request — nothing to serve.
    json r;
    r["cmdtype"]  = SUB_FILE;
    r["sequence"] = seq;
    r["result"]   = SUCCESS;
    json reply;
    reply["path"]      = first_path;
    reply["thumbnail"] = first_path;
    reply["size"]      = 0;
    r["reply"]         = std::move(reply);
    Emit(r.dump());
}

void FtpsTransport::HandleTaskCancel(int seq, json const& req)
{
    json r;
    r["cmdtype"]  = TASK_CANCEL;
    r["sequence"] = seq;
    r["result"]   = SUCCESS;
    json reply;
    reply["tasks"] = req.value("tasks", json::array());
    r["reply"]     = std::move(reply);
    Emit(r.dump());
}

void FtpsTransport::HandleUnsupported(int seq, int cmdtype)
{
    EmitError(seq, cmdtype, ERROR_PIPE);
}

int FtpsTransport::FtpsMlsd(std::string const& path, std::string& out)
{
    auto* curl = static_cast<CURL*>(m_curl);
    if (!curl) return -1;

    std::string url = "ftps://" + m_host + ":990" + EncodeFtpPath(path);

    out.clear();
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, m_user.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, m_pass.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL, (long) CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FTP_USE_EPSV, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        BOOST_LOG_TRIVIAL(warning) << "FtpsTransport::FtpsMlsd " << path
                                   << " (LIST) failed: " << curl_easy_strerror(rc);
        return -1;
    }
    return 0;
}

int FtpsTransport::FtpsDelete(std::string const& path)
{
    auto* curl = static_cast<CURL*>(m_curl);
    if (!curl) return -1;

    // Defense-in-depth at the FTP control-channel boundary. parse_list_line
    // already strips CR/LF/NUL from filenames it produces, but HandleFileDel
    // accepts paths from inbound JSON. Reject any control byte here so a
    // path with embedded \r\n can't inject a second QUOTE command into the
    // control channel.
    for (unsigned char c : path) {
        if (c < 0x20 || c == 0x7F) {
            BOOST_LOG_TRIVIAL(warning)
                << "FtpsTransport::FtpsDelete refusing path with control byte";
            return -1;
        }
    }

    auto sp = path.find_last_of('/');
    std::string dir  = sp == std::string::npos ? "/" : path.substr(0, sp + 1);
    std::string file = sp == std::string::npos ? path : path.substr(sp + 1);

    std::string url = "ftps://" + m_host + ":990" + EncodeFtpPath(dir);
    std::string cmd = "DELE " + file;
    struct curl_slist* slist = nullptr;
    slist = curl_slist_append(slist, cmd.c_str());

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, m_user.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, m_pass.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL, (long) CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FTP_USE_EPSV, 1L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_QUOTE, slist);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode rc = curl_easy_perform(curl);
    long ftp_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &ftp_code);
    curl_slist_free_all(slist);
    if (rc != CURLE_OK) {
        BOOST_LOG_TRIVIAL(warning) << "FtpsTransport::FtpsDelete " << path
                                   << " failed: " << curl_easy_strerror(rc)
                                   << " (ftp " << ftp_code << ")";
        // FTP 550 from a QUOTE command means "file not found / no permission".
        // Anything else is a transport-level failure (TLS handshake, timeout,
        // connection refused). Surface them differently so the UI doesn't
        // claim "file not found" on a network blip.
        return ftp_code == 550 ? -2 : -1;
    }
    return 0;
}

int FtpsTransport::FtpsRetrToOfs(std::string const& path,
                                 std::function<bool(unsigned char const*, std::size_t)> sink,
                                 std::int64_t& total_size,
                                 std::string&  out_md5_hex)
{
    auto* curl = static_cast<CURL*>(m_curl);
    if (!curl) return -1;

    std::string url = "ftps://" + m_host + ":990" + EncodeFtpPath(path);

    // Wrap the caller's sink so we can also feed bytes to MD5. PrinterFileSystem
    // computes its own md5 over the bytes it writes locally and compares to
    // the one we send on the final SUCCESS — they have to match.
    boost::uuids::detail::md5 md5_ctx;
    auto wrapped = std::function<bool(unsigned char const*, std::size_t)>(
        [&](unsigned char const* data, std::size_t n) -> bool {
            md5_ctx.process_bytes(data, n);
            return sink(data, n);
        });

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, m_user.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, m_pass.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL, (long) CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FTP_USE_EPSV, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L); // unbounded for large files
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_to_sink);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wrapped);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_abort_on_quit);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &m_quit);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        BOOST_LOG_TRIVIAL(warning) << "FtpsTransport::FtpsRetrToOfs " << path
                                   << " failed: " << curl_easy_strerror(rc);
        return -1;
    }
    curl_off_t dl = 0;
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &dl);
    total_size = static_cast<std::int64_t>(dl);

    boost::uuids::detail::md5::digest_type digest;
    md5_ctx.get_digest(digest);
    for (int i = 0; i < 4; ++i)
        digest[i] = boost::endian::endian_reverse(digest[i]);
    auto const* dp = reinterpret_cast<char const*>(&digest[0]);
    out_md5_hex.clear();
    boost::algorithm::hex(dp, dp + sizeof(digest), std::back_inserter(out_md5_hex));
    return 0;
}
