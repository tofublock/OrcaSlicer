#ifndef slic3r_GUI_BambuTunnelTransport_h_
#define slic3r_GUI_BambuTunnelTransport_h_

#define BAMBU_DYNAMIC
#include "BambuTunnel.h"
#include "IPrinterFileTransport.h"

#include <string>

// Byte-level transport backed by the closed-source Bambu_Tunnel API.
// PrinterFileSystem owns one of these via a unique_ptr<IPrinterFileTransport>.
// The class holds a BambuLib reference (the dynamically-loaded symbol table)
// rather than inheriting from BambuLib so the caller can share a single
// resolved table.
class BambuTunnelTransport : public IPrinterFileTransport
{
public:
    BambuTunnelTransport(std::string url, BambuLib const& lib);
    ~BambuTunnelTransport() override;

    int  Open() override;
    int  StartStream(int ctrl_type) override;
    int  SendMessage(int ctrl_type, char const* data, int len) override;
    int  ReadSample(Sample& out) override;
    void Close() override;
    bool IsOpen() const override { return m_tunnel != nullptr; }

    void SetLogger(LogFn fn, void* context) override;
    void FreeLogMsg(char const* msg) override;

private:
    std::string  m_url;
    BambuLib const& m_lib;
    Bambu_Tunnel m_tunnel = nullptr;
    LogFn        m_log_fn = nullptr;
    void*        m_log_ctx = nullptr;
};

#endif
