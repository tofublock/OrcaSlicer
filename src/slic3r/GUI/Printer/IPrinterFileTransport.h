#ifndef slic3r_GUI_IPrinterFileTransport_h_
#define slic3r_GUI_IPrinterFileTransport_h_

#include <cstddef>

// Abstraction over the byte transport used by PrinterFileSystem to talk to a
// printer's file-browsing endpoint. Two implementations:
//   - BambuTunnelTransport: wraps the closed-source Bambu_Tunnel API.
//   - FtpsTransport:        speaks implicit FTPS to a Bambu printer in
//                           LAN-only / Developer Mode (port 990).
//
// The interface is intentionally a thin 1:1 wrapper around the byte-level
// Bambu_Tunnel calls PrinterFileSystem already uses, so the upper layer's
// threading, queueing and reconnect logic stays unchanged. The FTPS impl
// translates outgoing JSON-RPC requests into FTPS commands and synthesizes
// Bambu-shaped JSON replies internally.
//
// Return codes match the Bambu_Error enum so existing callers comparing
// against Bambu_would_block / Bambu_stream_end keep working when the call is
// routed through the transport.

class IPrinterFileTransport
{
public:
    enum Result {
        OK             = 0, // == Bambu_success
        STREAM_END     = 1, // == Bambu_stream_end
        WOULD_BLOCK    = 2, // == Bambu_would_block
        BUFFER_LIMIT   = 3, // == Bambu_buffer_limit
    };

    struct Sample {
        unsigned char const* buffer = nullptr;
        std::size_t          size   = 0;
    };

    // Matches Bambu's Logger signature.
    using LogFn = void (*)(void* context, int level, char const* msg);

    virtual ~IPrinterFileTransport() = default;

    // Create + Open the connection described by the URL passed to the
    // concrete transport's constructor. Returns 0 on success, non-zero on
    // error. Blocking.
    virtual int  Open()                                             = 0;

    // Start a control stream. Mirrors Bambu_StartStreamEx; OK / WOULD_BLOCK
    // / error are all valid.
    virtual int  StartStream(int ctrl_type)                         = 0;

    virtual int  SendMessage(int ctrl_type,
                             char const* data,
                             int len)                               = 0;

    // On OK, `out` references a buffer owned by the transport, valid until
    // the next ReadSample / Close.
    virtual int  ReadSample(Sample& out)                            = 0;

    virtual void Close()                                            = 0;
    virtual bool IsOpen() const                                     = 0;

    virtual void SetLogger(LogFn fn, void* context)                 = 0;
    virtual void FreeLogMsg(char const* msg)                        = 0;
};

#endif
