#ifndef slic3r_GUI_FtpsTransport_h_
#define slic3r_GUI_FtpsTransport_h_

#include "IPrinterFileTransport.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <functional>
#include <cstdint>

#include "nlohmann/json_fwd.hpp"

// Byte-level transport that speaks implicit FTPS (port 990) to a Bambu printer
// in LAN-only / Developer Mode. Translates outgoing JSON-RPC requests
// (LIST_INFO, FILE_DEL, FILE_DOWNLOAD, ...) into FTPS commands via libcurl
// and synthesizes Bambu-shaped JSON responses. Synchronous Open() probes the
// connection; subsequent SendMessage/ReadSample calls are non-blocking and
// dispatched on a single worker thread.
class FtpsTransport : public IPrinterFileTransport
{
public:
    explicit FtpsTransport(std::string url);
    ~FtpsTransport() override;

    int  Open() override;
    int  StartStream(int ctrl_type) override;
    int  SendMessage(int ctrl_type, char const* data, int len) override;
    int  ReadSample(Sample& out) override;
    void Close() override;
    bool IsOpen() const override { return m_open; }

    void SetLogger(LogFn fn, void* context) override;
    void FreeLogMsg(char const* msg) override;

private:
    struct PendingRequest {
        std::string body; // raw JSON-RPC payload (with optional "\n\n<param>" tail)
    };

    void WorkerLoop();
    void Dispatch(std::string const& body);

    // Per-cmd handlers. Each pushes one or more synthesized response buffers
    // onto m_pending_samples via Emit().
    void HandleListInfo(int seq, nlohmann::json const& req);
    void HandleFileDel(int seq, nlohmann::json const& req);
    void HandleMediaAbility(int seq, nlohmann::json const& req);
    void HandleFileDownload(int seq, nlohmann::json const& req);
    void HandleSubFile(int seq, nlohmann::json const& req);
    void HandleTaskCancel(int seq, nlohmann::json const& req);
    void HandleUnsupported(int seq, int cmdtype);

    void Emit(std::string const& json_line, unsigned char const* data = nullptr, std::size_t data_len = 0);
    void EmitError(int seq, int cmdtype, int result);

    // FTPS helpers (run on worker thread; serialize via single curl handle).
    int  FtpsMlsd(std::string const& path, std::string& out);
    int  FtpsDelete(std::string const& path);
    int  FtpsRetrToOfs(std::string const& path,
                       std::function<bool(unsigned char const*, std::size_t)> sink,
                       std::int64_t& total_size,
                       std::string&  out_md5_hex);

    static std::string FolderForType(std::string const& type);
    static std::string EncodeFtpPath(std::string const& path);

private:
    std::string m_url;
    std::string m_host;
    std::string m_user = "bblp";
    std::string m_pass;

    std::atomic<bool>        m_open{false};
    std::atomic<bool>        m_quit{false};
    LogFn                    m_log_fn  = nullptr;
    void*                    m_log_ctx = nullptr;

    std::mutex               m_mutex;
    std::condition_variable  m_cond;
    std::deque<PendingRequest>            m_requests;
    std::deque<std::vector<unsigned char>> m_pending_samples;
    std::vector<unsigned char>            m_current_sample;

    // LRU cache of recently-RETR'd .gcode.3mf blobs, keyed by FTPS path. The
    // ModelMetadata flow downloads the whole zip; the follow-up ModelThumbnail
    // flow asks for `<file>#Metadata/plate_X.png`. Caching avoids a second
    // full download per model.
    //
    // Worker-thread-only: read and written exclusively from WorkerLoop()'s
    // dispatch path and cleared by Close() *after* the worker has joined.
    // Don't access from other threads — no mutex protects this field.
    std::vector<std::pair<std::string, std::vector<unsigned char>>> m_zip_cache;
    static constexpr std::size_t kZipCacheMaxEntries = 4;
    // Byte cap on top of the entry cap. .gcode.3mf can be 200+ MB; with 4
    // entries the worst-case resident set is several GB. Evict by bytes once
    // we go past this threshold (but always keep at least one entry — the
    // caller needs the just-inserted blob to chunk back to the receiver).
    static constexpr std::size_t kZipCacheMaxBytes = 512u * 1024u * 1024u;

    // Bound the queue of synthesized response samples. Without this the
    // worker can buffer an entire .gcode.3mf as 256 KB chunks ahead of the
    // recv thread's drain. With 8 samples × 256 KB the worst-case backlog is
    // ~2 MB; Emit() blocks the worker until the consumer drains.
    static constexpr std::size_t kMaxPendingSamples = 8;

    std::thread              m_worker;

    // Created on the calling thread inside Open() (single synchronous probe),
    // then handed off to and used exclusively by m_worker until Close()
    // joins. libcurl handles must not be shared between threads
    // concurrently; the handoff is safe because Open() returns before the
    // worker starts touching the handle.
    void* m_curl = nullptr; // opaque CURL*
};

#endif
