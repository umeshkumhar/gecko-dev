/* -*- Mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; tab-width: 40 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/basictypes.h"
#include "BluetoothOppManager.h"

#include "BluetoothReplyRunnable.h"
#include "BluetoothService.h"
#include "BluetoothSocket.h"
#include "BluetoothUtils.h"
#include "BluetoothUuid.h"
#include "ObexBase.h"

#include "mozilla/dom/bluetooth/BluetoothTypes.h"
#include "mozilla/RefPtr.h"
#include "mozilla/Services.h"
#include "mozilla/StaticPtr.h"
#include "nsAutoPtr.h"
#include "nsCExternalHandlerService.h"
#include "nsIDOMFile.h"
#include "nsIFile.h"
#include "nsIInputStream.h"
#include "nsIMIMEService.h"
#include "nsIObserver.h"
#include "nsIObserverService.h"
#include "nsIOutputStream.h"
#include "nsNetUtil.h"

#define TARGET_ROOT   "/sdcard/"
#define TARGET_SUBDIR "downloads/bluetooth/"
#define TARGET_FOLDER TARGET_ROOT TARGET_SUBDIR

USING_BLUETOOTH_NAMESPACE
using namespace mozilla;
using namespace mozilla::ipc;

class BluetoothOppManagerObserver : public nsIObserver
{
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER

  BluetoothOppManagerObserver()
  {
  }

  bool Init()
  {
    nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
    if (!obs || NS_FAILED(obs->AddObserver(this,
                                           NS_XPCOM_SHUTDOWN_OBSERVER_ID,
                                           false))) {
      NS_WARNING("Failed to add shutdown observer!");
      return false;
    }

    return true;
  }

  bool Shutdown()
  {
    nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
    if (!obs ||
        (NS_FAILED(obs->RemoveObserver(this, NS_XPCOM_SHUTDOWN_OBSERVER_ID)))) {
      NS_WARNING("Can't unregister observers, or already unregistered!");
      return false;
    }
    return true;
  }

  ~BluetoothOppManagerObserver()
  {
    Shutdown();
  }
};

namespace {
// Sending system message "bluetooth-opp-update-progress" every 50kb
static const uint32_t kUpdateProgressBase = 50 * 1024;

/*
 * The format of the header of an PUT request is
 * [opcode:1][packet length:2][headerId:1][header length:2]
 */
static const uint32_t kPutRequestHeaderSize = 6;

StaticAutoPtr<BluetoothOppManager> sInstance;
StaticRefPtr<BluetoothOppManagerObserver> sOppObserver;

/*
 * FIXME / Bug 806749
 *
 * Currently Bluetooth*Manager inherits mozilla::ipc::UnixSocketConsumer,
 * which means that each Bluetooth*Manager can handle only one socket
 * connection at a time. We need to support concurrent multiple socket
 * connections, and then we will be able to have multiple file transferring
 * sessions at a time.
 */
static uint32_t sSentFileLength = 0;
static nsString sFileName;
static uint32_t sFileLength = 0;
static nsString sContentType;
static bool sInShutdown = false;
static bool sWaitingToSendPutFinal = false;
}

NS_IMETHODIMP
BluetoothOppManagerObserver::Observe(nsISupports* aSubject,
                                     const char* aTopic,
                                     const PRUnichar* aData)
{
  MOZ_ASSERT(sInstance);

  if (!strcmp(aTopic, NS_XPCOM_SHUTDOWN_OBSERVER_ID)) {
    return sInstance->HandleShutdown();
  }

  MOZ_ASSERT(false, "BluetoothOppManager got unexpected topic!");
  return NS_ERROR_UNEXPECTED;
}

class SendSocketDataTask : public nsRunnable
{
public:
  SendSocketDataTask(uint8_t* aStream, uint32_t aSize)
    : mStream(aStream)
    , mSize(aSize)
  {
    MOZ_ASSERT(!NS_IsMainThread());
  }

  NS_IMETHOD Run()
  {
    MOZ_ASSERT(NS_IsMainThread());

    sInstance->SendPutRequest(mStream, mSize);
    sSentFileLength += mSize;

    return NS_OK;
  }

private:
  nsAutoArrayPtr<uint8_t> mStream;
  uint32_t mSize;
};


class ReadFileTask : public nsRunnable
{
public:
  ReadFileTask(nsIInputStream* aInputStream,
               uint32_t aRemoteMaxPacketSize) : mInputStream(aInputStream)
  {
    MOZ_ASSERT(NS_IsMainThread());

    mAvailablePacketSize = aRemoteMaxPacketSize - kPutRequestHeaderSize;
  }

  NS_IMETHOD Run()
  {
    MOZ_ASSERT(!NS_IsMainThread());

    uint32_t numRead;
    nsAutoArrayPtr<char> buf(new char[mAvailablePacketSize]);

    // function inputstream->Read() only works on non-main thread
    nsresult rv = mInputStream->Read(buf.get(), mAvailablePacketSize, &numRead);
    if (NS_FAILED(rv)) {
      // Needs error handling here
      NS_WARNING("Failed to read from input stream");
      return NS_ERROR_FAILURE;
    }

    if (numRead > 0) {
      if (sSentFileLength + numRead >= sFileLength) {
        sWaitingToSendPutFinal = true;
      }

      nsRefPtr<SendSocketDataTask> task =
        new SendSocketDataTask((uint8_t*)buf.forget(), numRead);
      if (NS_FAILED(NS_DispatchToMainThread(task))) {
        NS_WARNING("Failed to dispatch to main thread!");
        return NS_ERROR_FAILURE;
      }
    }

    return NS_OK;
  };

private:
  nsCOMPtr<nsIInputStream> mInputStream;
  uint32_t mAvailablePacketSize;
};

class CloseSocketTask : public Task
{
public:
  CloseSocketTask(BluetoothSocket* aSocket) : mSocket(aSocket)
  {
    MOZ_ASSERT(aSocket);
  }

  void Run() MOZ_OVERRIDE
  {
    MOZ_ASSERT(NS_IsMainThread());

    if (mSocket->GetConnectionStatus() ==
        SocketConnectionStatus::SOCKET_CONNECTED) {
      mSocket->Disconnect();
    }
  }

private:
  nsRefPtr<BluetoothSocket> mSocket;
};

BluetoothOppManager::BluetoothOppManager() : mConnected(false)
                                           , mRemoteObexVersion(0)
                                           , mRemoteConnectionFlags(0)
                                           , mRemoteMaxPacketLength(0)
                                           , mLastCommand(0)
                                           , mPacketLeftLength(0)
                                           , mBodySegmentLength(0)
                                           , mReceivedDataBufferOffset(0)
                                           , mAbortFlag(false)
                                           , mNewFileFlag(false)
                                           , mPutFinalFlag(false)
                                           , mSendTransferCompleteFlag(false)
                                           , mSuccessFlag(false)
                                           , mWaitingForConfirmationFlag(false)
{
  mConnectedDeviceAddress.AssignLiteral(BLUETOOTH_ADDRESS_NONE);
  Listen();
}

BluetoothOppManager::~BluetoothOppManager()
{
}

//static
BluetoothOppManager*
BluetoothOppManager::Get()
{
  MOZ_ASSERT(NS_IsMainThread());

  if (!sInstance) {
    sInstance = new BluetoothOppManager();
  }

  return sInstance;
}

bool
BluetoothOppManager::Connect(const nsAString& aDeviceObjectPath,
                             BluetoothReplyRunnable* aRunnable)
{
  MOZ_ASSERT(NS_IsMainThread());

  if (mSocket) {
    NS_WARNING("BluetoothOppManager has been already connected");
    return false;
  }

  // Stop listening because currently we only support one connection at a time.
  if (mRfcommSocket) {
    mRfcommSocket->Disconnect();
    mRfcommSocket = nullptr;
  }

  if (mL2capSocket) {
    mL2capSocket->Disconnect();
    mL2capSocket = nullptr;
  }

  BluetoothService* bs = BluetoothService::Get();
  if (!bs) {
    NS_WARNING("BluetoothService not available!");
    return false;
  }

  nsString uuid;
  BluetoothUuidHelper::GetString(BluetoothServiceClass::OBJECT_PUSH, uuid);

  mRunnable = aRunnable;
  mSocket =
    new BluetoothSocket(this, BluetoothSocketType::RFCOMM, true, true);

  nsresult rv = bs->GetSocketViaService(aDeviceObjectPath,
                                        uuid,
                                        BluetoothSocketType::RFCOMM,
                                        true,
                                        true,
                                        mSocket,
                                        mRunnable);

  return NS_FAILED(rv) ? false : true;
}

void
BluetoothOppManager::Disconnect()
{
  if (mSocket) {
    mSocket->Disconnect();
    mSocket = nullptr;
  }
}

nsresult
BluetoothOppManager::HandleShutdown()
{
  MOZ_ASSERT(NS_IsMainThread());
  sInShutdown = true;
  Disconnect();
  sInstance = nullptr;
  return NS_OK;
}

bool
BluetoothOppManager::Listen()
{
  MOZ_ASSERT(NS_IsMainThread());

  if (mSocket) {
    NS_WARNING("mSocket exists. Failed to listen.");
    return false;
  }

  if (!mRfcommSocket) {
    mRfcommSocket =
      new BluetoothSocket(this, BluetoothSocketType::RFCOMM, true, true);

    if (!mRfcommSocket->Listen(BluetoothReservedChannels::CHANNEL_OPUSH)) {
      NS_WARNING("[OPP] Can't listen on RFCOMM socket!");
      mRfcommSocket = nullptr;
      return false;
    }
  }

  if (!mL2capSocket) {
    mL2capSocket =
      new BluetoothSocket(this, BluetoothSocketType::EL2CAP, true, true);

    if (!mL2capSocket->Listen(BluetoothReservedChannels::CHANNEL_OPUSH_L2CAP)) {
      NS_WARNING("[OPP] Can't listen on L2CAP socket!");
      mRfcommSocket->Disconnect();
      mRfcommSocket = nullptr;
      mL2capSocket = nullptr;
      return false;
    }
  }

  return true;
}

bool
BluetoothOppManager::SendFile(BlobParent* aActor)
{
  if (mBlob) {
    // Means there's a sending process. Reply error.
    return false;
  }

  /*
   * Process of sending a file:
   *  - Keep blob because OPP connection has not been established yet.
   *  - Try to retrieve file name from the blob or assign one if failed to get.
   *  - Create an OPP connection by SendConnectRequest()
   *  - After receiving the response, start to read file and send.
   */
  mBlob = aActor->GetBlob();

  sFileName.Truncate();

  nsCOMPtr<nsIDOMFile> file = do_QueryInterface(mBlob);
  if (file) {
    file->GetName(sFileName);
  }

  /**
   * We try our best to get the file extention to avoid interoperability issues.
   * However, once we found that we are unable to get suitable extension or
   * information about the content type, sending a pre-defined file name without
   * extension would be fine.
   */
  if (sFileName.IsEmpty()) {
    sFileName.AssignLiteral("Unknown");
  }

  int32_t offset = sFileName.RFindChar('/');
  if (offset != kNotFound) {
    sFileName = Substring(sFileName, offset + 1);
  }

  offset = sFileName.RFindChar('.');
  if (offset == kNotFound) {
    nsCOMPtr<nsIMIMEService> mimeSvc = do_GetService(NS_MIMESERVICE_CONTRACTID);

    if (mimeSvc) {
      nsString mimeType;
      mBlob->GetType(mimeType);

      nsCString extension;
      nsresult rv =
        mimeSvc->GetPrimaryExtension(NS_LossyConvertUTF16toASCII(mimeType),
                                     EmptyCString(),
                                     extension);
      if (NS_SUCCEEDED(rv)) {
        sFileName.AppendLiteral(".");
        AppendUTF8toUTF16(extension, sFileName);
      }
    }
  }

  SendConnectRequest();
  mIsServer = false;
  StartFileTransfer();

  return true;
}

bool
BluetoothOppManager::StopSendingFile()
{
  mAbortFlag = true;

  return true;
}

bool
BluetoothOppManager::ConfirmReceivingFile(bool aConfirm)
{
  if (!mConnected) return false;

  if (!mWaitingForConfirmationFlag) {
    NS_WARNING("We are not waiting for a confirmation now.");
    return false;
  }
  mWaitingForConfirmationFlag = false;

  NS_ASSERTION(mPacketLeftLength == 0,
               "Should not be in the middle of receiving a PUT packet.");

  // For the first packet of first file
  bool success = false;
  if (aConfirm) {
    StartFileTransfer();
    if (CreateFile()) {
      success = WriteToFile(mBodySegment.get(), mBodySegmentLength);
    }
  }

  if (success && mPutFinalFlag) {
    mSuccessFlag = true;
    FileTransferComplete();
  }

  ReplyToPut(mPutFinalFlag, success);
  return true;
}

void
BluetoothOppManager::AfterFirstPut()
{
  mUpdateProgressCounter = 1;
  mPutFinalFlag = false;
  mReceivedDataBufferOffset = 0;
  mSendTransferCompleteFlag = false;
  sSentFileLength = 0;
  sWaitingToSendPutFinal = false;
  mSuccessFlag = false;
}

void
BluetoothOppManager::AfterOppConnected()
{
  MOZ_ASSERT(NS_IsMainThread());

  mConnected = true;
  mAbortFlag = false;
  mWaitingForConfirmationFlag = true;
  AfterFirstPut();
}

void
BluetoothOppManager::AfterOppDisconnected()
{
  MOZ_ASSERT(NS_IsMainThread());

  mConnected = false;
  mLastCommand = 0;
  mBlob = nullptr;
  mPacketLeftLength = 0;

  // We can't reset mSuccessFlag here since this function may be called
  // before we send system message of transfer complete
  // mSuccessFlag = false;

  if (mInputStream) {
    mInputStream->Close();
    mInputStream = nullptr;
  }

  if (mOutputStream) {
    mOutputStream->Close();
    mOutputStream = nullptr;
  }

  if (mReadFileThread) {
    mReadFileThread->Shutdown();
    mReadFileThread = nullptr;
  }
}

void
BluetoothOppManager::DeleteReceivedFile()
{
  nsString path;
  path.AssignLiteral(TARGET_FOLDER);

  nsCOMPtr<nsIFile> f;
  nsresult rv = NS_NewLocalFile(path + sFileName, false, getter_AddRefs(f));
  if (NS_FAILED(rv)) {
    NS_WARNING("Couldn't find received file, nothing to delete.");
    return;
  }

  if (mOutputStream) {
    mOutputStream->Close();
    mOutputStream = nullptr;
  }

  f->Remove(false);
}

DeviceStorageFile*
BluetoothOppManager::CreateDeviceStorageFile(nsIFile* aFile)
{
  nsString fullFilePath;
  aFile->GetPath(fullFilePath);

  MOZ_ASSERT(StringBeginsWith(fullFilePath, NS_LITERAL_STRING(TARGET_ROOT)));

  nsDependentSubstring storagePath =
    Substring(fullFilePath, strlen(TARGET_ROOT));

  nsCOMPtr<nsIMIMEService> mimeSvc = do_GetService(NS_MIMESERVICE_CONTRACTID);
  NS_ENSURE_TRUE(mimeSvc, nullptr);

  nsCString mimeType;
  nsresult rv = mimeSvc->GetTypeFromFile(aFile, mimeType);
  if (NS_FAILED(rv)) {
    return nullptr;
  }

  if (StringBeginsWith(mimeType, NS_LITERAL_CSTRING("image/"))) {
    return new DeviceStorageFile(NS_LITERAL_STRING("pictures"), storagePath);
  } else if (StringBeginsWith(mimeType, NS_LITERAL_CSTRING("video/"))) {
    return new DeviceStorageFile(NS_LITERAL_STRING("videos"), storagePath);
  } else if (StringBeginsWith(mimeType, NS_LITERAL_CSTRING("audio/"))) {
    return new DeviceStorageFile(NS_LITERAL_STRING("music"), storagePath);
  } else {
    NS_WARNING("Couldn't recognize the mimetype of received file.");
    return nullptr;
  }
}

bool
BluetoothOppManager::CreateFile()
{
  nsString path;
  path.AssignLiteral(TARGET_FOLDER);

  MOZ_ASSERT(mPacketLeftLength == 0);

  nsCOMPtr<nsIFile> f;
  nsresult rv;
  rv = NS_NewLocalFile(path + sFileName, false, getter_AddRefs(f));
  if (NS_FAILED(rv)) {
    NS_WARNING("Couldn't new a local file");
    return false;
  }

  rv = f->CreateUnique(nsIFile::NORMAL_FILE_TYPE, 00644);
  if (NS_FAILED(rv)) {
    NS_WARNING("Couldn't create the file");
    return false;
  }

  /*
   * The function CreateUnique() may create a file with a different file
   * name from the original sFileName. Therefore we have to retrieve
   * the file name again.
   */
  f->GetLeafName(sFileName);
  mDsFile = CreateDeviceStorageFile(f);

  NS_NewLocalFileOutputStream(getter_AddRefs(mOutputStream), f);
  NS_ENSURE_TRUE(mOutputStream, false);

  return true;
}

bool
BluetoothOppManager::WriteToFile(const uint8_t* aData, int aDataLength)
{
  if (!mOutputStream) {
    NS_WARNING("No available output stream");
    return false;
  }

  uint32_t wrote = 0;
  mOutputStream->Write((const char*)aData, aDataLength, &wrote);
  if (aDataLength != wrote) {
    NS_WARNING("Writing to the file failed");
    return false;
  }

  return true;
}

// Virtual function of class SocketConsumer
void
BluetoothOppManager::ExtractPacketHeaders(const ObexHeaderSet& aHeader)
{
  if (aHeader.Has(ObexHeaderId::Name)) {
    aHeader.GetName(sFileName);
  }

  if (aHeader.Has(ObexHeaderId::Type)) {
    aHeader.GetContentType(sContentType);
  }

  if (aHeader.Has(ObexHeaderId::Length)) {
    aHeader.GetLength(&sFileLength);
  }

  if (aHeader.Has(ObexHeaderId::Body) ||
      aHeader.Has(ObexHeaderId::EndOfBody)) {
    uint8_t* bodyPtr;
    aHeader.GetBody(&bodyPtr);
    mBodySegment = bodyPtr;

    aHeader.GetBodyLength(&mBodySegmentLength);
  }
}

bool
BluetoothOppManager::ExtractBlobHeaders()
{
  nsresult rv = mBlob->GetType(sContentType);
  if (NS_FAILED(rv)) {
    NS_WARNING("Can't get content type");
    SendDisconnectRequest();
    return false;
  }

  uint64_t fileLength;
  rv = mBlob->GetSize(&fileLength);
  if (NS_FAILED(rv)) {
    NS_WARNING("Can't get file size");
    SendDisconnectRequest();
    return false;
  }

  // Currently we keep the size of files which were sent/received via
  // Bluetooth not exceed UINT32_MAX because the Length header in OBEX
  // is only 4-byte long. Although it is possible to transfer a file
  // larger than UINT32_MAX, it needs to parse another OBEX Header
  // and I would like to leave it as a feature.
  if (fileLength > (uint64_t)UINT32_MAX) {
    NS_WARNING("The file size is too large for now");
    SendDisconnectRequest();
    return false;
  }

  sFileLength = fileLength;
  rv = NS_NewThread(getter_AddRefs(mReadFileThread));
  if (NS_FAILED(rv)) {
    NS_WARNING("Can't create thread");
    SendDisconnectRequest();
    return false;
  }

  return true;
}

bool
BluetoothOppManager::IsReservedChar(PRUnichar c)
{
  return (c < 0x0020 ||
          c == PRUnichar('?') || c == PRUnichar('|') || c == PRUnichar('<') ||
          c == PRUnichar('>') || c == PRUnichar('"') || c == PRUnichar(':') ||
          c == PRUnichar('/') || c == PRUnichar('*') || c == PRUnichar('\\'));
}

void
BluetoothOppManager::ValidateFileName()
{
  int length = sFileName.Length();

  for (int i = 0; i < length; ++i) {
    // Replace reserved char of fat file system with '_'
    if (IsReservedChar(sFileName.CharAt(i))) {
      sFileName.Replace(i, 1, PRUnichar('_'));
    }
  }
}

void
BluetoothOppManager::ServerDataHandler(UnixSocketRawData* aMessage)
{
  uint8_t opCode;
  int packetLength;
  int receivedLength = aMessage->mSize;

  if (mPacketLeftLength > 0) {
    opCode = mPutFinalFlag ? ObexRequestCode::PutFinal : ObexRequestCode::Put;
    packetLength = mPacketLeftLength;
  } else {
    opCode = aMessage->mData[0];
    packetLength = (((int)aMessage->mData[1]) << 8) | aMessage->mData[2];

    // When there's a Put packet right after a PutFinal packet,
    // which means it's the start point of a new file.
    if (mPutFinalFlag &&
        (opCode == ObexRequestCode::Put ||
         opCode == ObexRequestCode::PutFinal)) {
      mNewFileFlag = true;
      AfterFirstPut();
    }
  }

  ObexHeaderSet pktHeaders(opCode);
  if (opCode == ObexRequestCode::Connect) {
    // Section 3.3.1 "Connect", IrOBEX 1.2
    // [opcode:1][length:2][version:1][flags:1][MaxPktSizeWeCanReceive:2]
    // [Headers:var]
    ParseHeaders(&aMessage->mData[7],
                 receivedLength - 7,
                 &pktHeaders);
    ReplyToConnect();
    AfterOppConnected();
    mIsServer = true;
  } else if (opCode == ObexRequestCode::Disconnect ||
             opCode == ObexRequestCode::Abort) {
    // Section 3.3.2 "Disconnect", IrOBEX 1.2
    // Section 3.3.5 "Abort", IrOBEX 1.2
    // [opcode:1][length:2][Headers:var]
    ParseHeaders(&aMessage->mData[3],
                receivedLength - 3,
                &pktHeaders);
    ReplyToDisconnect();
    AfterOppDisconnected();
    if (opCode == ObexRequestCode::Abort) {
      DeleteReceivedFile();
    }
    FileTransferComplete();
  } else if (opCode == ObexRequestCode::Put ||
             opCode == ObexRequestCode::PutFinal) {
    int headerStartIndex = 0;

    // The first part of each Put packet
    if (mReceivedDataBufferOffset == 0) {
      // Section 3.3.3 "Put", IrOBEX 1.2
      // [opcode:1][length:2][Headers:var]
      headerStartIndex = 3;
      // The largest buffer size is 65535 because packetLength is a
      // 2-byte value (0 ~ 0xFFFF).
      mReceivedDataBuffer = new uint8_t[packetLength];
      mPacketLeftLength = packetLength;

      /*
       * A PUT request from remote devices may be divided into multiple parts.
       * In other words, one request may need to be received multiple times,
       * so here we keep a variable mPacketLeftLength to indicate if current
       * PUT request is done.
       */
      mPutFinalFlag = (opCode == ObexRequestCode::PutFinal);
    }

    memcpy(mReceivedDataBuffer.get() + mReceivedDataBufferOffset,
           &aMessage->mData[headerStartIndex],
           receivedLength - headerStartIndex);

    mPacketLeftLength -= receivedLength;
    mReceivedDataBufferOffset += receivedLength - headerStartIndex;
    if (mPacketLeftLength) {
      return;
    }

    // A Put packet is received completely
    ParseHeaders(mReceivedDataBuffer.get(),
                 mReceivedDataBufferOffset,
                 &pktHeaders);
    ExtractPacketHeaders(pktHeaders);
    ValidateFileName();

    mReceivedDataBufferOffset = 0;

    // When we cancel the transfer, delete the file and notify complemention
    if (mAbortFlag) {
      ReplyToPut(mPutFinalFlag, !mAbortFlag);
      sSentFileLength += mBodySegmentLength;
      DeleteReceivedFile();
      FileTransferComplete();
      return;
    }

    // Wait until get confirmation from user, then create file and write to it
    if (mWaitingForConfirmationFlag) {
      ReceivingFileConfirmation();
      sSentFileLength += mBodySegmentLength;
      return;
    }

    // Already get confirmation from user, create a new file if needed and
    // write to output stream
    if (mNewFileFlag) {
      StartFileTransfer();
      if (!CreateFile()) {
        ReplyToPut(mPutFinalFlag, false);
        return;
      }
      mNewFileFlag = false;
    }

    if (!WriteToFile(mBodySegment.get(), mBodySegmentLength)) {
      ReplyToPut(mPutFinalFlag, false);
      return;
    }

    ReplyToPut(mPutFinalFlag, true);

    // Send progress update
    sSentFileLength += mBodySegmentLength;
    if (sSentFileLength > kUpdateProgressBase * mUpdateProgressCounter) {
      UpdateProgress();
      mUpdateProgressCounter = sSentFileLength / kUpdateProgressBase + 1;
    }

    // Success to receive a file and notify completion
    if (mPutFinalFlag) {
      mSuccessFlag = true;
      FileTransferComplete();
      NotifyAboutFileChange();
    }
  } else {
    NS_WARNING("Unhandled ObexRequestCode");
  }
}

void
BluetoothOppManager::ClientDataHandler(UnixSocketRawData* aMessage)
{
  uint8_t opCode;
  int packetLength;

  if (mPacketLeftLength > 0) {
    opCode = mPutFinalFlag ? ObexRequestCode::PutFinal : ObexRequestCode::Put;
    packetLength = mPacketLeftLength;
  } else {
    opCode = aMessage->mData[0];
    packetLength = (((int)aMessage->mData[1]) << 8) | aMessage->mData[2];
  }

  // Check response code and send out system message as finished if the reponse
  // code is somehow incorrect.
  uint8_t expectedOpCode = ObexResponseCode::Success;
  if (mLastCommand == ObexRequestCode::Put) {
    expectedOpCode = ObexResponseCode::Continue;
  }

  if (opCode != expectedOpCode) {
    if (mLastCommand == ObexRequestCode::Put ||
        mLastCommand == ObexRequestCode::Abort ||
        mLastCommand == ObexRequestCode::PutFinal) {
      SendDisconnectRequest();
    }
    nsAutoCString str;
    str += "[OPP] 0x";
    str.AppendInt(mLastCommand, 16);
    str += " failed";
    NS_WARNING(str.get());
    FileTransferComplete();
    return;
  }

  if (mLastCommand == ObexRequestCode::PutFinal) {
    mSuccessFlag = true;
    FileTransferComplete();
    SendDisconnectRequest();
  } else if (mLastCommand == ObexRequestCode::Abort) {
    SendDisconnectRequest();
    FileTransferComplete();
  } else if (mLastCommand == ObexRequestCode::Disconnect) {
    AfterOppDisconnected();
    // Most devices will directly terminate connection after receiving
    // Disconnect request, so we make a delay here. If the socket hasn't been
    // disconnected, we will close it.
    if (mSocket) {
      MessageLoop::current()->
        PostDelayedTask(FROM_HERE, new CloseSocketTask(mSocket), 1000);
    }
  } else if (mLastCommand == ObexRequestCode::Connect) {
    MOZ_ASSERT(!sFileName.IsEmpty());
    MOZ_ASSERT(mBlob);

    AfterOppConnected();

    // Keep remote information
    mRemoteObexVersion = aMessage->mData[3];
    mRemoteConnectionFlags = aMessage->mData[4];
    mRemoteMaxPacketLength =
      (((int)(aMessage->mData[5]) << 8) | aMessage->mData[6]);

    /*
     * Before sending content, we have to send a header including
     * information such as file name, file length and content type.
     */
    if (ExtractBlobHeaders()) {
      sInstance->SendPutHeaderRequest(sFileName, sFileLength);
    }
  } else if (mLastCommand == ObexRequestCode::Put) {

    // Send PutFinal packet when we get response
    if (sWaitingToSendPutFinal) {
      SendPutFinalRequest();
      return;
    }

    if (mAbortFlag) {
      SendAbortRequest();
      return;
    }

    if (kUpdateProgressBase * mUpdateProgressCounter < sSentFileLength) {
      UpdateProgress();
      mUpdateProgressCounter = sSentFileLength / kUpdateProgressBase + 1;
    }

    nsresult rv;
    if (!mInputStream) {
      rv = mBlob->GetInternalStream(getter_AddRefs(mInputStream));
      if (NS_FAILED(rv)) {
        NS_WARNING("Can't get internal stream of blob");
        SendDisconnectRequest();
        return;
      }
    }

    nsRefPtr<ReadFileTask> task = new ReadFileTask(mInputStream,
                                                   mRemoteMaxPacketLength);
    rv = mReadFileThread->Dispatch(task, NS_DISPATCH_NORMAL);
    if (NS_FAILED(rv)) {
      NS_WARNING("Cannot dispatch read file task!");
      SendDisconnectRequest();
    }
  } else {
    NS_WARNING("Unhandled ObexRequestCode");
  }
}

// Virtual function of class SocketConsumer
void
BluetoothOppManager::ReceiveSocketData(BluetoothSocket* aSocket,
                                       nsAutoPtr<UnixSocketRawData>& aMessage)
{
  if (mLastCommand) {
    ClientDataHandler(aMessage);
    return;
  }

  ServerDataHandler(aMessage);
}

void
BluetoothOppManager::SendConnectRequest()
{
  if (mConnected) return;

  // Section 3.3.1 "Connect", IrOBEX 1.2
  // [opcode:1][length:2][version:1][flags:1][MaxPktSizeWeCanReceive:2]
  // [Headers:var]
  uint8_t req[255];
  int index = 7;

  req[3] = 0x10; // version=1.0
  req[4] = 0x00; // flag=0x00
  req[5] = BluetoothOppManager::MAX_PACKET_LENGTH >> 8;
  req[6] = (uint8_t)BluetoothOppManager::MAX_PACKET_LENGTH;

  SetObexPacketInfo(req, ObexRequestCode::Connect, index);
  mLastCommand = ObexRequestCode::Connect;

  UnixSocketRawData* s = new UnixSocketRawData(index);
  memcpy(s->mData, req, s->mSize);
  mSocket->SendSocketData(s);
}

void
BluetoothOppManager::SendPutHeaderRequest(const nsAString& aFileName,
                                          int aFileSize)
{
  uint8_t* req = new uint8_t[mRemoteMaxPacketLength];

  int len = aFileName.Length();
  uint8_t* fileName = new uint8_t[(len + 1) * 2];
  const PRUnichar* fileNamePtr = aFileName.BeginReading();

  for (int i = 0; i < len; i++) {
    fileName[i * 2] = (uint8_t)(fileNamePtr[i] >> 8);
    fileName[i * 2 + 1] = (uint8_t)fileNamePtr[i];
  }

  fileName[len * 2] = 0x00;
  fileName[len * 2 + 1] = 0x00;

  int index = 3;
  index += AppendHeaderName(&req[index], (char*)fileName, (len + 1) * 2);
  index += AppendHeaderLength(&req[index], aFileSize);

  SetObexPacketInfo(req, ObexRequestCode::Put, index);
  mLastCommand = ObexRequestCode::Put;

  UnixSocketRawData* s = new UnixSocketRawData(index);
  memcpy(s->mData, req, s->mSize);
  mSocket->SendSocketData(s);

  delete [] fileName;
  delete [] req;
}

void
BluetoothOppManager::SendPutRequest(uint8_t* aFileBody,
                                    int aFileBodyLength)
{
  int packetLeftSpace = mRemoteMaxPacketLength - kPutRequestHeaderSize;

  if (!mConnected) return;
  if (aFileBodyLength > packetLeftSpace) {
    NS_WARNING("Not allowed such a small MaxPacketLength value");
    return;
  }

  // Section 3.3.3 "Put", IrOBEX 1.2
  // [opcode:1][length:2][Headers:var]
  uint8_t* req = new uint8_t[mRemoteMaxPacketLength];

  int index = 3;
  index += AppendHeaderBody(&req[index], aFileBody, aFileBodyLength);

  SetObexPacketInfo(req, ObexRequestCode::Put, index);
  mLastCommand = ObexRequestCode::Put;

  UnixSocketRawData* s = new UnixSocketRawData(index);
  memcpy(s->mData, req, s->mSize);
  mSocket->SendSocketData(s);

  delete [] req;
}

void
BluetoothOppManager::SendPutFinalRequest()
{
  if (!mConnected) return;

  /**
   * Section 2.2.9, "End-of-Body", IrObex 1.2
   * End-of-Body is used to identify the last chunk of the object body.
   * For most platforms, a PutFinal packet is sent with an zero length
   * End-of-Body header.
   */

  // [opcode:1][length:2]
  int index = 3;
  uint8_t* req = new uint8_t[mRemoteMaxPacketLength];
  index += AppendHeaderEndOfBody(&req[index]);
  SetObexPacketInfo(req, ObexRequestCode::PutFinal, index);
  mLastCommand = ObexRequestCode::PutFinal;

  UnixSocketRawData* s = new UnixSocketRawData(index);
  memcpy(s->mData, req, s->mSize);
  mSocket->SendSocketData(s);

  sWaitingToSendPutFinal = false;

  delete [] req;
}

void
BluetoothOppManager::SendDisconnectRequest()
{
  // Section 3.3.2 "Disconnect", IrOBEX 1.2
  // [opcode:1][length:2][Headers:var]
  uint8_t req[255];
  int index = 3;

  SetObexPacketInfo(req, ObexRequestCode::Disconnect, index);
  mLastCommand = ObexRequestCode::Disconnect;

  UnixSocketRawData* s = new UnixSocketRawData(index);
  memcpy(s->mData, req, s->mSize);
  mSocket->SendSocketData(s);
}

void
BluetoothOppManager::SendAbortRequest()
{
  // Section 3.3.5 "Abort", IrOBEX 1.2
  // [opcode:1][length:2][Headers:var]
  uint8_t req[255];
  int index = 3;

  SetObexPacketInfo(req, ObexRequestCode::Abort, index);
  mLastCommand = ObexRequestCode::Abort;

  UnixSocketRawData* s = new UnixSocketRawData(index);
  memcpy(s->mData, req, s->mSize);
  mSocket->SendSocketData(s);
}

bool
BluetoothOppManager::IsTransferring()
{
  return (mConnected && !mSendTransferCompleteFlag);
}

void
BluetoothOppManager::ReplyToConnect()
{
  if (mConnected) return;
  mConnected = true;

  // Section 3.3.1 "Connect", IrOBEX 1.2
  // [opcode:1][length:2][version:1][flags:1][MaxPktSizeWeCanReceive:2]
  // [Headers:var]
  uint8_t req[255];
  int index = 7;

  req[3] = 0x10; // version=1.0
  req[4] = 0x00; // flag=0x00
  req[5] = BluetoothOppManager::MAX_PACKET_LENGTH >> 8;
  req[6] = (uint8_t)BluetoothOppManager::MAX_PACKET_LENGTH;

  SetObexPacketInfo(req, ObexResponseCode::Success, index);

  UnixSocketRawData* s = new UnixSocketRawData(index);
  memcpy(s->mData, req, s->mSize);
  mSocket->SendSocketData(s);
}

void
BluetoothOppManager::ReplyToDisconnect()
{
  if (!mConnected) return;
  mConnected = false;

  // Section 3.3.2 "Disconnect", IrOBEX 1.2
  // [opcode:1][length:2][Headers:var]
  uint8_t req[255];
  int index = 3;

  SetObexPacketInfo(req, ObexResponseCode::Success, index);

  UnixSocketRawData* s = new UnixSocketRawData(index);
  memcpy(s->mData, req, s->mSize);
  mSocket->SendSocketData(s);
}

void
BluetoothOppManager::ReplyToPut(bool aFinal, bool aContinue)
{
  if (!mConnected) return;

  // Section 3.3.2 "Disconnect", IrOBEX 1.2
  // [opcode:1][length:2][Headers:var]
  uint8_t req[255];
  int index = 3;

  if (aContinue) {
    if (aFinal) {
      SetObexPacketInfo(req, ObexResponseCode::Success, index);
    } else {
      SetObexPacketInfo(req, ObexResponseCode::Continue, index);
    }
  } else {
    if (aFinal) {
      SetObexPacketInfo(req, ObexResponseCode::Unauthorized, index);
    } else {
      SetObexPacketInfo(req,
                        ObexResponseCode::Unauthorized & (~FINAL_BIT),
                        index);
    }
  }

  UnixSocketRawData* s = new UnixSocketRawData(index);
  memcpy(s->mData, req, s->mSize);
  mSocket->SendSocketData(s);
}

void
BluetoothOppManager::FileTransferComplete()
{
  if (mSendTransferCompleteFlag) {
    return;
  }

  nsString type, name;
  BluetoothValue v;
  InfallibleTArray<BluetoothNamedValue> parameters;
  type.AssignLiteral("bluetooth-opp-transfer-complete");

  name.AssignLiteral("address");
  v = mConnectedDeviceAddress;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  name.AssignLiteral("success");
  v = mSuccessFlag;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  name.AssignLiteral("received");
  v = mIsServer;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  name.AssignLiteral("fileName");
  v = sFileName;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  name.AssignLiteral("fileLength");
  v = sSentFileLength;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  name.AssignLiteral("contentType");
  v = sContentType;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  if (!BroadcastSystemMessage(type, parameters)) {
    NS_WARNING("Failed to broadcast [bluetooth-opp-transfer-complete]");
    return;
  }

  mSendTransferCompleteFlag = true;
}

void
BluetoothOppManager::StartFileTransfer()
{
  nsString type, name;
  BluetoothValue v;
  InfallibleTArray<BluetoothNamedValue> parameters;
  type.AssignLiteral("bluetooth-opp-transfer-start");

  name.AssignLiteral("address");
  v = mConnectedDeviceAddress;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  name.AssignLiteral("received");
  v = mIsServer;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  name.AssignLiteral("fileName");
  v = sFileName;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  name.AssignLiteral("fileLength");
  v = sFileLength;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  name.AssignLiteral("contentType");
  v = sContentType;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  if (!BroadcastSystemMessage(type, parameters)) {
    NS_WARNING("Failed to broadcast [bluetooth-opp-transfer-start]");
    return;
  }
}

void
BluetoothOppManager::UpdateProgress()
{
  nsString type, name;
  BluetoothValue v;
  InfallibleTArray<BluetoothNamedValue> parameters;
  type.AssignLiteral("bluetooth-opp-update-progress");

  name.AssignLiteral("address");
  v = mConnectedDeviceAddress;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  name.AssignLiteral("received");
  v = mIsServer;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  name.AssignLiteral("processedLength");
  v = sSentFileLength;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  name.AssignLiteral("fileLength");
  v = sFileLength;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  if (!BroadcastSystemMessage(type, parameters)) {
    NS_WARNING("Failed to broadcast [bluetooth-opp-update-progress]");
    return;
  }
}

void
BluetoothOppManager::ReceivingFileConfirmation()
{
  nsString type, name;
  BluetoothValue v;
  InfallibleTArray<BluetoothNamedValue> parameters;
  type.AssignLiteral("bluetooth-opp-receiving-file-confirmation");

  name.AssignLiteral("address");
  v = mConnectedDeviceAddress;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  name.AssignLiteral("fileName");
  v = sFileName;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  name.AssignLiteral("fileLength");
  v = sFileLength;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  name.AssignLiteral("contentType");
  v = sContentType;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  if (!BroadcastSystemMessage(type, parameters)) {
    NS_WARNING("Failed to send [bluetooth-opp-receiving-file-confirmation]");
    return;
  }
}

void
BluetoothOppManager::NotifyAboutFileChange()
{
  NS_NAMED_LITERAL_STRING(data, "modified");

  nsCOMPtr<nsIObserverService> obs =
    mozilla::services::GetObserverService();
  NS_ENSURE_TRUE_VOID(obs);

  obs->NotifyObservers(mDsFile, "file-watcher-notify", data.get());
}

void
BluetoothOppManager::OnConnectSuccess(BluetoothSocket* aSocket)
{
  MOZ_ASSERT(aSocket);

  /**
   * If the created connection is an inbound connection, close another server
   * socket because currently only one file-transfer session is allowed. After
   * that, we need to make sure that both server socket would be nulled out.
   * As for outbound connections, we do nothing since sockets have been already
   * handled in function Connect().
   */
  if (aSocket == mRfcommSocket) {
    MOZ_ASSERT(!mSocket);
    mRfcommSocket.swap(mSocket);

    mL2capSocket->Disconnect();
    mL2capSocket = nullptr;
  } else if (aSocket == mL2capSocket) {
    MOZ_ASSERT(!mSocket);
    mL2capSocket.swap(mSocket);

    mRfcommSocket->Disconnect();
    mRfcommSocket = nullptr;
  }

  if (mRunnable) {
    BluetoothReply* reply = new BluetoothReply(BluetoothReplySuccess(true));
    mRunnable->SetReply(reply);
    if (NS_FAILED(NS_DispatchToMainThread(mRunnable))) {
      NS_WARNING("Failed to dispatch to main thread!");
    }
    mRunnable.forget();
  }

  // Cache device address since we can't get socket address when a remote
  // device disconnect with us.
  mSocket->GetAddress(mConnectedDeviceAddress);
}

void
BluetoothOppManager::OnConnectError(BluetoothSocket* aSocket)
{
  if (mRunnable) {
    nsString errorStr;
    errorStr.AssignLiteral("Failed to connect with a bluetooth opp manager!");
    BluetoothReply* reply = new BluetoothReply(BluetoothReplyError(errorStr));
    mRunnable->SetReply(reply);
    if (NS_FAILED(NS_DispatchToMainThread(mRunnable))) {
      NS_WARNING("Failed to dispatch to main thread!");
    }
    mRunnable.forget();
  }

  mSocket = nullptr;
  mRfcommSocket = nullptr;
  mL2capSocket = nullptr;

  Listen();
}

void
BluetoothOppManager::OnDisconnect(BluetoothSocket* aSocket)
{
  MOZ_ASSERT(aSocket);

  if (aSocket != mSocket) {
    // Do nothing when a listening server socket is closed.
    return;
  }

  /**
   * It is valid for a bluetooth device which is transfering file via OPP
   * closing socket without sending OBEX disconnect request first. So we
   * delete the broken file when we failed to receive a file from the remote,
   * and notify the transfer has been completed (but failed). We also call
   * AfterOppDisconnected here to ensure all variables will be cleaned.
   */
  if (!mSuccessFlag) {
    if (mIsServer) {
      DeleteReceivedFile();
    }
    FileTransferComplete();
  }

  AfterOppDisconnected();
  mConnectedDeviceAddress.AssignLiteral(BLUETOOTH_ADDRESS_NONE);
  mSuccessFlag = false;

  mSocket = nullptr;
  Listen();
}
