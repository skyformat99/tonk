/*
    Copyright (c) 2018 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Tonkinese nor the names of its contributors may be
      used to endorse or promote products derived from this software without
      specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

/**
    FileReceiver

    This receives files from the FileSender.  It receives the file and closes.

    It is incompatible with P2PFileSender.
*/

#include "tonk.h"
#include "tonk_file_transfer.h"
#include "TonkCppSDK.h"
#include "Logger.h"

#include <chrono>
#include <stdio.h>

static logger::Channel Logger("FileReceiver", logger::Level::Debug);


// Port for FileSender server to listen on
static const uint16_t kFileSenderServerPort = 10333;


class MyClient;

class MyClientConnection : public tonk::SDKConnection
{
    MyClient* Client = nullptr;
    TonkFile File = nullptr;
    uint64_t LastReportUsec = 0;

public:
    MyClientConnection(MyClient* client)
        : Client(client)
    {}

protected:
    void OnConnect() override;
    void OnData(
        uint32_t          channel,  // Channel number attached to each message by sender
        const uint8_t*       data,  // Pointer to a buffer containing the message data
        uint32_t            bytes   // Number of bytes in the message
    ) override;
    void OnClose(
        const tonk::SDKJsonResult& reason
    ) override;

    int OnFileDone(TonkFile file);
};

class MyClient : public tonk::SDKSocket
{
public:
    bool Initialize();

    // SDKSocket:
    void OnAdvertisement(
        const std::string& ipString, ///< Source IP address of advertisement
        uint16_t               port, ///< Source port address of advertisement
        const uint8_t*         data,  ///< Pointer to a buffer containing the message data
        uint32_t              bytes) override;  ///< Number of bytes in the message

    bool GotAdvertisementPong = false;

    std::atomic<bool> Complete = ATOMIC_VAR_INIT(false);
};


void MyClientConnection::OnConnect()
{
    auto status = GetStatusEx();

    Logger.Info("Connected to ", status.Remote.NetworkString, " : ", status.Remote.UDPPort);
}

void MyClientConnection::OnData(
    uint32_t          channel,  // Channel number attached to each message by sender
    const uint8_t*       data,  // Pointer to a buffer containing the message data
    uint32_t            bytes   // Number of bytes in the message
)
{
    if (channel == TonkChannel_LowPri0)
    {
        File = tonk_file_receive(
            File,
            data,
            bytes,
            [](TonkFile file, void* context) -> int
        {
            MyClientConnection* thiz = reinterpret_cast<MyClientConnection*>(context);

            return thiz->OnFileDone(file);
        }, this);

        const uint64_t nowUsec = tonk_time();
        if (File && nowUsec - LastReportUsec > 1000000)
        {
            LastReportUsec = nowUsec;
            Logger.Info("File(", File->Name, ") progress: ", File->ProgressBytes, " / ", File->TotalBytes,
                " Bytes (", File->ProgressBytes * 100 / (float)File->TotalBytes,
                "%).  Incoming at ", GetStatusEx().IncomingBPS / 1000.0, " KBPS");

            // Periodically send a ping to demonstrate that data is still low-latency
            uint8_t ping_message[1 + 8];
            ping_message[0] = 77;
            tonk::WriteU64_LE(ping_message + 1, nowUsec);
            Send(ping_message, 9, TonkChannel_Reliable1);
        }
    }
    else if (channel == TonkChannel_Reliable1 && bytes == 1 + 8 + 3)
    {
        uint64_t pingUsec = tonk::ReadU64_LE(data + 1);
        uint64_t pongUsec = tonk_time();

        uint32_t compressedRemoteUsec = tonk::ReadU24_LE(data + 1 + 8);
        uint64_t remoteUsec = FromLocalTime23(compressedRemoteUsec);

        Logger.Info("Low-latency ping RTT = ", (pongUsec - pingUsec) / 1000.f, " ms, OWD(remote2local) = ", (pongUsec - remoteUsec) / 1000.f, " ms");
    }
}

int MyClientConnection::OnFileDone(TonkFile file)
{
    if (file->Flags & TonkFileFlags_Failed)
    {
        Logger.Error("File transfer failed: ", file->Name);
        return TONK_FILE_DELETE;
    }

    Logger.Info("File(", file->Name, ") progress: ", file->ProgressBytes, " / ", file->TotalBytes,
        " Bytes (", file->ProgressBytes * 100 / (float)file->TotalBytes, "%)");

    if (file->OutputData)
    {
        Logger.Info("Written to memory buffer: ", file->TotalBytes, " bytes");
    }
    else
    {
        Logger.Info("File transfer completed to disk: ", file->OutputFileName);

        // Delete any old file with the target name
        std::remove(file->Name);

        // Rename the output file to the target file name
        std::rename(file->OutputFileName, file->Name);
    }

    // Mark it as complete so the session ends
    Client->Complete = true;

    return TONK_FILE_KEEP;
}

void MyClientConnection::OnClose(
    const tonk::SDKJsonResult& reason
)
{
    auto status = GetStatusEx();

    Logger.Info("Disconnected from ", status.Remote.NetworkString, " : ", status.Remote.UDPPort, " - Reason = ", reason.ToString());

    tonk_file_free(File);
    File = nullptr;

    // Mark it as complete so the session ends
    Client->Complete = true;
}


bool MyClient::Initialize()
{
    // Set configuration
    //Config.UDPListenPort = 0;
    //Config.MaximumClients = 0;
    //Config.EnableFEC = 0;
    //Config.InterfaceAddress = "127.0.0.1";
    //Config.BandwidthLimitBPS = 1000 * 1000 * 1000;

    tonk::SDKJsonResult result = Create();

    if (!result) {
        Logger.Error("Unable to create socket: ", result.ToString());
        return false;
    }

    return true;
}

void MyClient::OnAdvertisement(
    const std::string& ipString, ///< Source IP address of advertisement
    uint16_t               port, ///< Source port address of advertisement
    const uint8_t*         data,  ///< Pointer to a buffer containing the message data
    uint32_t              bytes)  ///< Number of bytes in the message
{
    if (GotAdvertisementPong) {
        return;
    }

    if (bytes != 1) {
        return;
    }

    if (data[0] == 200)
    {
        Logger.Info("Advertisement PONG received from ", ipString, ":", port, " - ", bytes, " bytes");

        GotAdvertisementPong = true;

        auto connection = std::make_shared<MyClientConnection>(this);
        connection->SetSelfReference();

        Connect(connection.get(), ipString, port);
    }
}


int main(int argc, char** argv)
{
    std::string hostname = "127.0.0.1";

    if (argc >= 2) {
        hostname = argv[1];
        Logger.Info("Server hostname: ", hostname, ", port: ", kFileSenderServerPort,
            " - Also broadcasting on the LAN to find server");
    }

    {
        MyClient client;

        if (client.Initialize()) {
            Logger.Debug("File sending client started");
        }
        else {
            Logger.Debug("File sending server failed to start");
        }

        bool connectionFailed = true;

        std::shared_ptr<MyClientConnection> connection;

        {
            connection = std::make_shared<MyClientConnection>(&client);

            tonk::SDKJsonResult result = client.Connect(connection.get(), hostname, kFileSenderServerPort);

            if (result.Failed())
            {
                Logger.Info("Client connection failed: ", result.ToString(),
                    " - Going to advertise on the LAN looking for connections");
            }
            else
            {
                connectionFailed = false;
            }
        }

        uint64_t LastAdvertisementUsec = 0;

        while (!client.Complete)
        {
            uint64_t nowUsec = tonk_time();

            if (nowUsec - LastAdvertisementUsec > 1000 * 1000)
            {
                LastAdvertisementUsec = nowUsec;

                uint8_t data[1] = {
                    100
                };

                // Broadcast looking for server on LAN if connection failed
                if (connectionFailed) {
                    client.Advertise(TONK_BROADCAST, kFileSenderServerPort, data, 1);
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        Logger.Debug("Client shutdown");
    }

    Logger.Debug("Press ENTER key to terminate");

    ::getchar();

    Logger.Debug("...Key press detected.  Terminating..");

    return 0;
}
