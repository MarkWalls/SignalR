#include "LongPollingTransport.h"

namespace MicrosoftAspNetSignalRClientCpp
{

LongPollingTransport::LongPollingTransport(shared_ptr<IHttpClient> httpClient) :
    HttpBasedTransport(httpClient, U("longPolling"))
{
}

LongPollingTransport::~LongPollingTransport()
{
}

void LongPollingTransport::OnStart(shared_ptr<Connection> connection, string_t data, pplx::cancellation_token disconnectToken, shared_ptr<TransportInitializationHandler> initializeHandler)
//void LongPollingTransport::OnStart(shared_ptr<Connection> connection, string_t data, pplx::cancellation_token disconnectToken, function<void()> initializeCallback, function<void()> errorCallback)
{
}

void LongPollingTransport::OnAbort()
{
}

void LongPollingTransport::LostConnection(shared_ptr<Connection> connection)
{
}

} // namespace MicrosoftAspNetSignalRClientCpp