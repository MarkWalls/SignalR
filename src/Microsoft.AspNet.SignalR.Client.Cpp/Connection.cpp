#include "Connection.h"
#include "LongPollingTransport.h"
#include "ServerSentEventsTransport.h"

namespace MicrosoftAspNetSignalRClientCpp
{

Connection::Connection(string_t uri) : cDefaultAbortTimeout(30)
{
    if (uri.empty())
    {
        throw exception("ArgumentNullException: uri");
    }
    mUri = uri;
    if (!(mUri.back() == U('/')))
    {
        mUri += U("/");
    }

    mState = ConnectionState::Disconnected;
    mProtocol = U("1.3");
    mTransportConnectTimeout = seconds(0);
}

Connection::~Connection()
{
}

ConnectionState Connection::GetState()
{
    return mState;
}

shared_ptr<IClientTransport> Connection::GetTransport()
{
    return pTransport;
}

string_t Connection::GetUri()
{
    return mUri;
}

string_t Connection::GetConnectionId()
{
    return mConnectionId;
}

string_t Connection::GetConnectionToken()
{
    return mConnectionToken;
}

string_t Connection::GetGroupsToken()
{
    return mGroupsToken;
}

string_t Connection::GetMessageId()
{
    return mMessageId;
}

string_t Connection::GetQueryString()
{
    return mQueryString;
}

string_t Connection::GetProtocol()
{
    return mProtocol;
}

seconds Connection::GetTransportConnectTimeout()
{
    return mTransportConnectTimeout;
}

void Connection::SetMessageId(string_t messageId)
{
    mMessageId = messageId;
}

void Connection::SetConnectionToken(string_t connectionToken)
{
    mConnectionToken = connectionToken;
}

void Connection::SetConnectionId(string_t connectionId)
{
    mConnectionId = connectionId;
}

void Connection::SetProtocol(string_t protocol)
{
    mProtocol = protocol;
}

void Connection::SetGroupsToken(string_t groupsToken)
{
    mGroupsToken = groupsToken;
}

void Connection::GetTransportConnectTimeout(seconds transportConnectTimeout)
{
    mTransportConnectTimeout = transportConnectTimeout;
}

pplx::task<void> Connection::Start() 
{
    return Start(shared_ptr<IHttpClient>(new DefaultHttpClient()));
}

pplx::task<void> Connection::Start(shared_ptr<IHttpClient> client) 
{	
    //return Start(new AutoTransport(client));
    // default to using ServerSentEvents
    return Start(shared_ptr<IClientTransport>(new ServerSentEventsTransport(client))); 
}

pplx::task<void> Connection::Start(shared_ptr<IClientTransport> transport) 
{	
    lock_guard<mutex> lock(mStartLock);

    mConnectingMessageBuffer.Initialize(shared_from_this(), [this](string_t message)
    {
        OnMessageReceived(message);
    });

    mConnectTask = pplx::task<void>();
    pDisconnectCts = unique_ptr<pplx::cancellation_token_source>(new pplx::cancellation_token_source());

    if(!ChangeState(ConnectionState::Disconnected, ConnectionState::Connecting))
    {
        return mConnectTask; 
    }
    
    pTransport = transport;
    mConnectTask = Negotiate(transport);

    return mConnectTask;
}

pplx::task<void> Connection::Negotiate(shared_ptr<IClientTransport> transport) 
{
    return pTransport->Negotiate(shared_from_this()).then([this](pplx::task<shared_ptr<NegotiationResponse>> negotiateTask)
    {
        shared_ptr<NegotiationResponse> response;
        exception ex;
        TaskStatus status = TaskAsyncHelper::RunTaskToCompletion<shared_ptr<NegotiationResponse>>(negotiateTask, response, ex);

        if (status == TaskStatus::TaskCompleted)
        {
            mConnectionId = response->mConnectionId;
            mConnectionToken = response->mConnectionToken;
            mTransportConnectTimeout = seconds(mTransportConnectTimeout.count() + response->mTransportConnectTimeout);

            return StartTransport();
        } 
        else
        {
            return pplx::task<void>([this]()
            {
                Disconnect();        
                // also throw some error
            });
        }
    });
}

pplx::task<void> Connection::StartTransport()
{
    pplx::cancellation_token token = pDisconnectCts->get_token();
    return pTransport->Start(shared_from_this(), U(""), pDisconnectCts->get_token()).then([this]()
    {
        ChangeState(ConnectionState::Connecting, ConnectionState::Connected);

        // Now that we're connected drain any messages within the buffer
        // We want to protect against state changes when draining
        {
            lock_guard<recursive_mutex> lock(mStateLock);
            mConnectingMessageBuffer.Drain();
        }
    });
}

pplx::task<void> Connection::Send(value::field_map object)
{
    stringstream_t stream;
    value v1 = value::object(object);
    v1.serialize(stream); 

    return Send(stream.str());
}

pplx::task<void> Connection::Send(string_t data)
{
    if (mState == ConnectionState::Disconnected)
    {
        throw exception("InvalidOperationException: The Start method must be called before data can be sent.");
    }
    if (mState == ConnectionState::Connecting)
    {
        throw exception("InvalidOperationException: The connection has not been established.");
    }

    return pTransport->Send(shared_from_this(), data);
}

bool Connection::ChangeState(ConnectionState oldState, ConnectionState newState)
{
    lock_guard<recursive_mutex> lock (mStateLock);

    if(mState == oldState)
    {
        SetState(newState);
        return true;
    }

    // Invalid transition
    return false;
}

bool Connection::EnsureReconnecting()
{
    if(ChangeState(ConnectionState::Connected, ConnectionState::Reconnecting))
    {
        OnReconnecting();
    }
            
    return mState == ConnectionState::Reconnecting;
}

void Connection::Stop()
{
    Stop(cDefaultAbortTimeout);
}

void Connection::Stop(seconds timeout) 
{
    lock_guard<mutex> lock(mStartLock);

    if (mConnectTask != pplx::task<void>())
    {
        try
        {
            if (!mConnectTask.is_done())
            {
                (mConnectTask || TaskAsyncHelper::Delay(timeout)).wait(); // the memory will eventually be reclaimed after 30 seconds so it's not a leak here
            }
        }
        catch (exception& ex)
        {
            //Trace
        }
    }

    {
        lock_guard<recursive_mutex> lock(mStateLock);

        if (mState != ConnectionState::Disconnected)
        {
            // If we've connected then instantly disconnected we may have data in the incomingMessageBuffer
            // Therefore we need to clear it incase we start the connection again. Also reset the buffer to 
            // avoid leaks due to circular referencing
            mConnectingMessageBuffer.Clear();

            pTransport->Abort(shared_from_this());

            Disconnect();
        }
    }
}

void Connection::Disconnect()
{
    lock_guard<recursive_mutex> lock(mStateLock);

    if (mState != ConnectionState::Disconnected)
    {
        SetState(ConnectionState::Disconnected);

        pDisconnectCts->cancel();

        mConnectionId.clear();
        mConnectionToken.clear();
        mGroupsToken.clear();
        mMessageId.clear();

        if (Closed != nullptr)
        {
            lock_guard<mutex> lock(mClosedLock);
            Closed();
        }
    }
}

void Connection::OnError(exception& ex)
{
    if (Error != nullptr)
    {
        lock_guard<mutex> lock(mErrorLock);
        Error(ex);
    }
}

void Connection::OnReceived(string_t message)
{
    lock_guard<recursive_mutex> lock(mStateLock);
    
    if (!mConnectingMessageBuffer.TryBuffer(message))
    {
        OnMessageReceived(message);
    }
}

void Connection::OnMessageReceived(string_t message)
{
    if (Received != nullptr)
    {
        try 
        {
            lock_guard<mutex> lock(mReceivedLock);
            Received(message);
        }
        catch (exception& ex)
        {
            OnError(ex);
        }
    }
}

void Connection::OnReconnecting()
{
    if (Reconnecting != nullptr)
    {
        lock_guard<mutex> lock(mReconnectingLock);
        Reconnecting();
    }
}

void Connection::OnReconnected()
{
    if (Reconnected != nullptr)
    {
        lock_guard<mutex> lock(mReconnectedLock);
        Reconnected();
    }
}

void Connection::OnConnectionSlow()
{
    if (ConnectionSlow != nullptr)
    {
        lock_guard<mutex> lock(mConnectionSlowLock);
        ConnectionSlow();
    }
}

void Connection::PrepareRequest(shared_ptr<HttpRequestWrapper> request)
{

}

void Connection::SetState(ConnectionState newState)
{
    lock_guard<recursive_mutex> lock(mStateLock);

    StateChange stateChange(mState, newState);
    mState = newState;

    if (StateChanged != nullptr)
    {
        lock_guard<mutex> lock(mStateChangedLock);
        StateChanged(stateChange);
    }
}

void Connection::SetStateChangedCallback(function<void(StateChange)> stateChanged)
{
    lock_guard<mutex> lock(mStateChangedLock);
    StateChanged = stateChanged;
}

void Connection::SetReconnectingCallback(function<void()> reconnecting)
{
    lock_guard<mutex> lock(mReconnectingLock);
    Reconnecting = reconnecting;
}

void Connection::SetReconnectedCallback(function<void()> reconnected)
{
    lock_guard<mutex> lock(mReconnectedLock);
    Reconnected = reconnected;
}

void Connection::SetConnectionSlowCallback(function<void()> connectionSlow)
{
    lock_guard<mutex> lock(mConnectionSlowLock);
    ConnectionSlow = connectionSlow;
}

void Connection::SetErrorCallback(function<void(exception&)> error)
{
    lock_guard<mutex> lock(mErrorLock);
    Error = error;
}

void Connection::SetClosedCallback(function<void()> closed)
{
    lock_guard<mutex> lock(mClosedLock);
    Closed = closed;
}

void Connection::SetReceivedCallback(function<void(string_t)> received)
{
    lock_guard<mutex> lock(mReceivedLock);
    Received = received;
}

} // namespace MicrosoftAspNetSignalRClientCpp