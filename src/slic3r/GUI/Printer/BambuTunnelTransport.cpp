#include "BambuTunnelTransport.h"

#include <utility>

BambuTunnelTransport::BambuTunnelTransport(std::string url, BambuLib const& lib)
    : m_url(std::move(url)), m_lib(lib)
{}

BambuTunnelTransport::~BambuTunnelTransport()
{
    BambuTunnelTransport::Close();
}

int BambuTunnelTransport::Open()
{
    if (m_tunnel != nullptr)
        return 0;
    int ret = m_lib.Bambu_Create(&m_tunnel, m_url.c_str());
    if (ret != 0) {
        m_tunnel = nullptr;
        return ret;
    }
    if (m_log_fn)
        m_lib.Bambu_SetLogger(m_tunnel, m_log_fn, m_log_ctx);
    ret = m_lib.Bambu_Open(m_tunnel);
    if (ret != 0) {
        Close();
        return ret;
    }
    return 0;
}

int BambuTunnelTransport::StartStream(int ctrl_type)
{
    if (m_tunnel == nullptr)
        return -1;
    if (m_lib.Bambu_StartStreamEx)
        return m_lib.Bambu_StartStreamEx(m_tunnel, ctrl_type);
    return m_lib.Bambu_StartStream(m_tunnel, false);
}

int BambuTunnelTransport::SendMessage(int ctrl_type, char const* data, int len)
{
    if (m_tunnel == nullptr)
        return -1;
    return m_lib.Bambu_SendMessage(m_tunnel, ctrl_type, data, len);
}

int BambuTunnelTransport::ReadSample(Sample& out)
{
    if (m_tunnel == nullptr)
        return -1;
    Bambu_Sample sample{};
    int ret = m_lib.Bambu_ReadSample(m_tunnel, &sample);
    if (ret == 0) {
        out.buffer = sample.buffer;
        out.size   = static_cast<std::size_t>(sample.size);
    }
    return ret;
}

void BambuTunnelTransport::Close()
{
    if (m_tunnel == nullptr)
        return;
    Bambu_Tunnel t = m_tunnel;
    m_tunnel = nullptr;
    m_lib.Bambu_Close(t);
    m_lib.Bambu_Destroy(t);
}

void BambuTunnelTransport::SetLogger(LogFn fn, void* context)
{
    m_log_fn  = fn;
    m_log_ctx = context;
    if (m_tunnel)
        m_lib.Bambu_SetLogger(m_tunnel, fn, context);
}

void BambuTunnelTransport::FreeLogMsg(char const* msg)
{
    m_lib.Bambu_FreeLogMsg(msg);
}
