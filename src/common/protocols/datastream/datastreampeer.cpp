/***************************************************************************
 *   Copyright (C) 2005-2014 by the Quassel Project                        *
 *   devel@quassel-irc.org                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) version 3.                                           *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.         *
 ***************************************************************************/

#include <QtEndian>

#include <QHostAddress>
#include <QTcpSocket>

#include "datastreampeer.h"
#include "quassel.h"

using namespace Protocol;

DataStreamPeer::DataStreamPeer(::AuthHandler *authHandler, QTcpSocket *socket, quint16 features, QObject *parent)
    : RemotePeer(authHandler, socket, parent),
    _blockSize(0)
{
    Q_UNUSED(features);

    _stream.setDevice(socket);
    _stream.setVersion(QDataStream::Qt_4_2);
}


quint16 DataStreamPeer::supportedFeatures()
{
    return 0;
}


bool DataStreamPeer::acceptsFeatures(quint16 peerFeatures)
{
    Q_UNUSED(peerFeatures);
    return true;
}


quint16 DataStreamPeer::enabledFeatures() const
{
    return 0;
}


// Note that we're already preparing for readSocketData() moving into RemotePeer, thus the slightly
// cumbersome type and stream handling here.
void DataStreamPeer::onSocketDataAvailable()
{
    // don't try to read more data if we're already closing
    if (socket()->state() !=  QAbstractSocket::ConnectedState)
        return;

    QByteArray data;
    while (readSocketData(data)) {
        // data contains always a serialized QVector<QVariant>
        QDataStream stream(data);
        stream.setVersion(QDataStream::Qt_4_2);
        QVariantList list;
        stream >> list;
        if (stream.status() != QDataStream::Ok) {
            close("Peer sent corrupt data, closing down!");
            return;
        }

        // if no sigproxy is set, we're in handshake mode
        if (!signalProxy())
            handleHandshakeMessage(list);
        else
            handlePackedFunc(list);
    }
}


bool DataStreamPeer::readSocketData(QByteArray &data)
{
    if (_blockSize == 0) {
        if (socket()->bytesAvailable() < 4)
            return false;
        // the block size is part of QByteArray's serialization format, so we don't actually read it now...
        socket()->peek((char*)&_blockSize, 4);
        _blockSize = qFromBigEndian<quint32>(_blockSize) + 4; // ... but of course we have to add its size to the total size of the block
    }

    if (_blockSize > 1 << 22) {
        close("Peer tried to send package larger than max package size!");
        return false;
    }

    if (_blockSize == 0) {
        close("Peer tried to send 0 byte package!");
        return false;
    }

    if (socket()->bytesAvailable() < _blockSize) {
        emit transferProgress(socket()->bytesAvailable(), _blockSize);
        return false;
    }

    emit transferProgress(_blockSize, _blockSize);

    _stream >> data;
    _blockSize = 0;

    if (_stream.status() != QDataStream::Ok) {
        close("Peer sent corrupt data, closing down!");
        return false;
    }

    return true;
}


void DataStreamPeer::writeSocketData(const QVariantList &list)
{
    if (!socket()->isOpen()) {
        qWarning() << Q_FUNC_INFO << "Can't write to a closed socket!";
        return;
    }

    QByteArray data;
    QDataStream msgStream(&data, QIODevice::WriteOnly);
    msgStream.setVersion(QDataStream::Qt_4_2);
    msgStream << list;

    _stream << data;  // also writes the block size as part of the serialization format
    if (_stream.status() != QDataStream::Ok)
        close("Could not serialize data for peer!");
}


void DataStreamPeer::writeSocketData(const QVariantMap &handshakeMsg)
{
    QVariantList list;
    QVariantMap::const_iterator it = handshakeMsg.begin();
    while (it != handshakeMsg.end()) {
        list << it.key().toUtf8() << it.value();
        ++it;
    }

    writeSocketData(list);
}



/*** Handshake messages ***/

/* These messages are transmitted during handshake phase, which in case of the legacy protocol means they have
 * a structure different from those being used after the handshake.
 * Also, the legacy handshake does not fully match the redesigned one, so we'll have to do various mappings here.
 */

void DataStreamPeer::handleHandshakeMessage(const QVariantList &mapData)
{
    QVariantMap m;
    for (int i = 0; i < mapData.count()/2; ++i)
        m[QString::fromUtf8(mapData[2*i].toByteArray())] = mapData[2*i+1];

    QString msgType = m["MsgType"].toString();
    if (msgType.isEmpty()) {
        emit protocolError(tr("Invalid handshake message!"));
        return;
    }

    if (msgType == "ClientInit") {
        handle(RegisterClient(m["ClientVersion"].toString(), false)); // UseSsl obsolete
    }

    else if (msgType == "ClientInitReject") {
        handle(ClientDenied(m["Error"].toString()));
    }

    else if (msgType == "ClientInitAck") {
        handle(ClientRegistered(m["CoreFeatures"].toUInt(), m["Configured"].toBool(), m["StorageBackends"].toList(), false, QDateTime())); // SupportsSsl and coreStartTime obsolete
    }

    else if (msgType == "CoreSetupData") {
        QVariantMap map = m["SetupData"].toMap();
        handle(SetupData(map["AdminUser"].toString(), map["AdminPasswd"].toString(), map["Backend"].toString(), map["ConnectionProperties"].toMap()));
    }

    else if (msgType == "CoreSetupReject") {
        handle(SetupFailed(m["Error"].toString()));
    }

    else if (msgType == "CoreSetupAck") {
        handle(SetupDone());
    }

    else if (msgType == "ClientLogin") {
        handle(Login(m["User"].toString(), m["Password"].toString()));
    }

    else if (msgType == "ClientLoginReject") {
        handle(LoginFailed(m["Error"].toString()));
    }

    else if (msgType == "ClientLoginAck") {
        handle(LoginSuccess());
    }

    else if (msgType == "SessionInit") {
        QVariantMap map = m["SessionState"].toMap();
        handle(SessionState(map["Identities"].toList(), map["BufferInfos"].toList(), map["NetworkIds"].toList()));
    }

    else {
        emit protocolError(tr("Unknown protocol message of type %1").arg(msgType));
    }
}


void DataStreamPeer::dispatch(const RegisterClient &msg) {
    QVariantMap m;
    m["MsgType"] = "ClientInit";
    m["ClientVersion"] = msg.clientVersion;
    m["ClientDate"] = Quassel::buildInfo().buildDate;

    writeSocketData(m);
}


void DataStreamPeer::dispatch(const ClientDenied &msg) {
    QVariantMap m;
    m["MsgType"] = "ClientInitReject";
    m["Error"] = msg.errorString;

    writeSocketData(m);
}


void DataStreamPeer::dispatch(const ClientRegistered &msg) {
    QVariantMap m;
    m["MsgType"] = "ClientInitAck";
    m["CoreFeatures"] = msg.coreFeatures;
    m["StorageBackends"] = msg.backendInfo;
    m["LoginEnabled"] = m["Configured"] = msg.coreConfigured;

    writeSocketData(m);
}


void DataStreamPeer::dispatch(const SetupData &msg)
{
    QVariantMap map;
    map["AdminUser"] = msg.adminUser;
    map["AdminPasswd"] = msg.adminPassword;
    map["Backend"] = msg.backend;
    map["ConnectionProperties"] = msg.setupData;

    QVariantMap m;
    m["MsgType"] = "CoreSetupData";
    m["SetupData"] = map;
    writeSocketData(m);
}


void DataStreamPeer::dispatch(const SetupFailed &msg)
{
    QVariantMap m;
    m["MsgType"] = "CoreSetupReject";
    m["Error"] = msg.errorString;

    writeSocketData(m);
}


void DataStreamPeer::dispatch(const SetupDone &msg)
{
    Q_UNUSED(msg)

    QVariantMap m;
    m["MsgType"] = "CoreSetupAck";

    writeSocketData(m);
}


void DataStreamPeer::dispatch(const Login &msg)
{
    QVariantMap m;
    m["MsgType"] = "ClientLogin";
    m["User"] = msg.user;
    m["Password"] = msg.password;

    writeSocketData(m);
}


void DataStreamPeer::dispatch(const LoginFailed &msg)
{
    QVariantMap m;
    m["MsgType"] = "ClientLoginReject";
    m["Error"] = msg.errorString;

    writeSocketData(m);
}


void DataStreamPeer::dispatch(const LoginSuccess &msg)
{
    Q_UNUSED(msg)

    QVariantMap m;
    m["MsgType"] = "ClientLoginAck";

    writeSocketData(m);
}


void DataStreamPeer::dispatch(const SessionState &msg)
{
    QVariantMap m;
    m["MsgType"] = "SessionInit";

    QVariantMap map;
    map["BufferInfos"] = msg.bufferInfos;
    map["NetworkIds"] = msg.networkIds;
    map["Identities"] = msg.identities;
    m["SessionState"] = map;

    writeSocketData(m);
}


/*** Standard messages ***/

void DataStreamPeer::handlePackedFunc(const QVariantList &packedFunc)
{
    QVariantList params(packedFunc);

    if (params.isEmpty()) {
        qWarning() << Q_FUNC_INFO << "Received incompatible data:" << packedFunc;
        return;
    }

    // TODO: make sure that this is a valid request type
    RequestType requestType = (RequestType)params.takeFirst().value<qint16>();
    switch (requestType) {
        case Sync: {
            if (params.count() < 3) {
                qWarning() << Q_FUNC_INFO << "Received invalid sync call:" << params;
                return;
            }
            QByteArray className = params.takeFirst().toByteArray();
            QString objectName = QString::fromUtf8(params.takeFirst().toByteArray());
            QByteArray slotName = params.takeFirst().toByteArray();
            handle(Protocol::SyncMessage(className, objectName, slotName, params));
            break;
        }
        case RpcCall: {
            if (params.empty()) {
                qWarning() << Q_FUNC_INFO << "Received empty RPC call!";
                return;
            }
            QByteArray slotName = params.takeFirst().toByteArray();
            handle(Protocol::RpcCall(slotName, params));
            break;
        }
        case InitRequest: {
            if (params.count() != 2) {
                qWarning() << Q_FUNC_INFO << "Received invalid InitRequest:" << params;
                return;
            }
            QByteArray className = params[0].toByteArray();
            QString objectName = QString::fromUtf8(params[1].toByteArray());
            handle(Protocol::InitRequest(className, objectName));
            break;
        }
        case InitData: {
            if (params.count() < 2) {
                qWarning() << Q_FUNC_INFO << "Received invalid InitData:" << params;
                return;
            }
            QByteArray className = params.takeFirst().toByteArray();
            QString objectName = QString::fromUtf8(params.takeFirst().toByteArray());
            QVariantMap initData;
            for (int i = 0; i < params.count()/2; ++i)
                initData[QString::fromUtf8(params[2*i].toByteArray())] = params[2*i+1];
            handle(Protocol::InitData(className, objectName, initData));
            break;
        }
        case HeartBeat: {
            if (params.count() != 1) {
                qWarning() << Q_FUNC_INFO << "Received invalid HeartBeat:" << params;
                return;
            }
            // Note: QDateTime instead of QTime as in the legacy protocol!
            handle(Protocol::HeartBeat(params[0].toDateTime()));
            break;
        }
        case HeartBeatReply: {
            if (params.count() != 1) {
                qWarning() << Q_FUNC_INFO << "Received invalid HeartBeat:" << params;
                return;
            }
            // Note: QDateTime instead of QTime as in the legacy protocol!
            handle(Protocol::HeartBeatReply(params[0].toDateTime()));
            break;
        }

    }
}


void DataStreamPeer::dispatch(const Protocol::SyncMessage &msg)
{
    dispatchPackedFunc(QVariantList() << (qint16)Sync << msg.className << msg.objectName.toUtf8() << msg.slotName << msg.params);
}


void DataStreamPeer::dispatch(const Protocol::RpcCall &msg)
{
    dispatchPackedFunc(QVariantList() << (qint16)RpcCall << msg.slotName << msg.params);
}


void DataStreamPeer::dispatch(const Protocol::InitRequest &msg)
{
    dispatchPackedFunc(QVariantList() << (qint16)InitRequest << msg.className << msg.objectName.toUtf8());
}


void DataStreamPeer::dispatch(const Protocol::InitData &msg)
{
    QVariantList initData;
    QVariantMap::const_iterator it = msg.initData.begin();
    while (it != msg.initData.end()) {
        initData << it.key().toUtf8() << it.value();
        ++it;
    }
    dispatchPackedFunc(QVariantList() << (qint16)InitData << msg.className << msg.objectName.toUtf8() << initData);
}


void DataStreamPeer::dispatch(const Protocol::HeartBeat &msg)
{
    dispatchPackedFunc(QVariantList() << (qint16)HeartBeat << msg.timestamp);
}


void DataStreamPeer::dispatch(const Protocol::HeartBeatReply &msg)
{
    dispatchPackedFunc(QVariantList() << (qint16)HeartBeatReply << msg.timestamp);
}


void DataStreamPeer::dispatchPackedFunc(const QVariantList &packedFunc)
{
    writeSocketData(packedFunc);
}