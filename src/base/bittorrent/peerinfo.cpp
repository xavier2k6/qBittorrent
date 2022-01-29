/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2015  Vladimir Golovnev <glassez@yandex.ru>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "peerinfo.h"

#include <QBitArray>

#include "base/bittorrent/ltqbitarray.h"
#include "base/bittorrent/torrent.h"
#include "base/net/geoipmanager.h"
#include "base/unicodestrings.h"
#include "peeraddress.h"

using namespace BitTorrent;

PeerInfo::PeerInfo(const Torrent *torrent, const lt::peer_info &nativeInfo)
    : m_nativeInfo(nativeInfo)
    , m_relevance(calcRelevance(torrent))
{
    determineFlags();
}

bool PeerInfo::fromDHT() const
{
    return static_cast<bool>(m_nativeInfo.source & lt::peer_info::dht);
}

bool PeerInfo::fromPeX() const
{
    return static_cast<bool>(m_nativeInfo.source & lt::peer_info::pex);
}

bool PeerInfo::fromLSD() const
{
    return static_cast<bool>(m_nativeInfo.source & lt::peer_info::lsd);
}

QString PeerInfo::country() const
{
    if (m_country.isEmpty())
        m_country = Net::GeoIPManager::instance()->lookup(address().ip);
    return m_country;
}

bool PeerInfo::isInteresting() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::interesting);
}

bool PeerInfo::isChocked() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::choked);
}

bool PeerInfo::isRemoteInterested() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::remote_interested);
}

bool PeerInfo::isRemoteChocked() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::remote_choked);
}

bool PeerInfo::isSupportsExtensions() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::supports_extensions);
}

bool PeerInfo::isLocalConnection() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::local_connection);
}

bool PeerInfo::isHandshake() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::handshake);
}

bool PeerInfo::isConnecting() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::connecting);
}

bool PeerInfo::isOnParole() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::on_parole);
}

bool PeerInfo::isSeed() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::seed);
}

bool PeerInfo::optimisticUnchoke() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::optimistic_unchoke);
}

bool PeerInfo::isSnubbed() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::snubbed);
}

bool PeerInfo::isUploadOnly() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::upload_only);
}

bool PeerInfo::isEndgameMode() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::endgame_mode);
}

bool PeerInfo::isHolepunched() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::holepunched);
}

bool PeerInfo::useI2PSocket() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::i2p_socket);
}

bool PeerInfo::useUTPSocket() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::utp_socket);
}

bool PeerInfo::useSSLSocket() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::ssl_socket);
}

bool PeerInfo::isRC4Encrypted() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::rc4_encrypted);
}

bool PeerInfo::isPlaintextEncrypted() const
{
    return static_cast<bool>(m_nativeInfo.flags & lt::peer_info::plaintext_encrypted);
}

PeerAddress PeerInfo::address() const
{
    // fast path for platforms which boost.asio internal struct maps to `sockaddr`
    return {QHostAddress(m_nativeInfo.ip.data()), m_nativeInfo.ip.port()};
    // slow path for the others
    //return {QHostAddress(QString::fromStdString(m_nativeInfo.ip.address().to_string()))
    //    , m_nativeInfo.ip.port()};
}

QString PeerInfo::client() const
{
    return QString::fromStdString(m_nativeInfo.client);
}

qreal PeerInfo::progress() const
{
    return m_nativeInfo.progress;
}

int PeerInfo::payloadUpSpeed() const
{
    return m_nativeInfo.payload_up_speed;
}

int PeerInfo::payloadDownSpeed() const
{
    return m_nativeInfo.payload_down_speed;
}

qlonglong PeerInfo::totalUpload() const
{
    return m_nativeInfo.total_upload;
}

qlonglong PeerInfo::totalDownload() const
{
    return m_nativeInfo.total_download;
}

QBitArray PeerInfo::pieces() const
{
    return LT::toQBitArray(m_nativeInfo.pieces);
}

QString PeerInfo::connectionType() const
{
    if (m_nativeInfo.flags & lt::peer_info::utp_socket)
        return QString::fromUtf8(C_UTP);

    return (m_nativeInfo.connection_type == lt::peer_info::standard_bittorrent)
        ? QLatin1String {"BT"}
        : QLatin1String {"Web"};
}

qreal PeerInfo::calcRelevance(const Torrent *torrent) const
{
    const QBitArray allPieces = torrent->pieces();
    const int localMissing = allPieces.count(false);
    if (localMissing <= 0)
        return 0;

    const QBitArray peerPieces = pieces();
    const int remoteHaves = (peerPieces & (~allPieces)).count(true);
    return static_cast<qreal>(remoteHaves) / localMissing;
}

qreal PeerInfo::relevance() const
{
    return m_relevance;
}

void PeerInfo::determineFlags()
{
    const auto updateFlags = [this](const QChar specifier, const QString &explanation)
    {
        m_flags += (specifier + QLatin1Char(' '));
        m_flagsDescription += QString::fromLatin1("%1 = %2\n").arg(specifier, explanation);
    };

    if (isInteresting())
    {
        if (isRemoteChocked())
        {
            // d = Your client wants to download, but peer doesn't want to send (interested and choked)
            updateFlags(QLatin1Char('d'), tr("Interested (local) and choked (peer)"));
        }
        else
        {
            // D = Currently downloading (interested and not choked)
            updateFlags(QLatin1Char('D'), tr("Interested (local) and unchoked (peer)"));
        }
    }

    if (isRemoteInterested())
    {
        if (isChocked())
        {
            // u = Peer wants your client to upload, but your client doesn't want to (interested and choked)
            updateFlags(QLatin1Char('u'), tr("Interested (peer) and choked (local)"));
        }
        else
        {
            // U = Currently uploading (interested and not choked)
            updateFlags(QLatin1Char('U'), tr("Interested (peer) and unchoked (local)"));
        }
    }

    // K = Peer is unchoking your client, but your client is not interested
    if (!isRemoteChocked() && !isInteresting())
        updateFlags(QLatin1Char('K'), tr("Not interested (local) and unchoked (peer)"));

    // ? = Your client unchoked the peer but the peer is not interested
    if (!isChocked() && !isRemoteInterested())
        updateFlags(QLatin1Char('?'), tr("Not interested (peer) and unchoked (local)"));

    // O = Optimistic unchoke
    if (optimisticUnchoke())
        updateFlags(QLatin1Char('O'), tr("Optimistic unchoke"));

    // S = Peer is snubbed
    if (isSnubbed())
        updateFlags(QLatin1Char('S'), tr("Peer snubbed"));

    // I = Peer is an incoming connection
    if (!isLocalConnection())
        updateFlags(QLatin1Char('I'), tr("Incoming connection"));

    // H = Peer was obtained through DHT
    if (fromDHT())
        updateFlags(QLatin1Char('H'), tr("Peer from DHT"));

    // X = Peer was included in peerlists obtained through Peer Exchange (PEX)
    if (fromPeX())
        updateFlags(QLatin1Char('X'), tr("Peer from PEX"));

    // L = Peer is local
    if (fromLSD())
        updateFlags(QLatin1Char('L'), tr("Peer from LSD"));

    // E = Peer is using Protocol Encryption (all traffic)
    if (isRC4Encrypted())
        updateFlags(QLatin1Char('E'), tr("Encrypted traffic"));

    // e = Peer is using Protocol Encryption (handshake)
    if (isPlaintextEncrypted())
        updateFlags(QLatin1Char('e'), tr("Encrypted handshake"));

    // P = Peer is using uTorrent uTP
    if (useUTPSocket())
        updateFlags(QLatin1Char('P'), QString::fromUtf8(C_UTP));

    m_flags.chop(1);
    m_flagsDescription.chop(1);
}

QString PeerInfo::flags() const
{
    return m_flags;
}

QString PeerInfo::flagsDescription() const
{
    return m_flagsDescription;
}

int PeerInfo::downloadingPieceIndex() const
{
    return static_cast<int>(m_nativeInfo.downloading_piece_index);
}
