#pragma once

#include <http_client.h>
#include "StringHelper.h"

using namespace std;
using namespace utility;

namespace MicrosoftAspNetSignalRClientCpp
{
    class ChunkBuffer
    {
    public:
        ChunkBuffer();
        ~ChunkBuffer();

        bool HasChuncks();
        void Add(shared_ptr<char> buffer);
        string_t ReadLine();

    private:
        unsigned int mOffset;
        string_t mBuffer;
        string_t mLineBuilder;
    };
} // namespace MicrosoftAspNetSignalRClientCpp