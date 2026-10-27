#include "spotify_connection.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QProcess>
#include <QRandomGenerator>
#include <QRunnable>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <limits>
#include <memory>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace {

constexpr qint64 spotifyImageCacheLifetimeMs = 365LL * 24 * 60 * 60 * 1000;
constexpr qint64 spotifyTrackPageCacheLifetimeMs = 24LL * 60 * 60 * 1000;
constexpr qint64 spotifyArtistMetadataCacheLifetimeMs = 30LL * 24 * 60 * 60 * 1000;
constexpr qint64 spotifyArtistReleaseCacheLifetimeMs = 365LL * 24 * 60 * 60 * 1000;
constexpr qint64 spotifyAlbumMetadataCacheLifetimeMs = 365LL * 24 * 60 * 60 * 1000;
constexpr qint64 spotifyLyricsCacheLifetimeMs = 365LL * 24 * 60 * 60 * 1000;
constexpr qint64 spotifyMissingLyricsCacheLifetimeMs = 7LL * 24 * 60 * 60 * 1000;
constexpr qint64 spotifyTrackPageRetryDelayMs = 30LL * 60 * 1000;
constexpr qint64 maxTrackPageCacheBytes = 64LL * 1024 * 1024;
constexpr int spotifyRequestTimeoutMs = 20000;
constexpr int spotifyImageRequestTimeoutMs = 30000;
constexpr int playbackStartupTimeoutMs = 25000;
QMutex spotifyImageCacheWriteMutex;
qint64 spotifyImageCacheDiskBytes = -1;

void setRequestTimeout(QNetworkRequest &request, int timeoutMs = spotifyRequestTimeoutMs)
{
    request.setTransferTimeout(timeoutMs);
}

void initializeImageCacheDiskSize(const QString &directoryPath)
{
    if (spotifyImageCacheDiskBytes >= 0) {
        return;
    }
    spotifyImageCacheDiskBytes = 0;
    QDir directory(directoryPath);
    for (const QFileInfo &entry : directory.entryInfoList(
             {QStringLiteral("*.bin")}, QDir::Files)) {
        spotifyImageCacheDiskBytes += entry.size();
    }
}

void pruneImageCacheFiles(const QString &directoryPath, const QString &newFilePath,
                          qint64 newFileSize, qint64 maximumBytes)
{
    if (directoryPath.isEmpty() || maximumBytes <= 0) {
        return;
    }
    QDir directory(directoryPath);
    if (!QDir().mkpath(directoryPath)) {
        return;
    }
    initializeImageCacheDiskSize(directoryPath);

    const qint64 targetOldSize = newFilePath.isEmpty() ? 0 : QFileInfo(newFilePath).size();
    qint64 projectedBytes = spotifyImageCacheDiskBytes - targetOldSize + newFileSize;
    if (projectedBytes <= maximumBytes) {
        return;
    }

    QList<QFileInfo> candidates;
    for (const QFileInfo &entry : directory.entryInfoList(
             {QStringLiteral("*.bin")}, QDir::Files, QDir::Time)) {
        if (!newFilePath.isEmpty() && entry.absoluteFilePath() == newFilePath) {
            continue;
        }
        candidates.append(entry);
    }

    std::sort(candidates.begin(), candidates.end(), [](const QFileInfo &left,
                                                        const QFileInfo &right) {
        return left.lastModified() < right.lastModified();
    });
    for (const QFileInfo &candidate : candidates) {
        if (projectedBytes <= maximumBytes) {
            break;
        }
        if (QFile::remove(candidate.absoluteFilePath())) {
            spotifyImageCacheDiskBytes -= candidate.size();
            projectedBytes -= candidate.size();
        }
    }
}

void removeImageCacheFile(const QString &path)
{
    QMutexLocker locker(&spotifyImageCacheWriteMutex);
    const qint64 fileSize = QFileInfo(path).size();
    if (QFile::remove(path) && spotifyImageCacheDiskBytes >= 0) {
        spotifyImageCacheDiskBytes = qMax<qint64>(0, spotifyImageCacheDiskBytes - fileSize);
    }
}

void writeImageCacheFile(const QString &path, const QByteArray &data, qint64 maximumBytes)
{
    if (path.isEmpty() || data.isEmpty()) {
        return;
    }
    const qint64 estimatedFileSize = data.size() + static_cast<qint64>(sizeof(qint64)) + 4;
    if (estimatedFileSize > maximumBytes) {
        return;
    }

    QMutexLocker locker(&spotifyImageCacheWriteMutex);
    const qint64 oldFileSize = QFileInfo(path).size();
    pruneImageCacheFiles(QFileInfo(path).absolutePath(), path, estimatedFileSize,
                         maximumBytes);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    QDataStream stream(&file);
    stream << QDateTime::currentMSecsSinceEpoch() << data;
    if (stream.status() == QDataStream::Ok && file.commit()) {
        const qint64 newFileSize = QFileInfo(path).size();
        spotifyImageCacheDiskBytes = qMax<qint64>(
            0, spotifyImageCacheDiskBytes + newFileSize - oldFileSize);
    }
}

QString bestImageUrl(const QJsonArray &images, int targetSize)
{
    QString fallback;
    int fallbackWidth = -1;
    QString selected;
    int selectedWidth = std::numeric_limits<int>::max();
    for (const QJsonValue &value : images) {
        const QJsonObject image = value.toObject();
        const QString url = image.value(QStringLiteral("url")).toString();
        if (url.isEmpty()) {
            continue;
        }
        const int width = image.value(QStringLiteral("width")).toInt(-1);
        if (fallback.isEmpty() || width > fallbackWidth) {
            fallback = url;
            fallbackWidth = width;
        }
        if (width >= targetSize && width < selectedWidth) {
            selected = url;
            selectedWidth = width;
        }
    }
    return selected.isEmpty() ? fallback : selected;
}

SpotifyArtistProfile parseArtistProfile(const QJsonObject &object)
{
    SpotifyArtistProfile artist;
    artist.id = object.value(QStringLiteral("id")).toString();
    artist.name = object.value(QStringLiteral("name")).toString();
    artist.imageUrl = bestImageUrl(object.value(QStringLiteral("images")).toArray(), 640);
    artist.spotifyUrl = object.value(QStringLiteral("external_urls")).toObject()
                            .value(QStringLiteral("spotify")).toString();
    for (const QJsonValue &genre : object.value(QStringLiteral("genres")).toArray()) {
        if (genre.isString()) artist.genres.append(genre.toString());
    }
    artist.followers = object.value(QStringLiteral("followers")).toObject()
                           .value(QStringLiteral("total")).toInt(-1);
    return artist;
}

SpotifyAlbumProfile parseAlbumProfile(const QJsonObject &object, int targetImageSize = 640)
{
    SpotifyAlbumProfile album;
    album.id = object.value(QStringLiteral("id")).toString();
    album.name = object.value(QStringLiteral("name")).toString();
    album.imageUrl = bestImageUrl(object.value(QStringLiteral("images")).toArray(),
                                  targetImageSize);
    album.spotifyUrl = object.value(QStringLiteral("external_urls")).toObject()
                           .value(QStringLiteral("spotify")).toString();
    album.releaseDate = object.value(QStringLiteral("release_date")).toString();
    album.albumType = object.value(QStringLiteral("album_type")).toString();
    album.totalTracks = object.value(QStringLiteral("total_tracks")).toInt();
    QStringList artistNames;
    for (const QJsonValue &value : object.value(QStringLiteral("artists")).toArray()) {
        const QJsonObject artistObject = value.toObject();
        const QString name = artistObject.value(QStringLiteral("name")).toString();
        if (album.artistId.isEmpty()) {
            album.artistId = artistObject.value(QStringLiteral("id")).toString();
        }
        if (!name.isEmpty()) artistNames.append(name);
    }
    album.artists = artistNames.join(QStringLiteral(", "));
    return album;
}

QList<SpotifyAlbumProfile> parseArtistAlbumPage(const QJsonObject &response)
{
    QList<SpotifyAlbumProfile> albums;
    const QJsonArray items = response.value(QStringLiteral("items")).toArray();
    albums.reserve(items.size());
    for (const QJsonValue &item : items) {
        SpotifyAlbumProfile album = parseAlbumProfile(item.toObject(), 300);
        if (!album.id.isEmpty() && !album.name.isEmpty()) albums.append(std::move(album));
    }
    return albums;
}

QByteArray secureRandomBytes(qsizetype length)
{
    QByteArray bytes;
    bytes.resize(length);

    auto *generator = QRandomGenerator::system();
    for (qsizetype index = 0; index < length; index += 4) {
        const quint32 value = generator->generate();
        for (qsizetype byteIndex = 0; byteIndex < 4 && index + byteIndex < length; ++byteIndex) {
            bytes[index + byteIndex] = static_cast<char>((value >> (byteIndex * 8)) & 0xff);
        }
    }

    return bytes;
}

QString randomUrlSafeString(qsizetype byteCount)
{
    return QString::fromLatin1(secureRandomBytes(byteCount).toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QByteArray formBody(const QList<QPair<QString, QString>> &fields)
{
    QUrlQuery form;
    for (const auto &field : fields) {
        form.addQueryItem(field.first, field.second);
    }
    return form.query(QUrl::FullyEncoded).toUtf8();
}

void respondToBrowser(QTcpSocket *socket, int statusCode, const QByteArray &reason,
                      const QByteArray &body)
{
    QByteArray response = "HTTP/1.1 " + QByteArray::number(statusCode) + " " + reason + "\r\n";
    response += "Content-Type: text/html; charset=utf-8\r\n";
    response += "Referrer-Policy: no-referrer\r\n";
    response += "Cache-Control: no-store\r\n";
    response += "Connection: close\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}

void respondToPlaybackBrowser(QTcpSocket *socket, int statusCode,
                              const QByteArray &reason, const QByteArray &contentType,
                              const QByteArray &body)
{
    QByteArray response = "HTTP/1.1 " + QByteArray::number(statusCode) + " " + reason + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Referrer-Policy: no-referrer\r\n";
    response += "Cache-Control: no-store\r\n";
    response += "X-Content-Type-Options: nosniff\r\n";
    response += "Connection: close\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}

QString replyError(QNetworkReply *reply, const QJsonObject &body)
{
    QString detail = body.value("error_description").toString();
    if (detail.isEmpty()) {
        detail = body.value("error").toString();
    }
    if (detail.isEmpty()) {
        detail = reply->errorString();
    }
    return detail;
}

SpotifyTrackResult parseTrack(const QJsonObject &item)
{
    SpotifyTrackResult track;
    track.name = item.value("name").toString();
    const QJsonObject album = item.value("album").toObject();
    track.album = album.value("name").toString();
    track.albumId = album.value("id").toString();
    track.albumImageUrl = bestImageUrl(album.value("images").toArray(), 300);
    track.spotifyUri = item.value("uri").toString();
    track.spotifyUrl = item.value("external_urls").toObject().value("spotify").toString();
    track.durationMs = item.value("duration_ms").toInt();

    const QJsonArray artistItems = item.value("artists").toArray();
    QStringList artistNames;
    artistNames.reserve(artistItems.size());
    for (const QJsonValue &artistValue : artistItems) {
        const QJsonObject artist = artistValue.toObject();
        const QString name = artist.value("name").toString();
        if (!name.isEmpty()) {
            artistNames.append(name);
        }
        if (track.artistId.isEmpty()) {
            track.artistId = artist.value("id").toString();
        }
    }
    track.artists = artistNames.join(QStringLiteral(", "));
    return track;
}

QString apiFailureMessage(QNetworkReply *reply, const QJsonObject &response)
{
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 403) {
        return QStringLiteral("Spotify denied access to this content (HTTP 403).");
    }
    if (statusCode == 429) {
        const QString reason = response.value(QStringLiteral("error")).toObject()
                                   .value(QStringLiteral("reason")).toString();
        if (reason == QStringLiteral("QUOTA_EXCEEDED")) {
            return QStringLiteral("Spotify's Development Mode API quota is exhausted (429: QUOTA_EXCEEDED). Wait for Spotify to restore the quota, then try again.");
        }
        bool retryAfterOk = false;
        const int retryAfterSeconds = reply->rawHeader(QByteArrayLiteral("Retry-After"))
                                          .toInt(&retryAfterOk);
        if (retryAfterOk && retryAfterSeconds > 0) {
            return QStringLiteral("Spotify is rate limiting Web API requests (HTTP 429). Retry after %1 seconds.")
                .arg(retryAfterSeconds);
        }
        return QStringLiteral("Spotify is rate limiting Web API requests (HTTP 429). The app is pausing automatic requests temporarily.");
    }

    QString message = replyError(reply, response);
    if (message.isEmpty() && statusCode != 0) {
        message = QStringLiteral("Spotify returned HTTP %1.").arg(statusCode);
    }
    return message;
}

QString findHeadlessChromium()
{
#ifdef Q_OS_WIN
    QStringList candidates;
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    const QString programFiles = qEnvironmentVariable("ProgramFiles");
    const QString programFilesX86 = qEnvironmentVariable("ProgramFiles(x86)");
    if (!localAppData.isEmpty()) {
        candidates.append(QDir(localAppData).filePath(QStringLiteral("Google/Chrome/Application/chrome.exe")));
        candidates.append(QDir(localAppData).filePath(QStringLiteral("Microsoft/Edge/Application/msedge.exe")));
    }
    if (!programFiles.isEmpty()) {
        candidates.append(QDir(programFiles).filePath(QStringLiteral("Google/Chrome/Application/chrome.exe")));
        candidates.append(QDir(programFiles).filePath(QStringLiteral("Microsoft/Edge/Application/msedge.exe")));
    }
    if (!programFilesX86.isEmpty()) {
        candidates.append(QDir(programFilesX86).filePath(QStringLiteral("Google/Chrome/Application/chrome.exe")));
        candidates.append(QDir(programFilesX86).filePath(QStringLiteral("Microsoft/Edge/Application/msedge.exe")));
    }
    candidates.append(QStringLiteral("chrome.exe"));
    candidates.append(QStringLiteral("msedge.exe"));
#elif defined(Q_OS_MACOS)
    const QString homeApplications = QDir::home().filePath(QStringLiteral("Applications"));
    QStringList candidates{
        QDir(homeApplications).filePath(
            QStringLiteral("Google Chrome.app/Contents/MacOS/Google Chrome")),
        QStringLiteral("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"),
        QDir(homeApplications).filePath(
            QStringLiteral("Microsoft Edge.app/Contents/MacOS/Microsoft Edge")),
        QStringLiteral("/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge"),
        QDir(homeApplications).filePath(
            QStringLiteral("Chromium.app/Contents/MacOS/Chromium")),
        QStringLiteral("/Applications/Chromium.app/Contents/MacOS/Chromium"),
        QStringLiteral("google-chrome"), QStringLiteral("google-chrome-stable"),
        QStringLiteral("chromium"), QStringLiteral("chromium-browser"),
        QStringLiteral("microsoft-edge")
    };
#else
    QStringList candidates{
        QStringLiteral("google-chrome"), QStringLiteral("google-chrome-stable"),
        QStringLiteral("chromium"), QStringLiteral("chromium-browser"),
        QStringLiteral("microsoft-edge")
    };
#endif

    for (const QString &candidate : candidates) {
        if (QFileInfo(candidate).isFile()) {
            return QFileInfo(candidate).absoluteFilePath();
        }
        const QString executable = QStandardPaths::findExecutable(candidate);
        if (!executable.isEmpty()) {
            return executable;
        }
    }
    return {};
}

QByteArray playbackPageHtml(int initialVolumePercent)
{
    static constexpr char html[] = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="referrer" content="no-referrer">
  <title>MusicPlayer Playback</title>
  <style>
    :root { color-scheme: dark; font: 16px "Segoe UI", sans-serif; background: #000000; color: #ffffff; }
    body { margin: 0; min-height: 100vh; display: grid; place-items: center; }
    main { width: min(520px, calc(100% - 48px)); padding: 28px; border: 1px solid #282828; border-radius: 14px; background: #181818; }
    h1 { margin: 0 0 8px; font-size: 22px; }
    p { color: #b3b3b3; line-height: 1.55; }
    button { padding: 10px 16px; border: 0; border-radius: 8px; background: #1ed760; color: #000000; font: inherit; font-weight: 650; cursor: pointer; }
    button:disabled { opacity: .55; cursor: default; }
    #status { min-height: 24px; margin-bottom: 0; }
  </style>
</head>
<body>
  <main>
    <h1>MusicPlayer playback engine</h1>
    <p>Spotify audio is running in MusicPlayer's hidden playback process. You can close this page if you opened it manually.</p>
    <p id="status">Loading Spotify's playback engine…</p>
  </main>
  <script>
    const pageUrl = new URL(location.href);
    const localKey = pageUrl.searchParams.get('key') || '';
    history.replaceState(null, '', pageUrl.pathname);
    const statusElement = document.getElementById('status');
    let player = null;
    let isReady = false;
    let commandPollRunning = false;
    let playerStateSyncTimer = null;

    function setStatus(message) { statusElement.textContent = message; }
    async function sendLocal(path, data, method = 'POST') {
      const options = { method, headers: { 'X-MusicPlayer-Key': localKey } };
      if (data !== undefined) {
        options.headers['Content-Type'] = 'application/json';
        options.body = JSON.stringify(data);
      }
      return fetch(path, options);
    }
    function reportError(type, message) {
      setStatus(message);
      sendLocal('/error', { type, message }).catch(() => {});
    }
    function bestArtworkUrl(images, targetSize = 300) {
      let fallback = '';
      let fallbackWidth = -1;
      let selected = '';
      let selectedWidth = Number.MAX_SAFE_INTEGER;
      for (const image of images || []) {
        if (!image || !image.url) continue;
        const width = Number(image.width || image.height || -1);
        if (!fallback || width > fallbackWidth) {
          fallback = image.url;
          fallbackWidth = width;
        }
        if (width >= targetSize && width < selectedWidth) {
          selected = image.url;
          selectedWidth = width;
        }
      }
      return selected || fallback;
    }
    function reportPlaybackState(state, treatNullAsInactive = true) {
      if (!state) {
        if (treatNullAsInactive) sendLocal('/state', { active: false }).catch(() => {});
        return;
      }
      const track = state.track_window.current_track;
      sendLocal('/state', {
        active: !!track,
        position_ms: state.position,
        duration_ms: track ? track.duration_ms : state.duration,
        paused: state.paused,
        track: track ? {
          name: track.name,
          uri: track.uri,
          artists: track.artists.map(artist => artist.name),
          artist_id: track.artists.length && track.artists[0].uri
            ? track.artists[0].uri.split(':').pop() : '',
          album: track.album.name,
          album_id: track.album.uri ? track.album.uri.split(':').pop() : '',
          artwork_url: bestArtworkUrl(track.album.images)
        } : null
      }).catch(() => {});
    }
    async function pollPlaybackCommands() {
      if (!isReady || !player) {
        commandPollRunning = false;
        return;
      }
      try {
        const response = await sendLocal('/commands', undefined, 'GET');
        if (!response.ok) throw new Error('The local playback command bridge is unavailable.');
        const commands = await response.json();
        for (const command of commands) {
          try {
            if (command.name === 'pause') await player.pause();
            else if (command.name === 'resume') await player.resume();
            else if (command.name === 'next') await player.nextTrack();
            else if (command.name === 'previous') await player.previousTrack();
            else if (command.name === 'seek') await player.seek(command.position_ms);
            else if (command.name === 'volume') await player.setVolume(command.volume);
          } catch (error) {
            sendLocal('/command-error', {
              name: command.name,
              message: error && error.message ? error.message : String(error)
            }).catch(() => {});
          }
        }
      } catch (error) {
        sendLocal('/command-error', {
          name: 'bridge',
          message: error && error.message ? error.message : String(error)
        }).catch(() => {});
      } finally {
        if (isReady && player) window.setTimeout(pollPlaybackCommands, 50);
        else commandPollRunning = false;
      }
    }
    function startPlaybackCommandPolling() {
      if (commandPollRunning) return;
      commandPollRunning = true;
      pollPlaybackCommands();
    }

    window.onSpotifyWebPlaybackSDKReady = () => {
      player = new Spotify.Player({
        name: 'MusicPlayer',
        volume: __INITIAL_VOLUME_PERCENT__ / 100,
        // Keep the player independent from Chrome's media-session audio focus.
        enableMediaSession: false,
        getOAuthToken: async callback => {
          try {
            const response = await sendLocal('/token', undefined, 'GET');
            if (!response.ok) throw new Error('MusicPlayer could not provide a Spotify access token.');
            const result = await response.json();
            callback(result.access_token);
          } catch (error) {
            reportError('authentication_error', error.message);
          }
        }
      });

      player.addListener('ready', ({ device_id }) => {
        isReady = true;
        setStatus('Spotify playback device is ready.');
        sendLocal('/ready', { device_id }).then(() => {
          if (!isReady || !player) return;
          return sendLocal('/activated', {}).then(() => {
            if (isReady && player) startPlaybackCommandPolling();
          });
        }).catch(() => {});
      });
      player.addListener('not_ready', () => {
        isReady = false;
        commandPollRunning = false;
        setStatus('Spotify playback device is offline.');
        sendLocal('/stopped', {}).catch(() => {});
      });
      player.addListener('player_state_changed', state => {
        reportPlaybackState(state);
      });
      player.addListener('initialization_error', ({ message }) => reportError('initialization_error', message));
      player.addListener('authentication_error', ({ message }) => reportError('authentication_error', message));
      player.addListener('account_error', ({ message }) => reportError('account_error', message));
      player.addListener('playback_error', ({ message }) => reportError('playback_error', message));
      player.addListener('autoplay_failed', () => {
        const message = 'Headless browser autoplay was denied.';
        setStatus(message);
        sendLocal('/error', { type: 'autoplay_error', message }).catch(() => {});
      });

      player.connect().then(connected => {
        if (!connected) setStatus('Could not connect the Spotify playback device.');
      });
      if (!playerStateSyncTimer) {
        playerStateSyncTimer = window.setInterval(async () => {
          if (!isReady || !player) return;
          try {
            reportPlaybackState(await player.getCurrentState(), false);
          } catch (_) {}
        }, 500);
      }
    };

    window.addEventListener('pagehide', () => {
      fetch('/stopped', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-MusicPlayer-Key': localKey },
        body: '{}',
        keepalive: true
      }).catch(() => {});
      if (playerStateSyncTimer) window.clearInterval(playerStateSyncTimer);
    });

    const healthTimer = setInterval(async () => {
      try {
        const response = await sendLocal('/health', undefined, 'GET');
        if (!response.ok) throw new Error('offline');
      } catch (_) {
        clearInterval(healthTimer);
        if (player) player.disconnect();
        setStatus('MusicPlayer is closed. The playback engine has disconnected.');
      }
    }, 3000);

    const sdk = document.createElement('script');
    sdk.src = 'https://sdk.scdn.co/spotify-player.js';
    sdk.referrerPolicy = 'no-referrer';
    document.head.appendChild(sdk);
  </script>
</body>
</html>)HTML";
    QByteArray page(html);
    page.replace(QByteArrayLiteral("__INITIAL_VOLUME_PERCENT__"),
                 QByteArray::number(qBound(0, initialVolumePercent, 100)));
    return page;
}

} // namespace

SpotifyConnection::SpotifyConnection(QObject *parent)
    : QObject(parent),
      networkManager_(new QNetworkAccessManager(this)),
      callbackServer_(new QTcpServer(this)),
      playbackServer_(new QTcpServer(this)),
      playbackBrowserProcess_(new QProcess(this)),
      loginTimeout_(new QTimer(this)),
      refreshTimer_(new QTimer(this)),
      profileRetryTimer_(new QTimer(this)),
      playbackPollTimer_(new QTimer(this)),
      playbackStartupTimer_(new QTimer(this))
{
    const QString cacheBasePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString apiCachePath = QDir(cacheBasePath).filePath(QStringLiteral("spotify-api"));
    if (!cacheBasePath.isEmpty() && QDir().mkpath(apiCachePath)) {
        httpCache_ = new QNetworkDiskCache(networkManager_);
        httpCache_->setCacheDirectory(apiCachePath);
        httpCache_->setMaximumCacheSize(16 * 1024 * 1024);
        networkManager_->setCache(httpCache_);
    }
    loadLyricsCache();
    loginTimeout_->setSingleShot(true);
    refreshTimer_->setSingleShot(true);
    profileRetryTimer_->setSingleShot(true);
    playbackPollTimer_->setInterval(10000);
    playbackStartupTimer_->setSingleShot(true);
    connect(playbackPollTimer_, &QTimer::timeout, this, [this] { pollPlaybackState(); });
    connect(profileRetryTimer_, &QTimer::timeout, this, [this] {
        if (!accessToken_.isEmpty()) {
            requestCurrentUserProfile();
        }
    });
    connect(playbackStartupTimer_, &QTimer::timeout, this, [this] {
        if (!localPlaybackState_.deviceReady || !localPlaybackState_.audioActivated) {
            if (playbackStatusHandler_) {
                playbackStatusHandler_(QStringLiteral(
                    "Headless Spotify playback initialization timed out. Check your network and browser."));
            }
        }
    });

    connect(callbackServer_, &QTcpServer::newConnection,
            this, [this] { handleIncomingConnection(); });
    connect(playbackServer_, &QTcpServer::newConnection,
            this, [this] { handlePlaybackConnection(); });
    connect(playbackBrowserProcess_, &QProcess::started, this, [this] {
#ifdef Q_OS_WIN
        if (!playbackBrowserJob_) {
            HANDLE job = CreateJobObjectW(nullptr, nullptr);
            if (job) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                            &limits, sizeof(limits))) {
                    HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                                                FALSE,
                                                static_cast<DWORD>(playbackBrowserProcess_->processId()));
                    if (process) {
                        if (AssignProcessToJobObject(job, process)) {
                            playbackBrowserJob_ = job;
                        }
                        CloseHandle(process);
                    }
                }
                if (!playbackBrowserJob_) {
                    CloseHandle(job);
                }
            }
        }
#endif
    });
    connect(playbackBrowserProcess_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) {
            return;
        }
        playbackStartupTimer_->stop();
        playbackPageOpened_ = false;
        if (playbackStatusHandler_) {
            playbackStatusHandler_(QStringLiteral("Could not start the headless Chromium playback browser: %1")
                                       .arg(playbackBrowserProcess_->errorString()));
        }
    });
    connect(playbackBrowserProcess_, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus) {
        playbackStartupTimer_->stop();
        playbackPageOpened_ = false;
        const QString stoppedDeviceId = playbackDeviceId_;
        playbackDeviceId_.clear();
        localPlaybackState_ = {};
        if (!stoppedDeviceId.isEmpty() && apiPlaybackState_.deviceId == stoppedDeviceId) {
            apiPlaybackState_ = {};
            apiPlaybackStateKnown_ = false;
        }
        updateEffectivePlaybackState();
#ifdef Q_OS_WIN
        if (playbackBrowserJob_) {
            CloseHandle(static_cast<HANDLE>(playbackBrowserJob_));
            playbackBrowserJob_ = nullptr;
        }
#endif
        if (connected_ && exitCode != 0 && playbackStatusHandler_) {
            playbackStatusHandler_(QStringLiteral("The headless playback process stopped unexpectedly (exit code %1).")
                                       .arg(exitCode));
        }
    });
    connect(loginTimeout_, &QTimer::timeout, this, [this] {
        fail("Spotify sign-in timed out. Please try connecting again.");
    });
    connect(refreshTimer_, &QTimer::timeout, this, [this] { refreshAccessToken(); });
}

SpotifyConnection::~SpotifyConnection()
{
    playbackStateHandler_ = {};
    playbackStatusHandler_ = {};
    stopPlaybackCompanion();
}

void SpotifyConnection::stopPlaybackCompanion()
{
    playbackStartupTimer_->stop();
    pendingPlaybackCommands_.clear();
    if (playbackBrowserProcess_ && playbackBrowserProcess_->state() != QProcess::NotRunning) {
#ifdef Q_OS_WIN
        if (playbackBrowserJob_) {
            CloseHandle(static_cast<HANDLE>(playbackBrowserJob_));
            playbackBrowserJob_ = nullptr;
        } else
#endif
        {
            playbackBrowserProcess_->terminate();
            if (!playbackBrowserProcess_->waitForFinished(1000)) {
                playbackBrowserProcess_->kill();
            }
        }
        if (playbackBrowserProcess_->state() != QProcess::NotRunning) {
            playbackBrowserProcess_->waitForFinished(2000);
        }
    }
#ifdef Q_OS_WIN
    if (playbackBrowserJob_) {
        CloseHandle(static_cast<HANDLE>(playbackBrowserJob_));
        playbackBrowserJob_ = nullptr;
    }
#endif
}

void SpotifyConnection::setStateHandler(StateHandler handler)
{
    stateHandler_ = std::move(handler);
}

void SpotifyConnection::setErrorHandler(ErrorHandler handler)
{
    errorHandler_ = std::move(handler);
}

void SpotifyConnection::setPlaybackStateHandler(PlaybackStateHandler handler)
{
    playbackStateHandler_ = std::move(handler);
}

void SpotifyConnection::setPlaybackStatusHandler(PlaybackStatusHandler handler)
{
    playbackStatusHandler_ = std::move(handler);
}

void SpotifyConnection::setDevicesUpdatedHandler(DevicesUpdatedHandler handler)
{
    devicesUpdatedHandler_ = std::move(handler);
    notifyDevicesUpdated();
}

QList<SpotifyDeviceResult> SpotifyConnection::availableDevices() const
{
    return availableDevices_;
}

QString SpotifyConnection::availableDevicesError() const
{
    return availableDevicesError_;
}

QString SpotifyConnection::localPlaybackDeviceId() const
{
    return playbackDeviceId_;
}

void SpotifyConnection::refreshAvailableDevices(std::function<void()> completion)
{
    if (completion) {
        devicesRefreshCallbacks_.append(std::move(completion));
    }
    if (!connected_ || accessToken_.isEmpty()) {
        const auto callbacks = std::move(devicesRefreshCallbacks_);
        devicesRefreshCallbacks_.clear();
        for (const auto &callback : callbacks) {
            if (callback) {
                callback();
            }
        }
        return;
    }
    if (apiRateLimitActive()) {
        availableDevicesError_ = apiRateLimitMessage();
        notifyDevicesUpdated();
        const auto callbacks = std::move(devicesRefreshCallbacks_);
        devicesRefreshCallbacks_.clear();
        for (const auto &callback : callbacks) {
            if (callback) {
                callback();
            }
        }
        return;
    }
    if (devicesPollInFlight_) {
        return;
    }

    devicesPollInFlight_ = true;
    lastDevicesRefreshMs_ = QDateTime::currentMSecsSinceEpoch();
    QNetworkReply *reply = authenticatedGet(
        QUrl(QStringLiteral("https://api.spotify.com/v1/me/player/devices")), false, true);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        devicesPollInFlight_ = false;
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() == QNetworkReply::NoError && statusCode >= 200 && statusCode < 300) {
            availableDevicesError_.clear();
            availableDevices_.clear();
            const QJsonArray devices = response.value(QStringLiteral("devices")).toArray();
            availableDevices_.reserve(devices.size());
            for (const QJsonValue &value : devices) {
                const QJsonObject object = value.toObject();
                SpotifyDeviceResult device;
                device.id = object.value(QStringLiteral("id")).toString();
                device.name = object.value(QStringLiteral("name")).toString();
                device.type = object.value(QStringLiteral("type")).toString();
                device.active = object.value(QStringLiteral("is_active")).toBool();
                device.restricted = object.value(QStringLiteral("is_restricted")).toBool();
                device.supportsVolume = object.value(QStringLiteral("supports_volume")).toBool();
                if (object.value(QStringLiteral("volume_percent")).isDouble()) {
                    device.volumePercent = object.value(QStringLiteral("volume_percent")).toInt();
                }
                availableDevices_.append(std::move(device));
            }
            lastDevicesRefreshMs_ = QDateTime::currentMSecsSinceEpoch();
            notifyDevicesUpdated();
        } else {
            const QJsonObject errorObject = response.value(QStringLiteral("error")).toObject();
            QString detail = errorObject.value(QStringLiteral("message")).toString();
            if (detail.isEmpty()) {
                detail = reply->errorString();
            }
            if (statusCode == 403) {
                availableDevicesError_ = QStringLiteral(
                    "Spotify denied device access (403). Reconnect in Settings to grant playback-state access.");
                if (!playbackScopeWarningShown_ && playbackStatusHandler_) {
                    playbackScopeWarningShown_ = true;
                    playbackStatusHandler_(availableDevicesError_);
                }
            } else if (statusCode == 401) {
                availableDevicesError_ = QStringLiteral(
                    "Spotify rejected device access (401). Reconnect your account and try again.");
            } else if (statusCode == 429) {
                availableDevicesError_ = apiFailureMessage(reply, response);
            } else if (statusCode != 0) {
                availableDevicesError_ = QStringLiteral("Spotify device check failed (HTTP %1): %2")
                                             .arg(statusCode).arg(detail);
            } else {
                availableDevicesError_ = QStringLiteral("Could not reach Spotify's device endpoint: %1")
                                             .arg(detail);
            }
            availableDevices_.clear();
            notifyDevicesUpdated();
        }
        reply->deleteLater();

        const auto callbacks = std::move(devicesRefreshCallbacks_);
        devicesRefreshCallbacks_.clear();
        for (const auto &callback : callbacks) {
            if (callback) {
                callback();
            }
        }
    });
}

void SpotifyConnection::notifyDevicesUpdated()
{
    if (devicesUpdatedHandler_) {
        devicesUpdatedHandler_(availableDevices_);
    }
}

void SpotifyConnection::playTracks(const QStringList &trackUris, const QString &contextUri,
                                   const QString &offsetUri, int positionMs)
{
    if (!connected_ || accessToken_.isEmpty()) {
        if (playbackStatusHandler_) {
            playbackStatusHandler_(QStringLiteral("Connect Spotify before starting playback."));
        }
        return;
    }
    const bool hasContext = !contextUri.isEmpty();
    const bool hasQueue = !trackUris.isEmpty();
    if ((!hasContext && !hasQueue)
        || (hasContext && !offsetUri.startsWith(QStringLiteral("spotify:track:")))) {
        if (playbackStatusHandler_) {
            playbackStatusHandler_(QStringLiteral("Spotify did not provide a playable track or playlist."));
        }
        return;
    }
    for (const QString &uri : trackUris) {
        if (!uri.startsWith(QStringLiteral("spotify:track:"))
            && !uri.startsWith(QStringLiteral("spotify:episode:"))) {
            if (playbackStatusHandler_) {
                playbackStatusHandler_(QStringLiteral("A selected item is not a playable Spotify track or episode."));
            }
            return;
        }
    }

    pendingPlaybackUris_ = trackUris;
    pendingPlaybackContextUri_ = contextUri;
    pendingPlaybackOffsetUri_ = offsetUri;
    pendingPlaybackPositionMs_ = positionMs >= 0 ? positionMs : -1;
    startPlaybackCompanion();
    maybeStartPendingPlayback();
}

void SpotifyConnection::pausePlayback()
{
    if (playbackState_.externalDevice) {
        QUrl url(QStringLiteral("https://api.spotify.com/v1/me/player/pause"));
        QUrlQuery query;
        if (!playbackState_.deviceId.isEmpty()) {
            query.addQueryItem(QStringLiteral("device_id"), playbackState_.deviceId);
        }
        url.setQuery(query);
        sendPlayerCommand(QByteArrayLiteral("PUT"), url, {});
        return;
    }
    queueLocalPlaybackCommand(QStringLiteral("pause"));
}

void SpotifyConnection::resumePlayback()
{
    if (playbackState_.externalDevice) {
        QUrl url(QStringLiteral("https://api.spotify.com/v1/me/player/play"));
        QUrlQuery query;
        if (!playbackState_.deviceId.isEmpty()) {
            query.addQueryItem(QStringLiteral("device_id"), playbackState_.deviceId);
        }
        url.setQuery(query);
        sendPlayerCommand(QByteArrayLiteral("PUT"), url, QByteArrayLiteral("{}"));
        return;
    }
    if (playbackDeviceId_.isEmpty()) {
        startPlaybackCompanion();
        return;
    }
    queueLocalPlaybackCommand(QStringLiteral("resume"));
}

void SpotifyConnection::nextTrack()
{
    if (playbackState_.externalDevice) {
        QUrl url(QStringLiteral("https://api.spotify.com/v1/me/player/next"));
        QUrlQuery query;
        if (!playbackState_.deviceId.isEmpty()) {
            query.addQueryItem(QStringLiteral("device_id"), playbackState_.deviceId);
        }
        url.setQuery(query);
        sendPlayerCommand(QByteArrayLiteral("POST"), url, {});
        return;
    }
    queueLocalPlaybackCommand(QStringLiteral("next"));
}

void SpotifyConnection::previousTrack()
{
    if (playbackState_.externalDevice) {
        QUrl url(QStringLiteral("https://api.spotify.com/v1/me/player/previous"));
        QUrlQuery query;
        if (!playbackState_.deviceId.isEmpty()) {
            query.addQueryItem(QStringLiteral("device_id"), playbackState_.deviceId);
        }
        url.setQuery(query);
        sendPlayerCommand(QByteArrayLiteral("POST"), url, {});
        return;
    }
    queueLocalPlaybackCommand(QStringLiteral("previous"));
}

void SpotifyConnection::setRepeatMode(const QString &mode)
{
    if (mode != QStringLiteral("off") && mode != QStringLiteral("context")
        && mode != QStringLiteral("track")) {
        return;
    }
    QUrl url(QStringLiteral("https://api.spotify.com/v1/me/player/repeat"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("state"), mode);
    if (!playbackState_.deviceId.isEmpty()) {
        query.addQueryItem(QStringLiteral("device_id"), playbackState_.deviceId);
    }
    url.setQuery(query);
    sendPlayerCommand(QByteArrayLiteral("PUT"), url, {});
}

void SpotifyConnection::seekPlayback(int positionMs)
{
    if (playbackState_.externalDevice) {
        QUrl url(QStringLiteral("https://api.spotify.com/v1/me/player/seek"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("position_ms"), QString::number(qMax(0, positionMs)));
        if (!playbackState_.deviceId.isEmpty()) {
            query.addQueryItem(QStringLiteral("device_id"), playbackState_.deviceId);
        }
        url.setQuery(query);
        sendPlayerCommand(QByteArrayLiteral("PUT"), url, {});
        return;
    }
    queueLocalPlaybackCommand(QStringLiteral("seek"), qMax(0, positionMs));
}

void SpotifyConnection::setPlaybackVolume(int volumePercent)
{
    const int boundedVolume = qBound(0, volumePercent, 100);
    preferredPlaybackVolumePercent_ = boundedVolume;
    if (playbackState_.externalDevice) {
        QUrl url(QStringLiteral("https://api.spotify.com/v1/me/player/volume"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("volume_percent"),
                           QString::number(boundedVolume));
        if (!playbackState_.deviceId.isEmpty()) {
            query.addQueryItem(QStringLiteral("device_id"), playbackState_.deviceId);
        }
        url.setQuery(query);
        sendPlayerCommand(QByteArrayLiteral("PUT"), url, {});
        return;
    }
    localPlaybackState_.deviceVolumePercent = boundedVolume;
    updateEffectivePlaybackState();
    if (localPlaybackState_.deviceReady && localPlaybackState_.audioActivated) {
        queueLocalPlaybackCommand(QStringLiteral("volume"), localPlaybackState_.deviceVolumePercent);
    }
}

void SpotifyConnection::transferPlaybackToDevice(const QString &deviceId)
{
    if (!connected_ || deviceId.isEmpty()) {
        return;
    }
    if (deviceId == playbackState_.deviceId) {
        return;
    }

    QJsonArray deviceIds;
    deviceIds.append(deviceId);
    const QJsonObject body{{QStringLiteral("device_ids"), deviceIds}};
    const QUrl url(QStringLiteral("https://api.spotify.com/v1/me/player"));
    sendPlayerCommand(QByteArrayLiteral("PUT"), url,
                      QJsonDocument(body).toJson(QJsonDocument::Compact));
    QTimer::singleShot(1200, this, [this] {
        pollPlaybackState();
        refreshAvailableDevices();
    });
}

QString SpotifyConnection::sessionFilePath() const
{
    const QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(appDataPath).filePath(QStringLiteral("spotify-session.json"));
}

bool SpotifyConnection::saveSession(QString *errorMessage) const
{
#ifdef Q_OS_WIN
    if (refreshToken_.isEmpty() || clientId_.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Spotify did not provide a refresh token to save.");
        }
        return false;
    }

    const QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (appDataPath.isEmpty() && errorMessage) {
        *errorMessage = QStringLiteral("Windows did not provide an app data folder.");
    }
    if (appDataPath.isEmpty() || !QDir().mkpath(appDataPath)) {
        if (errorMessage && errorMessage->isEmpty()) {
            *errorMessage = QStringLiteral("Could not create the app data folder for the Spotify session.");
        }
        return false;
    }

    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(refreshToken_.size());
    input.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(refreshToken_.constData()));
    DATA_BLOB encrypted{};
    if (!CryptProtectData(&input, L"MusicPlayer Spotify refresh token", nullptr,
                          nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &encrypted)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Windows could not protect the Spotify refresh token.");
        }
        return false;
    }

    const QByteArray encryptedToken(reinterpret_cast<const char *>(encrypted.pbData),
                                    static_cast<qsizetype>(encrypted.cbData));
    LocalFree(encrypted.pbData);

    QJsonObject session;
    session.insert(QStringLiteral("version"), 1);
    session.insert(QStringLiteral("client_id"), clientId_);
    session.insert(QStringLiteral("refresh_token_dpapi"),
                   QString::fromLatin1(encryptedToken.toBase64()));

    QSaveFile file(sessionFilePath());
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not open the Spotify session file in AppData: %1")
                                .arg(file.errorString());
        }
        return false;
    }
    const QByteArray json = QJsonDocument(session).toJson(QJsonDocument::Compact);
    if (file.write(json) != json.size() || !file.commit()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not save the Spotify session in AppData: %1")
                                .arg(file.errorString());
        }
        return false;
    }
    return true;
#else
    Q_UNUSED(errorMessage);
    return false;
#endif
}

QString SpotifyConnection::restoreSession()
{
#ifdef Q_OS_WIN
    QFile file(sessionFilePath());
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorHandler_) {
            errorHandler_(QStringLiteral("Could not read the saved Spotify session: %1")
                              .arg(file.errorString()));
        }
        return {};
    }

    const QJsonObject session = QJsonDocument::fromJson(file.readAll()).object();
    const QString storedClientId = session.value(QStringLiteral("client_id")).toString().trimmed();
    const QByteArray encryptedToken = QByteArray::fromBase64(
        session.value(QStringLiteral("refresh_token_dpapi")).toString().toLatin1());
    if (storedClientId.isEmpty() || encryptedToken.isEmpty()) {
        if (errorHandler_) {
            errorHandler_(QStringLiteral("The saved Spotify session is incomplete. Connect Spotify again."));
        }
        return {};
    }

    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(encryptedToken.size());
    input.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(encryptedToken.constData()));
    DATA_BLOB decrypted{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &decrypted)) {
        if (errorHandler_) {
            errorHandler_(QStringLiteral("Windows could not unlock the saved Spotify session. Connect Spotify again."));
        }
        return {};
    }

    refreshToken_ = QByteArray(reinterpret_cast<const char *>(decrypted.pbData),
                               static_cast<qsizetype>(decrypted.cbData));
    SecureZeroMemory(decrypted.pbData, decrypted.cbData);
    LocalFree(decrypted.pbData);
    if (refreshToken_.isEmpty()) {
        if (errorHandler_) {
            errorHandler_(QStringLiteral("The saved Spotify session has no refresh token. Connect Spotify again."));
        }
        return {};
    }

    clientId_ = storedClientId;
    refreshAccessToken();
    return clientId_;
#else
    return {};
#endif
}

void SpotifyConnection::beginLogin(const QString &clientId, quint16 callbackPort)
{
    profileRetryTimer_->stop();
    playbackScopeWarningShown_ = false;
    clientId_ = clientId.trimmed();
    if (clientId_.isEmpty()) {
        fail("Enter your Spotify Client ID in Settings first.");
        return;
    }

    clearApiCache();
    resetImageMemoryCache();

    if (callbackServer_->isListening()) {
        callbackServer_->close();
    }
    if (!callbackServer_->listen(QHostAddress::LocalHost, callbackPort)) {
        fail(QStringLiteral("Could not open the Spotify callback at http://127.0.0.1:%1. %2")
                 .arg(callbackPort)
                 .arg(callbackServer_->errorString()));
        return;
    }

    codeVerifier_ = randomUrlSafeString(48);
    state_ = randomUrlSafeString(32);
    const QString codeChallenge = QString::fromLatin1(
        QCryptographicHash::hash(codeVerifier_.toUtf8(), QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    redirectUri_ = QStringLiteral("http://127.0.0.1:%1").arg(callbackPort);

    QUrl authorizationUrl(QStringLiteral("https://accounts.spotify.com/authorize"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client_id"), clientId_);
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("redirect_uri"), redirectUri_);
    query.addQueryItem(QStringLiteral("scope"),
                       QStringLiteral("user-read-private user-read-playback-state user-library-read playlist-read-private playlist-read-collaborative streaming user-modify-playback-state"));
    query.addQueryItem(QStringLiteral("show_dialog"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("state"), state_);
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    query.addQueryItem(QStringLiteral("code_challenge"), codeChallenge);
    authorizationUrl.setQuery(query);

    setState("Waiting for Spotify authorization in your browser…", false, true);
    loginTimeout_->start(5 * 60 * 1000);
    if (!QDesktopServices::openUrl(authorizationUrl)) {
        fail("Could not open your browser for Spotify sign-in.");
    }
}

void SpotifyConnection::disconnectAccount()
{
    loginTimeout_->stop();
    refreshTimer_->stop();
    callbackServer_->close();
    codeVerifier_.clear();
    state_.clear();
    redirectUri_.clear();
    accessToken_.clear();
    refreshToken_.clear();
    connected_ = false;
    playbackPollTimer_->stop();
    stopPlaybackCompanion();
    clearApiCache();
    resetImageMemoryCache();
    playbackServer_->close();
    playbackKey_.clear();
    playbackDeviceId_.clear();
    pendingPlaybackUris_.clear();
    pendingPlaybackContextUri_.clear();
    pendingPlaybackOffsetUri_.clear();
    pendingPlaybackPositionMs_ = -1;
    playbackPageOpened_ = false;
    playbackState_ = {};
    localPlaybackState_ = {};
    apiPlaybackState_ = {};
    apiPlaybackStateKnown_ = false;
    currentUserDisplayName_.clear();
    currentUserImageUrl_.clear();
    availableDevices_.clear();
    availableDevicesError_.clear();
    playbackScopeWarningShown_ = false;
    notifyDevicesUpdated();
    notifyPlaybackState();
    if (playbackStatusHandler_) {
        playbackStatusHandler_(QStringLiteral("Connect Spotify to enable playback."));
    }
    if (QFile::exists(sessionFilePath()) && !QFile::remove(sessionFilePath()) && errorHandler_) {
        errorHandler_(QStringLiteral("Spotify was disconnected, but its saved AppData session could not be removed."));
    }
    setState("Spotify not connected", false, false);
}

bool SpotifyConnection::isConnected() const
{
    return connected_;
}

void SpotifyConnection::invalidateLibraryCache()
{
    QFile::remove(libraryItemsCachePath());
}

void SpotifyConnection::searchCatalog(const QString &searchText, SearchHandler handler)
{
    const QString queryText = searchText.trimmed();
    if (!connected_ || accessToken_.isEmpty()) {
        handler({}, {}, QStringLiteral("Connect Spotify before searching."));
        return;
    }
    if (queryText.isEmpty()) {
        handler({}, {}, QStringLiteral("Enter a track, artist, or album name."));
        return;
    }

    QUrl url(QStringLiteral("https://api.spotify.com/v1/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), queryText);
    query.addQueryItem(QStringLiteral("type"), QStringLiteral("track,artist"));
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("10"));
    url.setQuery(query);

    if (apiRateLimitActive()) {
        handler({}, {}, apiRateLimitMessage());
        return;
    }
    QNetworkReply *reply = authenticatedGet(url, false, true);
    connect(reply, &QNetworkReply::finished, this, [reply, handler = std::move(handler)]() mutable {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
            QString message = replyError(reply, response);
            if (statusCode == 429) {
                message = apiFailureMessage(reply, response);
            } else if (message.isEmpty()) {
                message = QStringLiteral("Spotify returned HTTP %1.").arg(statusCode);
            }
            reply->deleteLater();
            handler({}, {}, message);
            return;
        }

        const QJsonArray items = response.value("tracks").toObject().value("items").toArray();
        QList<SpotifyTrackResult> tracks;
        tracks.reserve(items.size());
        for (const QJsonValue &itemValue : items) {
            SpotifyTrackResult track = parseTrack(itemValue.toObject());
            if (!track.name.isEmpty()) {
                tracks.append(track);
            }
        }

        QList<SpotifyArtistProfile> artists;
        const QJsonArray artistItems = response.value("artists").toObject()
                                           .value("items").toArray();
        artists.reserve(artistItems.size());
        for (const QJsonValue &artistValue : artistItems) {
            const QJsonObject object = artistValue.toObject();
            SpotifyArtistProfile artist;
            artist.id = object.value(QStringLiteral("id")).toString();
            artist.name = object.value(QStringLiteral("name")).toString();
            artist.imageUrl = bestImageUrl(object.value(QStringLiteral("images")).toArray(), 256);
            artist.spotifyUrl = object.value(QStringLiteral("external_urls")).toObject()
                                    .value(QStringLiteral("spotify")).toString();
            if (!artist.id.isEmpty() && !artist.name.isEmpty()) {
                artists.append(std::move(artist));
            }
        }

        reply->deleteLater();
        handler(tracks, artists, {});
    });
}

QNetworkReply *SpotifyConnection::authenticatedGet(const QUrl &url, bool cacheable,
                                                   bool forceNetwork)
{
    QNetworkRequest request(url);
    setRequestTimeout(request);
    request.setRawHeader("Authorization", "Bearer " + accessToken_);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         cacheable && !forceNetwork ? QNetworkRequest::PreferNetwork
                                                     : QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, cacheable);
    QNetworkReply *reply = networkManager_->get(request);
    observeApiReply(reply);
    return reply;
}

void SpotifyConnection::observeApiReply(QNetworkReply *reply)
{
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (statusCode == 429) {
            if (nowMs - lastApiRateLimitMs_ > 10 * 60 * 1000) {
                consecutiveApiRateLimits_ = 0;
            }
            ++consecutiveApiRateLimits_;
            lastApiRateLimitMs_ = nowMs;

            const QJsonObject response = QJsonDocument::fromJson(reply->peek(8192)).object();
            apiRateLimitReason_ = response.value(QStringLiteral("error")).toObject()
                                      .value(QStringLiteral("reason")).toString();
            const int backoffSeconds = qMin(1800, 30 * (1 << qMin(5, consecutiveApiRateLimits_ - 1)));
            bool retryAfterOk = false;
            const int serverRetrySeconds = reply->rawHeader(QByteArrayLiteral("Retry-After"))
                                               .toInt(&retryAfterOk);
            int delaySeconds = retryAfterOk && serverRetrySeconds > 0
                ? serverRetrySeconds : backoffSeconds;
            if (apiRateLimitReason_ == QStringLiteral("QUOTA_EXCEEDED")
                && !retryAfterOk) {
                delaySeconds = qMax(delaySeconds, 300);
            }
            apiRateLimitUntilMs_ = qMax(apiRateLimitUntilMs_, nowMs + delaySeconds * 1000LL);

            if (!apiRateLimitStatusShown_ && playbackStatusHandler_) {
                apiRateLimitStatusShown_ = true;
                playbackStatusHandler_(apiRateLimitMessage());
            }
        } else if (statusCode >= 200 && statusCode < 300
                   && nowMs >= apiRateLimitUntilMs_) {
            consecutiveApiRateLimits_ = 0;
            apiRateLimitReason_.clear();
            apiRateLimitStatusShown_ = false;
        }
    });
}

bool SpotifyConnection::apiRateLimitActive() const
{
    return QDateTime::currentMSecsSinceEpoch() < apiRateLimitUntilMs_;
}

int SpotifyConnection::apiRateLimitRemainingSeconds() const
{
    const qint64 remainingMs = apiRateLimitUntilMs_ - QDateTime::currentMSecsSinceEpoch();
    return remainingMs > 0 ? static_cast<int>((remainingMs + 999) / 1000) : 0;
}

QString SpotifyConnection::apiRateLimitMessage() const
{
    if (apiRateLimitReason_ == QStringLiteral("QUOTA_EXCEEDED")) {
        return QStringLiteral("Spotify's Development Mode API quota is exhausted (429: QUOTA_EXCEEDED). Wait for Spotify to restore the quota, then try again.");
    }
    return QStringLiteral("Spotify rate limited Web API requests (HTTP 429). Automatic requests are paused for about %1 seconds.")
        .arg(apiRateLimitRemainingSeconds());
}

void SpotifyConnection::pollPlaybackState()
{
    if (!connected_ || accessToken_.isEmpty() || playbackPollInFlight_
        || apiRateLimitActive()) {
        return;
    }

    playbackPollInFlight_ = true;
    QNetworkReply *reply = authenticatedGet(
        QUrl(QStringLiteral("https://api.spotify.com/v1/me/player")), false, true);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        playbackPollInFlight_ = false;
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray payload = reply->readAll();
        if (statusCode == 204 && reply->error() == QNetworkReply::NoError) {
            apiPlaybackState_ = {};
            apiPlaybackStateKnown_ = true;
            updateEffectivePlaybackState();
        } else if (reply->error() == QNetworkReply::NoError
                   && statusCode >= 200 && statusCode < 300) {
            const QJsonObject response = QJsonDocument::fromJson(payload).object();
            const QJsonObject device = response.value(QStringLiteral("device")).toObject();
            const QJsonObject item = response.value(QStringLiteral("item")).toObject();
            SpotifyPlaybackState state;
            state.deviceId = device.value(QStringLiteral("id")).toString();
            state.deviceName = device.value(QStringLiteral("name")).toString();
            state.deviceType = device.value(QStringLiteral("type")).toString();
            state.repeatState = response.value(QStringLiteral("repeat_state"))
                                    .toString(QStringLiteral("off"));
            state.deviceRestricted = device.value(QStringLiteral("is_restricted")).toBool();
            state.supportsVolume = device.value(QStringLiteral("supports_volume")).toBool();
            if (device.value(QStringLiteral("volume_percent")).isDouble()) {
                state.deviceVolumePercent = device.value(QStringLiteral("volume_percent")).toInt();
            }
            state.positionMs = qMax(0, response.value(QStringLiteral("progress_ms")).toInt());
            state.paused = !response.value(QStringLiteral("is_playing")).toBool();
            state.active = !item.isEmpty() || response.value(QStringLiteral("is_playing")).toBool();
            state.trackName = item.value(QStringLiteral("name")).toString();
            state.trackUri = item.value(QStringLiteral("uri")).toString();
            state.durationMs = qMax(0, item.value(QStringLiteral("duration_ms")).toInt());
            state.album = item.value(QStringLiteral("album")).toObject()
                              .value(QStringLiteral("name")).toString();
            state.albumId = item.value(QStringLiteral("album")).toObject()
                                .value(QStringLiteral("id")).toString();
            state.artworkUrl = bestImageUrl(
                item.value(QStringLiteral("album")).toObject()
                    .value(QStringLiteral("images")).toArray(), 300);
            if (state.album.isEmpty()) {
                const QJsonObject show = item.value(QStringLiteral("show")).toObject();
                state.album = show.value(QStringLiteral("name")).toString();
                state.artworkUrl = bestImageUrl(
                    show.value(QStringLiteral("images")).toArray(), 300);
            }
            QStringList artistNames;
            const QJsonArray artists = item.value(QStringLiteral("artists")).toArray();
            for (const QJsonValue &artist : artists) {
                const QJsonObject artistObject = artist.toObject();
                const QString name = artistObject.value(QStringLiteral("name")).toString();
                if (!name.isEmpty()) {
                    artistNames.append(name);
                }
                if (state.artistId.isEmpty()) {
                    state.artistId = artistObject.value(QStringLiteral("id")).toString();
                }
            }
            state.artists = artistNames.join(QStringLiteral(", "));
            state.externalDevice = !state.deviceId.isEmpty()
                && state.deviceId != playbackDeviceId_;
            apiPlaybackState_ = std::move(state);
            apiPlaybackStateKnown_ = true;
            updateEffectivePlaybackState();
        } else if (statusCode == 403 && !playbackScopeWarningShown_) {
            playbackScopeWarningShown_ = true;
            if (playbackStatusHandler_) {
                playbackStatusHandler_(QStringLiteral(
                    "Spotify playback-state access needs approval. Open Settings and reconnect to grant it."));
            }
        }
        reply->deleteLater();

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - lastDevicesRefreshMs_ >= 30000) {
            refreshAvailableDevices();
        }
    });
}

void SpotifyConnection::updateEffectivePlaybackState()
{
    const bool apiHasRemoteDevice = apiPlaybackStateKnown_
        && !apiPlaybackState_.deviceId.isEmpty()
        && apiPlaybackState_.deviceId != playbackDeviceId_;
    const bool apiHasUnidentifiedRemotePlayback = apiPlaybackStateKnown_
        && apiPlaybackState_.deviceId.isEmpty()
        && apiPlaybackState_.active
        && !localPlaybackState_.active;

    if (apiHasRemoteDevice || apiHasUnidentifiedRemotePlayback) {
        playbackState_ = apiPlaybackState_;
        playbackState_.externalDevice = true;
        playbackState_.deviceReady = localPlaybackState_.deviceReady;
        playbackState_.audioActivated = localPlaybackState_.audioActivated;
        notifyPlaybackState();
        return;
    }

    playbackState_ = localPlaybackState_;
    if (apiPlaybackStateKnown_ && apiPlaybackState_.deviceId == playbackDeviceId_
        && !playbackDeviceId_.isEmpty()) {
        playbackState_.deviceId = apiPlaybackState_.deviceId;
        playbackState_.deviceName = apiPlaybackState_.deviceName;
        playbackState_.deviceType = apiPlaybackState_.deviceType;
        playbackState_.deviceVolumePercent = localPlaybackState_.deviceVolumePercent >= 0
            ? localPlaybackState_.deviceVolumePercent : apiPlaybackState_.deviceVolumePercent;
        playbackState_.deviceRestricted = apiPlaybackState_.deviceRestricted;
        playbackState_.supportsVolume = apiPlaybackState_.supportsVolume;
        if (!playbackState_.active && apiPlaybackState_.active) {
            playbackState_.active = true;
            playbackState_.paused = apiPlaybackState_.paused;
            playbackState_.positionMs = apiPlaybackState_.positionMs;
            playbackState_.durationMs = apiPlaybackState_.durationMs;
            playbackState_.trackName = apiPlaybackState_.trackName;
            playbackState_.trackUri = apiPlaybackState_.trackUri;
            playbackState_.artists = apiPlaybackState_.artists;
            playbackState_.artistId = apiPlaybackState_.artistId;
            playbackState_.album = apiPlaybackState_.album;
            playbackState_.albumId = apiPlaybackState_.albumId;
            playbackState_.artworkUrl = apiPlaybackState_.artworkUrl;
        }
    }
    if (playbackState_.active && playbackState_.albumId.isEmpty()
        && apiPlaybackStateKnown_
        && apiPlaybackState_.trackUri == playbackState_.trackUri) {
        playbackState_.albumId = apiPlaybackState_.albumId;
    }
    if (apiPlaybackStateKnown_) {
        playbackState_.repeatState = apiPlaybackState_.repeatState;
    }
    playbackState_.externalDevice = false;
    notifyPlaybackState();
}

QString SpotifyConnection::metadataCachePath(const QString &cacheKey) const
{
    const QString cacheBase = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheBase.isEmpty()) return {};
    const QByteArray hash = QCryptographicHash::hash(cacheKey.toUtf8(),
                                                      QCryptographicHash::Sha256).toHex();
    return QDir(QDir(cacheBase).filePath(QStringLiteral("spotify-metadata")))
        .filePath(QString::fromLatin1(hash) + QStringLiteral(".json"));
}

bool SpotifyConnection::loadMetadataFromDisk(const QString &cacheKey, QJsonObject *value,
                                             qint64 *expiresAtMs) const
{
    if (!value) return false;
    QFile file(metadataCachePath(cacheKey));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return false;
    const QJsonObject root = document.object();
    const QJsonObject payload = root.value(QStringLiteral("value")).toObject();
    if (payload.isEmpty()) return false;
    *value = payload;
    if (expiresAtMs) {
        *expiresAtMs = static_cast<qint64>(
            root.value(QStringLiteral("expires_at_ms")).toDouble());
    }
    return true;
}

void SpotifyConnection::saveMetadataToDisk(const QString &cacheKey,
                                           const QJsonObject &value,
                                           qint64 lifetimeMs) const
{
    const QString path = metadataCachePath(cacheKey);
    if (path.isEmpty() || value.isEmpty()) return;
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) return;
    QJsonObject root;
    root.insert(QStringLiteral("expires_at_ms"), static_cast<double>(
        QDateTime::currentMSecsSinceEpoch() + lifetimeMs));
    root.insert(QStringLiteral("value"), value);
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

QString SpotifyConnection::lyricsCachePath() const
{
    const QString cacheBase = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheBase.isEmpty()) {
        return {};
    }
    return QDir(cacheBase).filePath(QStringLiteral("spotify-lyrics.json"));
}

void SpotifyConnection::loadLyricsCache()
{
    QFile file(lyricsCachePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const QJsonObject entries = document.object().value(QStringLiteral("entries")).toObject();
    for (auto entry = entries.constBegin(); entry != entries.constEnd(); ++entry) {
        const QJsonObject object = entry.value().toObject();
        CachedLyrics cached;
        cached.cachedAtMs = static_cast<qint64>(
            object.value(QStringLiteral("cached_at_ms")).toDouble());
        cached.expiresAtMs = static_cast<qint64>(
            object.value(QStringLiteral("expires_at_ms")).toDouble());
        if (cached.expiresAtMs <= nowMs) {
            continue;
        }
        cached.lyrics.plainLyrics = object.value(QStringLiteral("plain_lyrics")).toString();
        cached.lyrics.syncedLyrics = object.value(QStringLiteral("synced_lyrics")).toString();
        cached.lyrics.instrumental = object.value(QStringLiteral("instrumental")).toBool();
        cached.errorMessage = object.value(QStringLiteral("error")).toString();
        lyricsCache_.insert(entry.key(), std::move(cached));
    }
}

void SpotifyConnection::saveLyricsCache()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (auto entry = lyricsCache_.begin(); entry != lyricsCache_.end();) {
        if (entry->expiresAtMs <= nowMs) {
            entry = lyricsCache_.erase(entry);
        } else {
            ++entry;
        }
    }

    const QString path = lyricsCachePath();
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) {
        return;
    }

    QJsonObject entries;
    for (auto entry = lyricsCache_.constBegin(); entry != lyricsCache_.constEnd(); ++entry) {
        QJsonObject object;
        object.insert(QStringLiteral("plain_lyrics"), entry->lyrics.plainLyrics);
        object.insert(QStringLiteral("synced_lyrics"), entry->lyrics.syncedLyrics);
        object.insert(QStringLiteral("instrumental"), entry->lyrics.instrumental);
        object.insert(QStringLiteral("error"), entry->errorMessage);
        object.insert(QStringLiteral("cached_at_ms"), static_cast<double>(entry->cachedAtMs));
        object.insert(QStringLiteral("expires_at_ms"), static_cast<double>(entry->expiresAtMs));
        entries.insert(entry.key(), object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("entries"), entries);
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

QString SpotifyConnection::trackPageCachePath(const QString &cacheKey) const
{
    const QString cacheBase = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheBase.isEmpty()) {
        return {};
    }
    const QString directory = QDir(cacheBase).filePath(QStringLiteral("spotify-track-pages"));
    const QByteArray keyHash = QCryptographicHash::hash(cacheKey.toUtf8(),
                                                         QCryptographicHash::Sha256).toHex();
    return QDir(directory).filePath(QString::fromLatin1(keyHash) + QStringLiteral(".json"));
}

bool SpotifyConnection::loadTrackPageFromDisk(const QString &cacheKey,
                                              CachedTrackPage *page) const
{
    if (!page) {
        return false;
    }
    const QString path = trackPageCachePath(cacheKey);
    if (path.isEmpty()) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject root = document.object();
    CachedTrackPage cached;
    cached.expiresAtMs = static_cast<qint64>(root.value(QStringLiteral("expires_at_ms")).toDouble());
    cached.retryAfterMs = static_cast<qint64>(root.value(QStringLiteral("retry_after_ms")).toDouble());
    if (!document.isObject()) {
        return false;
    }
    cached.nextOffset = root.value(QStringLiteral("next_offset")).toInt();
    cached.total = root.value(QStringLiteral("total")).toInt();
    const QJsonArray tracks = root.value(QStringLiteral("tracks")).toArray();
    cached.tracks.reserve(tracks.size());
    for (const QJsonValue &value : tracks) {
        const QJsonObject object = value.toObject();
        SpotifyTrackResult track;
        track.name = object.value(QStringLiteral("name")).toString();
        track.artists = object.value(QStringLiteral("artists")).toString();
        track.artistId = object.value(QStringLiteral("artist_id")).toString();
        track.album = object.value(QStringLiteral("album")).toString();
        track.albumId = object.value(QStringLiteral("album_id")).toString();
        track.albumImageUrl = object.value(QStringLiteral("album_image_url")).toString();
        track.addedAt = object.value(QStringLiteral("added_at")).toString();
        track.spotifyUri = object.value(QStringLiteral("uri")).toString();
        track.spotifyUrl = object.value(QStringLiteral("url")).toString();
        track.durationMs = object.value(QStringLiteral("duration_ms")).toInt();
        cached.tracks.append(std::move(track));
    }
    *page = std::move(cached);
    return true;
}

void SpotifyConnection::saveTrackPageToDisk(const QString &cacheKey,
                                            const CachedTrackPage &page) const
{
    const QString path = trackPageCachePath(cacheKey);
    if (path.isEmpty()) {
        return;
    }
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        return;
    }

    QJsonArray tracks;
    for (const SpotifyTrackResult &track : page.tracks) {
        QJsonObject object;
        object.insert(QStringLiteral("name"), track.name);
        object.insert(QStringLiteral("artists"), track.artists);
        object.insert(QStringLiteral("artist_id"), track.artistId);
        object.insert(QStringLiteral("album"), track.album);
        object.insert(QStringLiteral("album_id"), track.albumId);
        object.insert(QStringLiteral("album_image_url"), track.albumImageUrl);
        object.insert(QStringLiteral("added_at"), track.addedAt);
        object.insert(QStringLiteral("uri"), track.spotifyUri);
        object.insert(QStringLiteral("url"), track.spotifyUrl);
        object.insert(QStringLiteral("duration_ms"), track.durationMs);
        tracks.append(object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("cache_key"), cacheKey);
    root.insert(QStringLiteral("expires_at_ms"), static_cast<double>(page.expiresAtMs));
    root.insert(QStringLiteral("retry_after_ms"), static_cast<double>(page.retryAfterMs));
    root.insert(QStringLiteral("next_offset"), page.nextOffset);
    root.insert(QStringLiteral("total"), page.total);
    root.insert(QStringLiteral("tracks"), tracks);

    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    QDir cacheDirectory(directory);
    QFileInfoList candidates;
    qint64 usedBytes = 0;
    for (const QFileInfo &entry : cacheDirectory.entryInfoList(
             {QStringLiteral("*.json")}, QDir::Files, QDir::Time)) {
        if (entry.absoluteFilePath() == path) {
            continue;
        }
        usedBytes += entry.size();
        candidates.append(entry);
    }
    std::sort(candidates.begin(), candidates.end(), [](const QFileInfo &left,
                                                       const QFileInfo &right) {
        return left.lastModified() < right.lastModified();
    });
    for (const QFileInfo &entry : candidates) {
        if (usedBytes + payload.size() <= maxTrackPageCacheBytes) {
            break;
        }
        if (QFile::remove(entry.absoluteFilePath())) {
            usedBytes -= entry.size();
        }
    }

    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(payload);
        file.commit();
    }
}

QString SpotifyConnection::libraryItemsCachePath() const
{
    const QString cacheBase = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return cacheBase.isEmpty() ? QString{} : QDir(cacheBase).filePath(
        QStringLiteral("spotify-library-items.json"));
}

bool SpotifyConnection::loadLibraryItemsCache(QList<SpotifyLibraryItem> *items) const
{
    if (!items) return false;
    QFile file(libraryItemsCachePath());
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject() || !document.object().value(QStringLiteral("complete")).toBool()) {
        return false;
    }

    QList<SpotifyLibraryItem> cachedItems;
    const QJsonArray values = document.object().value(QStringLiteral("items")).toArray();
    cachedItems.reserve(values.size());
    for (const QJsonValue &value : values) {
        const QJsonObject object = value.toObject();
        SpotifyLibraryItem item;
        item.type = static_cast<SpotifyLibraryItemType>(
            object.value(QStringLiteral("type")).toInt());
        item.id = object.value(QStringLiteral("id")).toString();
        item.name = object.value(QStringLiteral("name")).toString();
        item.creator = object.value(QStringLiteral("creator")).toString();
        item.category = object.value(QStringLiteral("category")).toString();
        item.imageUrl = object.value(QStringLiteral("image_url")).toString();
        item.spotifyUri = object.value(QStringLiteral("uri")).toString();
        item.spotifyUrl = object.value(QStringLiteral("url")).toString();
        item.itemCount = object.value(QStringLiteral("item_count")).toInt();
        item.durationMs = object.value(QStringLiteral("duration_ms")).toInt();
        if (!item.id.isEmpty() && !item.name.isEmpty()) {
            cachedItems.append(std::move(item));
        }
    }
    *items = std::move(cachedItems);
    return true;
}

void SpotifyConnection::saveLibraryItemsCache(const QList<SpotifyLibraryItem> &items) const
{
    const QString path = libraryItemsCachePath();
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) return;

    QJsonArray values;
    for (const SpotifyLibraryItem &item : items) {
        QJsonObject object;
        object.insert(QStringLiteral("type"), static_cast<int>(item.type));
        object.insert(QStringLiteral("id"), item.id);
        object.insert(QStringLiteral("name"), item.name);
        object.insert(QStringLiteral("creator"), item.creator);
        object.insert(QStringLiteral("category"), item.category);
        object.insert(QStringLiteral("image_url"), item.imageUrl);
        object.insert(QStringLiteral("uri"), item.spotifyUri);
        object.insert(QStringLiteral("url"), item.spotifyUrl);
        object.insert(QStringLiteral("item_count"), item.itemCount);
        object.insert(QStringLiteral("duration_ms"), item.durationMs);
        values.append(object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("complete"), true);
    root.insert(QStringLiteral("items"), values);
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

void SpotifyConnection::invalidateSavedTracksPageCache()
{
    QSet<QString> savedTrackCacheKeys;
    for (auto entry = trackPageCache_.begin(); entry != trackPageCache_.end();) {
        if (entry.key().contains(QStringLiteral("/me/tracks?"))) {
            savedTrackCacheKeys.insert(entry.key());
            QFile::remove(trackPageCachePath(entry.key()));
            entry = trackPageCache_.erase(entry);
        } else {
            ++entry;
        }
    }

    const QString cacheBase = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir directory(QDir(cacheBase).filePath(QStringLiteral("spotify-track-pages")));
    for (const QFileInfo &entry : directory.entryInfoList(
             {QStringLiteral("*.json")}, QDir::Files)) {
        QFile file(entry.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QString cacheKey = root.value(QStringLiteral("cache_key")).toString();
        if (cacheKey.isEmpty()) {
            const int offset = root.value(QStringLiteral("next_offset")).toInt()
                - root.value(QStringLiteral("tracks")).toArray().size();
            if (offset >= 0) {
                QUrl pageUrl(QStringLiteral("https://api.spotify.com/v1/me/tracks"));
                QUrlQuery query;
                query.addQueryItem(QStringLiteral("limit"), QStringLiteral("50"));
                query.addQueryItem(QStringLiteral("offset"), QString::number(offset));
                pageUrl.setQuery(query);
                const QString candidate = pageUrl.toString(QUrl::FullyEncoded);
                if (QDir::cleanPath(trackPageCachePath(candidate)).compare(
                        QDir::cleanPath(entry.absoluteFilePath()), Qt::CaseInsensitive) == 0) {
                    cacheKey = candidate;
                }
            }
        }
        if (!cacheKey.contains(QStringLiteral("/me/tracks?"))) continue;

        savedTrackCacheKeys.insert(cacheKey);
        QFile::remove(entry.absoluteFilePath());
    }
    if (httpCache_) {
        for (const QString &cacheKey : std::as_const(savedTrackCacheKeys)) {
            httpCache_->remove(QUrl(cacheKey));
        }
    }
}

void SpotifyConnection::fetchCurrentUserPlaylists(PlaylistsHandler handler, bool forceNetwork)
{
    if (!connected_ || accessToken_.isEmpty()) {
        handler({}, QStringLiteral("Connect Spotify before loading playlists."));
        return;
    }
    if (apiRateLimitActive()) {
        handler({}, apiRateLimitMessage());
        return;
    }
    fetchPlaylistPage(0, {}, std::move(handler), forceNetwork);
}

void SpotifyConnection::fetchCurrentUserLibraryItems(LibraryItemsHandler handler,
                                                     bool forceNetwork)
{
    if (!connected_ || accessToken_.isEmpty()) {
        handler({}, QStringLiteral("Connect Spotify before loading your library."));
        return;
    }
    if (apiRateLimitActive()) {
        handler({}, apiRateLimitMessage());
        return;
    }
    if (!forceNetwork) {
        QList<SpotifyLibraryItem> cachedItems;
        if (loadLibraryItemsCache(&cachedItems)) {
            QTimer::singleShot(0, this,
                               [handler = std::move(handler), cachedItems]() mutable {
                handler(cachedItems, {});
            });
            return;
        }
    }

    struct Aggregate
    {
        QList<SpotifyLibraryItem> items;
        QStringList errors;
        int remaining = 4;
        LibraryItemsHandler handler;
    };
    auto aggregate = std::make_shared<Aggregate>();
    aggregate->handler = std::move(handler);
    const auto collect = [this, aggregate](const QList<SpotifyLibraryItem> &items,
                                           const QString &error) {
        aggregate->items.append(items);
        if (!error.isEmpty()) {
            aggregate->errors.append(error);
        }
        if (--aggregate->remaining == 0 && aggregate->handler) {
            auto completion = std::move(aggregate->handler);
            if (aggregate->errors.isEmpty()) {
                saveLibraryItemsCache(aggregate->items);
            }
            completion(aggregate->items, aggregate->errors.join(QStringLiteral("\n")));
        }
    };

    fetchLibraryItemPage(QStringLiteral("me/playlists"), SpotifyLibraryItemType::Playlist,
                         0, {}, collect, forceNetwork);
    fetchLibraryItemPage(QStringLiteral("me/albums"), SpotifyLibraryItemType::Album,
                         0, {}, collect, forceNetwork);
    fetchLibraryItemPage(QStringLiteral("me/episodes"), SpotifyLibraryItemType::Episode,
                         0, {}, collect, forceNetwork);
    fetchLibraryItemPage(QStringLiteral("me/shows"), SpotifyLibraryItemType::Show,
                         0, {}, collect, forceNetwork);
}

void SpotifyConnection::checkSavedTracksChanged(const QString &currentTrackUri,
                                                SavedTracksChangeHandler handler)
{
    if (!connected_ || accessToken_.isEmpty()) {
        if (handler) handler(false, QStringLiteral("Connect Spotify before checking saved tracks."));
        return;
    }
    if (apiRateLimitActive()) {
        if (handler) handler(false, apiRateLimitMessage());
        return;
    }

    constexpr int spotifyPageSize = 50;
    constexpr int sampleDistance = 20;
    const auto pageKeyForOffset = [](int offset) {
        QUrl pageUrl(QStringLiteral("https://api.spotify.com/v1/me/tracks"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("limit"), QString::number(spotifyPageSize));
        query.addQueryItem(QStringLiteral("offset"), QString::number(offset));
        pageUrl.setQuery(query);
        return pageUrl.toString(QUrl::FullyEncoded);
    };
    const auto loadCachedPage = [this, &pageKeyForOffset](int offset,
                                                          CachedTrackPage *page) {
        if (!page) return false;
        const QString cacheKey = pageKeyForOffset((offset / spotifyPageSize) * spotifyPageSize);
        const auto cached = trackPageCache_.constFind(cacheKey);
        if (cached != trackPageCache_.cend()) {
            *page = cached.value();
            return true;
        }
        return loadTrackPageFromDisk(cacheKey, page);
    };

    CachedTrackPage firstPage;
    if (!loadCachedPage(0, &firstPage)) {
        // Without a local baseline, the visible page load will fetch only what is needed.
        if (handler) handler(false, {});
        return;
    }

    const int cachedTotal = qMax(0, firstPage.total);
    int anchorOffset = 0;
    if (!currentTrackUri.isEmpty()) {
        for (int pageOffset = 0; pageOffset < cachedTotal; pageOffset += spotifyPageSize) {
            CachedTrackPage page;
            if (!loadCachedPage(pageOffset, &page)) continue;
            for (int index = 0; index < page.tracks.size(); ++index) {
                if (page.tracks.at(index).spotifyUri == currentTrackUri) {
                    anchorOffset = pageOffset + index;
                    pageOffset = cachedTotal;
                    break;
                }
            }
        }
    }

    QList<int> sampleOffsets;
    if (cachedTotal > 0) {
        sampleOffsets.append(anchorOffset);
        if (anchorOffset >= sampleDistance) sampleOffsets.append(anchorOffset - sampleDistance);
        if (anchorOffset + sampleDistance < cachedTotal) {
            sampleOffsets.append(anchorOffset + sampleDistance);
        }
        std::sort(sampleOffsets.begin(), sampleOffsets.end());
        sampleOffsets.erase(std::unique(sampleOffsets.begin(), sampleOffsets.end()),
                            sampleOffsets.end());
    }

    QHash<int, QPair<QString, QString>> cachedSamples;
    for (const int sampleOffset : std::as_const(sampleOffsets)) {
        CachedTrackPage page;
        if (!loadCachedPage(sampleOffset, &page)) {
            // An uncached sample cannot establish that the saved list is unchanged.
            if (handler) handler(false, {});
            return;
        }
        if (page.total != cachedTotal) {
            invalidateSavedTracksPageCache();
            if (handler) handler(true, {});
            return;
        }
        const int pageIndex = sampleOffset % spotifyPageSize;
        if (pageIndex < 0 || pageIndex >= page.tracks.size()) {
            if (handler) handler(false, {});
            return;
        }
        const SpotifyTrackResult &track = page.tracks.at(pageIndex);
        cachedSamples.insert(sampleOffset, {track.spotifyUri, track.addedAt});
    }

    const int requestOffset = sampleOffsets.isEmpty() ? 0 : sampleOffsets.first();
    const int requestEnd = sampleOffsets.isEmpty() ? 0 : sampleOffsets.last();
    const int requestLimit = sampleOffsets.isEmpty()
        ? 1 : requestEnd - requestOffset + 1;

    const auto renewCachedSavedPages = [this, pageKeyForOffset](qint64 expiresAtMs,
                                                                qint64 retryAfterMs) {
        for (auto page = trackPageCache_.begin(); page != trackPageCache_.end(); ++page) {
            if (!page.key().contains(QStringLiteral("/me/tracks?"))) continue;
            page->expiresAtMs = expiresAtMs;
            page->retryAfterMs = retryAfterMs;
        }

        const QString cacheBase = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir directory(QDir(cacheBase).filePath(QStringLiteral("spotify-track-pages")));
        for (const QFileInfo &entry : directory.entryInfoList(
                 {QStringLiteral("*.json")}, QDir::Files)) {
            QFile file(entry.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly)) continue;
            QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
            QString cacheKey = root.value(QStringLiteral("cache_key")).toString();
            if (cacheKey.isEmpty()) {
                const int offset = root.value(QStringLiteral("next_offset")).toInt()
                    - root.value(QStringLiteral("tracks")).toArray().size();
                if (offset >= 0) {
                    const QString candidate = pageKeyForOffset(offset);
                    if (QDir::cleanPath(trackPageCachePath(candidate)).compare(
                            QDir::cleanPath(entry.absoluteFilePath()),
                            Qt::CaseInsensitive) == 0) {
                        cacheKey = candidate;
                    }
                }
            }
            if (!cacheKey.contains(QStringLiteral("/me/tracks?"))) {
                continue;
            }
            root.insert(QStringLiteral("cache_key"), cacheKey);
            root.insert(QStringLiteral("expires_at_ms"), static_cast<double>(expiresAtMs));
            root.insert(QStringLiteral("retry_after_ms"), static_cast<double>(retryAfterMs));
            QSaveFile updatedFile(entry.absoluteFilePath());
            if (updatedFile.open(QIODevice::WriteOnly)) {
                updatedFile.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
                updatedFile.commit();
            }
        }
    };

    QUrl url(QStringLiteral("https://api.spotify.com/v1/me/tracks"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("limit"), QString::number(requestLimit));
    query.addQueryItem(QStringLiteral("offset"), QString::number(requestOffset));
    url.setQuery(query);
    QNetworkReply *reply = authenticatedGet(url, false, true);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, cachedTotal, cachedSamples, requestOffset,
             renewCachedSavedPages,
             handler = std::move(handler)]() mutable {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
            const QString error = apiFailureMessage(reply, response);
            reply->deleteLater();
            const qint64 retryAfterMs = QDateTime::currentMSecsSinceEpoch()
                + spotifyTrackPageRetryDelayMs;
            renewCachedSavedPages(retryAfterMs, retryAfterMs);
            if (handler) handler(false, error);
            return;
        }

        const int remoteTotal = qMax(0, response.value(QStringLiteral("total")).toInt());
        const QJsonArray items = response.value(QStringLiteral("items")).toArray();
        bool changed = cachedTotal != remoteTotal;
        for (auto sample = cachedSamples.cbegin(); sample != cachedSamples.cend(); ++sample) {
            const int itemIndex = sample.key() - requestOffset;
            if (itemIndex < 0 || itemIndex >= items.size()) {
                changed = true;
                continue;
            }
            const QJsonObject savedItem = items.at(itemIndex).toObject();
            QJsonObject track = savedItem.value(QStringLiteral("item")).toObject();
            if (track.isEmpty()) track = savedItem.value(QStringLiteral("track")).toObject();
            const QString remoteUri = track.value(QStringLiteral("uri")).toString();
            const QString remoteAddedAt = savedItem.value(QStringLiteral("added_at")).toString();
            if (sample.value().first != remoteUri || sample.value().second != remoteAddedAt) {
                changed = true;
            }
        }
        if (changed) {
            invalidateSavedTracksPageCache();
        } else {
            renewCachedSavedPages(QDateTime::currentMSecsSinceEpoch()
                                      + spotifyTrackPageCacheLifetimeMs,
                                  0);
        }
        reply->deleteLater();
        if (handler) handler(changed, {});
    });
}

void SpotifyConnection::fetchLibraryItemPage(const QString &endpoint,
                                             SpotifyLibraryItemType type, int offset,
                                             QList<SpotifyLibraryItem> items,
                                             LibraryItemsHandler handler, bool forceNetwork)
{
    if (apiRateLimitActive()) {
        handler(items, apiRateLimitMessage());
        return;
    }

    QUrl url(QStringLiteral("https://api.spotify.com/v1/%1").arg(endpoint));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("50"));
    query.addQueryItem(QStringLiteral("offset"), QString::number(offset));
    url.setQuery(query);

    QNetworkReply *reply = authenticatedGet(url, true, forceNetwork);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, endpoint, type, offset, items = std::move(items),
             handler = std::move(handler), forceNetwork]() mutable {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
            const QString message = statusCode == 403
                ? QStringLiteral("Spotify denied access to part of your library (HTTP 403). Reconnect from Settings and approve library access.")
                : apiFailureMessage(reply, response);
            reply->deleteLater();
            handler(items, message);
            return;
        }

        const QJsonArray responseItems = response.value(QStringLiteral("items")).toArray();
        for (const QJsonValue &value : responseItems) {
            const QJsonObject wrapper = value.toObject();
            QJsonObject object = wrapper;
            QString objectKey;
            switch (type) {
            case SpotifyLibraryItemType::Album: objectKey = QStringLiteral("album"); break;
            case SpotifyLibraryItemType::Episode: objectKey = QStringLiteral("episode"); break;
            case SpotifyLibraryItemType::Show: objectKey = QStringLiteral("show"); break;
            case SpotifyLibraryItemType::LikedSongs: break;
            case SpotifyLibraryItemType::Playlist: break;
            }
            if (!objectKey.isEmpty()) {
                object = wrapper.value(objectKey).toObject();
            }

            SpotifyLibraryItem item;
            item.type = type;
            item.id = object.value(QStringLiteral("id")).toString();
            item.name = object.value(QStringLiteral("name")).toString();
            item.spotifyUri = object.value(QStringLiteral("uri")).toString();
            item.spotifyUrl = object.value(QStringLiteral("external_urls")).toObject()
                                  .value(QStringLiteral("spotify")).toString();
            item.imageUrl = bestImageUrl(object.value(QStringLiteral("images")).toArray(), 64);

            switch (type) {
            case SpotifyLibraryItemType::Playlist: {
                item.category = QStringLiteral("Playlist");
                const QJsonObject owner = object.value(QStringLiteral("owner")).toObject();
                item.creator = owner.value(QStringLiteral("display_name")).toString();
                if (item.creator.isEmpty()) {
                    item.creator = owner.value(QStringLiteral("id")).toString();
                }
                item.itemCount = object.value(QStringLiteral("items")).toObject()
                                     .value(QStringLiteral("total")).toInt(
                                         object.value(QStringLiteral("tracks")).toObject()
                                             .value(QStringLiteral("total")).toInt());
                break;
            }
            case SpotifyLibraryItemType::Album: {
                const QString albumType = object.value(QStringLiteral("album_type")).toString();
                item.category = albumType == QStringLiteral("single")
                    ? QStringLiteral("Single / EP")
                    : albumType == QStringLiteral("compilation")
                        ? QStringLiteral("Compilation") : QStringLiteral("Album");
                QStringList artists;
                for (const QJsonValue &artistValue : object.value(QStringLiteral("artists")).toArray()) {
                    const QString name = artistValue.toObject().value(QStringLiteral("name")).toString();
                    if (!name.isEmpty()) artists.append(name);
                }
                item.creator = artists.join(QStringLiteral(", "));
                item.itemCount = object.value(QStringLiteral("total_tracks")).toInt();
                break;
            }
            case SpotifyLibraryItemType::Episode: {
                item.category = QStringLiteral("Episode");
                const QJsonObject show = object.value(QStringLiteral("show")).toObject();
                item.creator = show.value(QStringLiteral("name")).toString();
                if (item.imageUrl.isEmpty()) {
                    item.imageUrl = bestImageUrl(show.value(QStringLiteral("images")).toArray(), 64);
                }
                item.durationMs = object.value(QStringLiteral("duration_ms")).toInt();
                break;
            }
            case SpotifyLibraryItemType::Show:
                item.category = QStringLiteral("Podcast");
                item.creator = object.value(QStringLiteral("publisher")).toString();
                item.itemCount = object.value(QStringLiteral("total_episodes")).toInt();
                break;
            case SpotifyLibraryItemType::LikedSongs:
                break;
            }
            if (!item.id.isEmpty() && !item.name.isEmpty()) {
                items.append(std::move(item));
            }
        }

        const bool hasNextPage = !response.value(QStringLiteral("next")).toString().isEmpty();
        reply->deleteLater();
        if (hasNextPage && !responseItems.isEmpty()) {
            fetchLibraryItemPage(endpoint, type, offset + responseItems.size(), std::move(items),
                                 std::move(handler), forceNetwork);
            return;
        }
        handler(items, {});
    });
}

void SpotifyConnection::fetchPlaylistPage(int offset,
                                          QList<SpotifyPlaylistResult> playlists,
                                          PlaylistsHandler handler,
                                          bool forceNetwork)
{
    if (apiRateLimitActive()) {
        handler(playlists, apiRateLimitMessage());
        return;
    }

    QUrl url(QStringLiteral("https://api.spotify.com/v1/me/playlists"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("50"));
    query.addQueryItem(QStringLiteral("offset"), QString::number(offset));
    url.setQuery(query);

    QNetworkReply *reply = authenticatedGet(url, true, forceNetwork);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, offset, playlists = std::move(playlists),
             handler = std::move(handler), forceNetwork]() mutable {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
            const QString message = statusCode == 403
                ? QStringLiteral("Spotify denied playlist access (HTTP 403). Reconnect from Settings and approve playlist access.")
                : apiFailureMessage(reply, response);
            reply->deleteLater();
            handler({}, message);
            return;
        }

        const QJsonArray items = response.value(QStringLiteral("items")).toArray();
        for (const QJsonValue &itemValue : items) {
            const QJsonObject item = itemValue.toObject();
            SpotifyPlaylistResult playlist;
            playlist.id = item.value(QStringLiteral("id")).toString();
            playlist.name = item.value(QStringLiteral("name")).toString();
            playlist.spotifyUrl = item.value(QStringLiteral("external_urls")).toObject()
                                      .value(QStringLiteral("spotify")).toString();
            playlist.imageUrl = bestImageUrl(item.value(QStringLiteral("images")).toArray(), 64);
            playlist.itemCount = item.value(QStringLiteral("items")).toObject()
                                     .value(QStringLiteral("total")).toInt(
                                         item.value(QStringLiteral("tracks")).toObject()
                                             .value(QStringLiteral("total")).toInt());
            if (!playlist.id.isEmpty() && !playlist.name.isEmpty()) {
                playlists.append(playlist);
            }
        }

        const bool hasNextPage = !response.value(QStringLiteral("next")).toString().isEmpty();
        reply->deleteLater();
        if (hasNextPage && !items.isEmpty()) {
            fetchPlaylistPage(offset + items.size(), std::move(playlists), std::move(handler),
                              forceNetwork);
            return;
        }
        handler(playlists, {});
    });
}

void SpotifyConnection::fetchSavedTracks(int offset, TracksPageHandler handler, bool forceNetwork)
{
    if (!connected_ || accessToken_.isEmpty()) {
        handler({}, offset, 0, QStringLiteral("Connect Spotify before loading your saved tracks."));
        return;
    }

    QUrl url(QStringLiteral("https://api.spotify.com/v1/me/tracks"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("50"));
    query.addQueryItem(QStringLiteral("offset"), QString::number(offset));
    url.setQuery(query);
    fetchTrackPage(url, offset,
                   [handler = std::move(handler)](const QList<SpotifyTrackResult> &tracks,
                                                  int nextOffset, int total,
                                                  const QString &error) mutable {
        const QString message = error.contains(QStringLiteral("HTTP 403"))
            ? QStringLiteral("Spotify denied access to your saved tracks (HTTP 403). Reconnect from Settings and approve library access.")
            : error;
        handler(tracks, nextOffset, total, message);
                   }, forceNetwork);
}

void SpotifyConnection::fetchPlaylistTracks(const QString &playlistId, int offset,
                                           TracksPageHandler handler, bool forceNetwork)
{
    if (!connected_ || accessToken_.isEmpty()) {
        handler({}, offset, 0, QStringLiteral("Connect Spotify before loading playlist tracks."));
        return;
    }
    if (playlistId.isEmpty()) {
        handler({}, offset, 0, QStringLiteral("Spotify returned an empty playlist ID."));
        return;
    }

    QUrl url(QStringLiteral("https://api.spotify.com/v1/playlists/%1/items")
                 .arg(QString::fromLatin1(QUrl::toPercentEncoding(playlistId))));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("50"));
    query.addQueryItem(QStringLiteral("offset"), QString::number(offset));
    url.setQuery(query);
    fetchTrackPage(url, offset,
                   [handler = std::move(handler)](const QList<SpotifyTrackResult> &tracks,
                                                  int nextOffset, int total,
                                                  const QString &error) mutable {
        const QString message = error.contains(QStringLiteral("HTTP 403"))
            ? QStringLiteral("Spotify only exposes items for playlists you own or collaborate on. If this playlist is yours, reconnect from Settings and approve library access.")
            : error;
        handler(tracks, nextOffset, total, message);
                   }, forceNetwork);
}

void SpotifyConnection::fetchArtistProfile(const QString &artistId,
                                           ArtistProfileHandler handler)
{
    if (!connected_ || accessToken_.isEmpty() || artistId.isEmpty()) {
        handler({}, QStringLiteral("Connect Spotify to load this artist profile."));
        return;
    }
    const QString cacheKey = QStringLiteral("artist-profile:%1").arg(artistId);
    QJsonObject cachedValue;
    qint64 expiresAtMs = 0;
    const bool hasCachedValue = loadMetadataFromDisk(cacheKey, &cachedValue, &expiresAtMs);
    if (hasCachedValue && expiresAtMs > QDateTime::currentMSecsSinceEpoch()) {
        const SpotifyArtistProfile artist = parseArtistProfile(cachedValue);
        QTimer::singleShot(0, this, [handler = std::move(handler), artist]() mutable {
            handler(artist, {});
        });
        return;
    }
    if (apiRateLimitActive()) {
        if (hasCachedValue) handler(parseArtistProfile(cachedValue), {});
        else handler({}, apiRateLimitMessage());
        return;
    }

    const QUrl url(QStringLiteral("https://api.spotify.com/v1/artists/%1")
                       .arg(QString::fromLatin1(QUrl::toPercentEncoding(artistId))));
    QNetworkReply *reply = authenticatedGet(url, false, true);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, cacheKey, hasCachedValue, cachedValue,
             handler = std::move(handler)]() mutable {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
            const QString error = apiFailureMessage(reply, response);
            reply->deleteLater();
            if (hasCachedValue) handler(parseArtistProfile(cachedValue), {});
            else handler({}, error);
            return;
        }

        saveMetadataToDisk(cacheKey, response, spotifyArtistMetadataCacheLifetimeMs);
        const SpotifyArtistProfile artist = parseArtistProfile(response);
        reply->deleteLater();
        handler(artist, {});
    });
}

void SpotifyConnection::fetchArtistAlbums(const QString &artistId, int offset,
                                          ArtistAlbumsHandler handler)
{
    if (!connected_ || accessToken_.isEmpty() || artistId.isEmpty()) {
        handler({}, offset, 0, false,
                QStringLiteral("Connect Spotify to load this artist's releases."));
        return;
    }
    const QString cacheKey = QStringLiteral("artist-albums:%1:%2")
                                 .arg(artistId).arg(qMax(0, offset));
    QJsonObject cachedValue;
    qint64 expiresAtMs = 0;
    const bool hasCachedValue = loadMetadataFromDisk(cacheKey, &cachedValue, &expiresAtMs);
    if (hasCachedValue && (expiresAtMs > QDateTime::currentMSecsSinceEpoch()
                           || apiRateLimitActive())) {
        const int total = qMax(0, cachedValue.value(QStringLiteral("total")).toInt());
        const int nextOffset = cachedValue.value(QStringLiteral("next_offset"))
                                   .toInt(offset + cachedValue.value(
                                       QStringLiteral("items")).toArray().size());
        const QList<SpotifyAlbumProfile> albums = parseArtistAlbumPage(cachedValue);
        QTimer::singleShot(0, this,
                           [handler = std::move(handler), albums, nextOffset, total]() mutable {
            handler(albums, nextOffset, total, true, {});
        });
        return;
    }
    if (apiRateLimitActive()) {
        handler({}, offset, 0, false, apiRateLimitMessage());
        return;
    }

    QUrl url(QStringLiteral("https://api.spotify.com/v1/artists/%1/albums")
                 .arg(QString::fromLatin1(QUrl::toPercentEncoding(artistId))));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("include_groups"),
                       QStringLiteral("album,single,compilation,appears_on"));
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("10"));
    query.addQueryItem(QStringLiteral("offset"), QString::number(qMax(0, offset)));
    url.setQuery(query);

    QNetworkReply *reply = authenticatedGet(url, false, true);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, cacheKey, hasCachedValue, cachedValue, offset,
             handler = std::move(handler)]() mutable {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
            const QString error = apiFailureMessage(reply, response);
            reply->deleteLater();
            if (hasCachedValue) {
                const int total = qMax(0, cachedValue.value(QStringLiteral("total")).toInt());
                const int nextOffset = cachedValue.value(QStringLiteral("next_offset"))
                    .toInt(offset + cachedValue.value(QStringLiteral("items")).toArray().size());
                handler(parseArtistAlbumPage(cachedValue), nextOffset, total, true, {});
            } else {
                handler({}, offset, 0, false, error);
            }
            return;
        }

        const QJsonArray items = response.value(QStringLiteral("items")).toArray();
        const int total = qMax(0, response.value(QStringLiteral("total")).toInt());
        const int nextOffset = items.isEmpty()
            || response.value(QStringLiteral("next")).toString().isEmpty()
            ? total : offset + items.size();
        QJsonObject cachedPage = response;
        cachedPage.insert(QStringLiteral("next_offset"), nextOffset);
        saveMetadataToDisk(cacheKey, cachedPage, spotifyArtistReleaseCacheLifetimeMs);
        const QList<SpotifyAlbumProfile> albums = parseArtistAlbumPage(response);
        reply->deleteLater();
        handler(albums, nextOffset, total, false, {});
    });
}

void SpotifyConnection::fetchAlbumProfile(const QString &albumId,
                                          AlbumProfileHandler handler)
{
    if (!connected_ || accessToken_.isEmpty() || albumId.isEmpty()) {
        handler({}, QStringLiteral("Connect Spotify to load this album profile."));
        return;
    }
    const QString cacheKey = QStringLiteral("album-profile:%1").arg(albumId);
    QJsonObject cachedValue;
    qint64 expiresAtMs = 0;
    const bool hasCachedValue = loadMetadataFromDisk(cacheKey, &cachedValue, &expiresAtMs);
    if (hasCachedValue && expiresAtMs > QDateTime::currentMSecsSinceEpoch()) {
        const SpotifyAlbumProfile album = parseAlbumProfile(cachedValue);
        QTimer::singleShot(0, this, [handler = std::move(handler), album]() mutable {
            handler(album, {});
        });
        return;
    }
    if (apiRateLimitActive()) {
        if (hasCachedValue) handler(parseAlbumProfile(cachedValue), {});
        else handler({}, apiRateLimitMessage());
        return;
    }

    const QUrl url(QStringLiteral("https://api.spotify.com/v1/albums/%1")
                       .arg(QString::fromLatin1(QUrl::toPercentEncoding(albumId))));
    QNetworkReply *reply = authenticatedGet(url, false, true);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, cacheKey, hasCachedValue, cachedValue,
             handler = std::move(handler)]() mutable {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
            const QString error = apiFailureMessage(reply, response);
            reply->deleteLater();
            if (hasCachedValue) handler(parseAlbumProfile(cachedValue), {});
            else handler({}, error);
            return;
        }
        saveMetadataToDisk(cacheKey, response, spotifyAlbumMetadataCacheLifetimeMs);
        const SpotifyAlbumProfile album = parseAlbumProfile(response);
        reply->deleteLater();
        handler(album, {});
    });
}

void SpotifyConnection::fetchAlbumTracks(const QString &albumId, int offset,
                                         TracksPageHandler handler)
{
    if (!connected_ || accessToken_.isEmpty() || albumId.isEmpty()) {
        handler({}, offset, 0, QStringLiteral("Connect Spotify to load this album's tracks."));
        return;
    }
    QUrl url(QStringLiteral("https://api.spotify.com/v1/albums/%1/tracks")
                 .arg(QString::fromLatin1(QUrl::toPercentEncoding(albumId))));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("50"));
    query.addQueryItem(QStringLiteral("offset"), QString::number(qMax(0, offset)));
    url.setQuery(query);
    fetchTrackPage(url, offset,
                   [albumId, handler = std::move(handler)](
                       const QList<SpotifyTrackResult> &tracks, int nextOffset,
                       int total, const QString &error) mutable {
        QList<SpotifyTrackResult> enrichedTracks = tracks;
        for (SpotifyTrackResult &track : enrichedTracks) track.albumId = albumId;
        handler(enrichedTracks, nextOffset, total, error);
    }, false, spotifyAlbumMetadataCacheLifetimeMs);
}

QString SpotifyConnection::imageCachePath(const QString &imageUrl) const
{
    const QString cacheBase = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheBase.isEmpty()) {
        return {};
    }
    const QByteArray key = QCryptographicHash::hash(imageUrl.toUtf8(),
                                                     QCryptographicHash::Sha256).toHex();
    return QDir(QDir(cacheBase).filePath(QStringLiteral("spotify-images")))
        .filePath(QString::fromLatin1(key) + QStringLiteral(".bin"));
}

bool SpotifyConnection::loadImageFromDisk(const QString &imageUrl, QImage *image,
                                          qint64 *expiresAtMs)
{
    if (!image || !expiresAtMs) {
        return false;
    }
    const QString path = imageCachePath(imageUrl);
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QDataStream stream(&file);
    qint64 cachedAtMs = 0;
    QByteArray data;
    stream >> cachedAtMs >> data;
    if (stream.status() != QDataStream::Ok || data.isEmpty()
        || cachedAtMs + spotifyImageCacheLifetimeMs <= QDateTime::currentMSecsSinceEpoch()) {
        file.close();
        removeImageCacheFile(path);
        return false;
    }

    QImage decoded;
    if (!decoded.loadFromData(data)) {
        file.close();
        removeImageCacheFile(path);
        return false;
    }
    *image = std::move(decoded);
    *expiresAtMs = cachedAtMs + spotifyImageCacheLifetimeMs;
    file.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
    return true;
}

void SpotifyConnection::pruneImageCache(const QString &newFilePath, qint64 newFileSize)
{
    const QString cacheBase = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheBase.isEmpty()) {
        return;
    }
    const QString directoryPath = QDir(cacheBase).filePath(QStringLiteral("spotify-images"));
    const qint64 maximumBytes = imageCacheMaxBytes_;
    QThreadPool::globalInstance()->start(QRunnable::create(
        [directoryPath, newFilePath, newFileSize, maximumBytes] {
            QMutexLocker locker(&spotifyImageCacheWriteMutex);
            pruneImageCacheFiles(directoryPath, newFilePath, newFileSize, maximumBytes);
        }));
}

void SpotifyConnection::saveImageToDisk(const QString &imageUrl, const QByteArray &data)
{
    const QString path = imageCachePath(imageUrl);
    if (path.isEmpty() || data.isEmpty()) {
        return;
    }
    const qint64 maximumBytes = imageCacheMaxBytes_;
    QThreadPool::globalInstance()->start(QRunnable::create(
        [path, data, maximumBytes] { writeImageCacheFile(path, data, maximumBytes); }));
}

void SpotifyConnection::setImageCacheMaxSizeMb(int megabytes)
{
    constexpr int minimumCacheSizeMb = 16;
    constexpr int maximumCacheSizeMb = 1024 * 1024;
    const int boundedMegabytes = std::clamp(megabytes, minimumCacheSizeMb,
                                             maximumCacheSizeMb);
    imageCacheMaxBytes_ = static_cast<qint64>(boundedMegabytes) * 1024 * 1024;
    pruneImageCache({}, 0);
}

void SpotifyConnection::fetchTrackLyrics(const QString &trackUri, const QString &trackName,
                                         const QString &artistName,
                                         const QString &albumName, int durationMs,
                                         LyricsHandler handler)
{
    const QString normalizedTrack = trackName.trimmed();
    const QString normalizedArtist = artistName.trimmed();
    if (normalizedTrack.isEmpty() || normalizedArtist.isEmpty()) {
        if (handler) {
            handler({}, QStringLiteral("Track details are not available for a lyrics lookup."));
        }
        return;
    }

    const int durationSeconds = durationMs > 0 ? (durationMs + 500) / 1000 : 0;
    const QString metadataIdentity = QStringList{
        normalizedTrack.toCaseFolded(), normalizedArtist.toCaseFolded(),
        albumName.trimmed().toCaseFolded(), QString::number(durationSeconds)
    }.join(QLatin1Char('\n'));
    const QString cacheIdentity = trackUri.trimmed().isEmpty()
        ? metadataIdentity : trackUri.trimmed().toCaseFolded();
    const QString cacheKey = QString::fromLatin1(QCryptographicHash::hash(
        cacheIdentity.toUtf8(), QCryptographicHash::Sha256).toHex());
    auto cached = lyricsCache_.find(cacheKey);
    if (cached != lyricsCache_.end()
        && cached->expiresAtMs > QDateTime::currentMSecsSinceEpoch()) {
        if (handler) {
            handler(cached->lyrics, cached->errorMessage);
        }
        return;
    }
    if (cached != lyricsCache_.end()) {
        lyricsCache_.erase(cached);
    }

    QUrl url(QStringLiteral("https://lrclib.net/api/get"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("track_name"), normalizedTrack);
    query.addQueryItem(QStringLiteral("artist_name"), normalizedArtist);
    if (!albumName.trimmed().isEmpty()) {
        query.addQueryItem(QStringLiteral("album_name"), albumName.trimmed());
    }
    if (durationSeconds > 0 && durationSeconds <= 3600) {
        query.addQueryItem(QStringLiteral("duration"), QString::number(durationSeconds));
    }
    url.setQuery(query);

    QNetworkRequest request(url);
    setRequestTimeout(request);
    request.setRawHeader(QByteArrayLiteral("User-Agent"),
                         QByteArrayLiteral("MusicPlayer/0.1.0 (https://github.com/HoszuAndreiAlexandru/MusicPlayer)"));
    QNetworkReply *reply = networkManager_->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, cacheKey, handler = std::move(handler)]() mutable {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray responseBody = reply->readAll();
        if (statusCode == 404) {
            CachedLyrics cached;
            cached.errorMessage = QStringLiteral("No lyrics were found for this track.");
            cached.cachedAtMs = QDateTime::currentMSecsSinceEpoch();
            cached.expiresAtMs = cached.cachedAtMs + spotifyMissingLyricsCacheLifetimeMs;
            lyricsCache_.insert(cacheKey, std::move(cached));
            saveLyricsCache();
            reply->deleteLater();
            if (handler) {
                handler({}, QStringLiteral("No lyrics were found for this track."));
            }
            return;
        }
        if (reply->error() != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
            const QString message = statusCode == 429
                ? QStringLiteral("The lyrics provider is rate limiting requests. Try again shortly.")
                : statusCode != 0
                    ? QStringLiteral("The lyrics provider returned HTTP %1.").arg(statusCode)
                    : reply->errorString();
            reply->deleteLater();
            if (handler) {
                handler({}, message);
            }
            return;
        }

        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(responseBody, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            reply->deleteLater();
            if (handler) {
                handler({}, QStringLiteral("The lyrics provider returned an invalid response."));
            }
            return;
        }

        const QJsonObject object = document.object();
        TrackLyrics lyrics;
        lyrics.plainLyrics = object.value(QStringLiteral("plainLyrics")).toString();
        lyrics.syncedLyrics = object.value(QStringLiteral("syncedLyrics")).toString();
        lyrics.instrumental = object.value(QStringLiteral("instrumental")).toBool();
        CachedLyrics cached;
        cached.lyrics = lyrics;
        cached.cachedAtMs = QDateTime::currentMSecsSinceEpoch();
        cached.expiresAtMs = cached.cachedAtMs + spotifyLyricsCacheLifetimeMs;
        lyricsCache_.insert(cacheKey, std::move(cached));
        saveLyricsCache();
        reply->deleteLater();
        if (handler) {
            handler(lyrics, {});
        }
    });
}

void SpotifyConnection::requestImage(const QString &imageUrl, ImageHandler handler)
{
    if (!handler) {
        return;
    }
    const QUrl url(imageUrl);
    const QString host = url.host().toLower();
    if (!url.isValid() || url.scheme() != QStringLiteral("https")
        || (!host.endsWith(QStringLiteral(".scdn.co"))
            && !host.endsWith(QStringLiteral(".spotifycdn.com")))) {
        QTimer::singleShot(0, this, [handler = std::move(handler)] { handler({}); });
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    auto cached = imageMemoryCache_.find(imageUrl);
    if (cached != imageMemoryCache_.end() && cached->expiresAtMs > nowMs) {
        const QImage image = cached->image;
        QTimer::singleShot(0, this, [handler = std::move(handler), image] { handler(image); });
        return;
    }
    if (cached != imageMemoryCache_.end()) {
        imageMemoryCache_.erase(cached);
    }
    auto pending = pendingImageHandlers_.find(imageUrl);
    if (pending != pendingImageHandlers_.end()) {
        pending->append(std::move(handler));
        return;
    }

    pendingImageHandlers_.insert(imageUrl, {std::move(handler)});
    const quint64 generation = imageCacheGeneration_;
    QPointer<SpotifyConnection> guard(this);
    QThreadPool::globalInstance()->start(QRunnable::create(
        [guard, imageUrl, generation] {
            QImage diskImage;
            qint64 expiresAtMs = 0;
            const bool loaded = guard
                && guard->loadImageFromDisk(imageUrl, &diskImage, &expiresAtMs);
            if (!guard) {
                return;
            }
            QMetaObject::invokeMethod(guard.data(),
                [guard, imageUrl, generation, loaded, diskImage, expiresAtMs] {
                    if (!guard || generation != guard->imageCacheGeneration_) {
                        return;
                    }
                    if (!loaded) {
                        guard->fetchImageFromNetwork(imageUrl, generation);
                        return;
                    }
                    if (guard->imageMemoryCache_.size() >= 64) {
                        guard->imageMemoryCache_.erase(guard->imageMemoryCache_.begin());
                    }
                    guard->imageMemoryCache_.insert(
                        imageUrl, CachedImage{diskImage, expiresAtMs});
                    const QList<ImageHandler> handlers =
                        guard->pendingImageHandlers_.take(imageUrl);
                    for (const ImageHandler &pendingHandler : handlers) {
                        if (pendingHandler) {
                            pendingHandler(diskImage);
                        }
                    }
                }, Qt::QueuedConnection);
        }));
}

void SpotifyConnection::fetchImageFromNetwork(const QString &imageUrl, quint64 generation)
{
    const QUrl url(imageUrl);
    QNetworkRequest request(url);
    setRequestTimeout(request, spotifyImageRequestTimeoutMs);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    QNetworkReply *reply = networkManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, imageUrl, generation] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        QImage image;
        if (generation == imageCacheGeneration_
            && reply->error() == QNetworkReply::NoError
            && statusCode >= 200 && statusCode < 300 && image.loadFromData(data)) {
            saveImageToDisk(imageUrl, data);
            if (imageMemoryCache_.size() >= 64) {
                imageMemoryCache_.erase(imageMemoryCache_.begin());
            }
            imageMemoryCache_.insert(
                imageUrl, CachedImage{image,
                    QDateTime::currentMSecsSinceEpoch() + spotifyImageCacheLifetimeMs});
        }
        reply->deleteLater();

        const QList<ImageHandler> handlers = pendingImageHandlers_.take(imageUrl);
        for (const ImageHandler &pendingHandler : handlers) {
            if (pendingHandler) {
                pendingHandler(image);
            }
        }
    });
}

void SpotifyConnection::resetImageMemoryCache()
{
    ++imageCacheGeneration_;
    imageMemoryCache_.clear();
    const auto pending = std::move(pendingImageHandlers_);
    pendingImageHandlers_.clear();
    for (const QList<ImageHandler> &handlers : pending) {
        for (const ImageHandler &handler : handlers) {
            if (handler) {
                handler({});
            }
        }
    }
}

QString SpotifyConnection::currentUserDisplayName() const
{
    return currentUserDisplayName_;
}

QString SpotifyConnection::currentUserImageUrl() const
{
    return currentUserImageUrl_;
}

void SpotifyConnection::fetchTrackPage(const QUrl &url, int requestedOffset,
                                      TracksPageHandler handler, bool forceNetwork,
                                      qint64 cacheLifetimeMs)
{
    const QString cacheKey = url.toString(QUrl::FullyEncoded);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    CachedTrackPage stalePage;
    bool hasStalePage = false;
    const auto cachedPage = trackPageCache_.constFind(cacheKey);
    if (cachedPage != trackPageCache_.cend()) {
        stalePage = *cachedPage;
        hasStalePage = true;
    }

    for (auto it = trackPageCache_.begin(); it != trackPageCache_.end();) {
        if (it.key() != cacheKey && it->expiresAtMs <= nowMs) {
            it = trackPageCache_.erase(it);
        } else {
            ++it;
        }
    }
    if (!hasStalePage) {
        CachedTrackPage diskPage;
        if (loadTrackPageFromDisk(cacheKey, &diskPage)) {
            stalePage = std::move(diskPage);
            hasStalePage = true;
        }
    }

    const bool stalePageIsFresh = hasStalePage && stalePage.expiresAtMs > nowMs;
    const bool stalePageIsInRetryDelay = hasStalePage && stalePage.retryAfterMs > nowMs;
    if (!forceNetwork && hasStalePage && (stalePageIsFresh || stalePageIsInRetryDelay)) {
        trackPageCache_.insert(cacheKey, stalePage);
        QTimer::singleShot(0, this, [handler = std::move(handler), stalePage]() mutable {
            handler(stalePage.tracks, stalePage.nextOffset, stalePage.total, {});
        });
        return;
    }
    if (hasStalePage) {
        trackPageCache_.insert(cacheKey, stalePage);
    }

    if (apiRateLimitActive()) {
        if (hasStalePage) {
            handler(stalePage.tracks, stalePage.nextOffset, stalePage.total,
                    apiRateLimitMessage());
        } else {
            handler({}, requestedOffset, 0, apiRateLimitMessage());
        }
        return;
    }

    QNetworkReply *reply = authenticatedGet(url, true, forceNetwork);
    const qint64 effectiveCacheLifetimeMs = cacheLifetimeMs > 0
        ? cacheLifetimeMs : spotifyTrackPageCacheLifetimeMs;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, requestedOffset, cacheKey, hasStalePage, stalePage,
             effectiveCacheLifetimeMs,
             handler = std::move(handler)]() mutable {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
            const QString message = apiFailureMessage(reply, response);
            reply->deleteLater();
            if (hasStalePage) {
                stalePage.retryAfterMs = QDateTime::currentMSecsSinceEpoch()
                    + spotifyTrackPageRetryDelayMs;
                trackPageCache_.insert(cacheKey, stalePage);
                saveTrackPageToDisk(cacheKey, stalePage);
                handler(stalePage.tracks, stalePage.nextOffset, stalePage.total,
                        QStringLiteral("Spotify refresh failed; showing saved tracks. %1")
                            .arg(message));
                return;
            }
            handler({}, requestedOffset, 0, message);
            return;
        }

        const QJsonArray items = response.value(QStringLiteral("items")).toArray();
        QList<SpotifyTrackResult> tracks;
        tracks.reserve(items.size());
        for (const QJsonValue &itemValue : items) {
            const QJsonObject savedItem = itemValue.toObject();
            QJsonObject trackObject = savedItem.value(QStringLiteral("item")).toObject();
            if (trackObject.isEmpty()) {
                trackObject = savedItem.value(QStringLiteral("track")).toObject();
            }
            if (trackObject.isEmpty() && savedItem.contains(QStringLiteral("name"))) {
                trackObject = savedItem;
            }

            SpotifyTrackResult track = parseTrack(trackObject);
            track.addedAt = savedItem.value(QStringLiteral("added_at")).toString();
            tracks.append(std::move(track));
        }

        const int currentOffset = response.value(QStringLiteral("offset")).toInt(requestedOffset);
        const int total = response.value(QStringLiteral("total")).toInt();
        const int nextOffset = currentOffset + items.size();
        CachedTrackPage cachedPage;
        cachedPage.tracks = tracks;
        cachedPage.nextOffset = nextOffset;
        cachedPage.total = total;
        cachedPage.expiresAtMs = QDateTime::currentMSecsSinceEpoch()
            + effectiveCacheLifetimeMs;
        cachedPage.retryAfterMs = 0;
        saveTrackPageToDisk(cacheKey, cachedPage);
        trackPageCache_.insert(cacheKey, cachedPage);
        reply->deleteLater();
        handler(tracks, nextOffset, total, {});
    });
}

void SpotifyConnection::clearApiCache()
{
    trackPageCache_.clear();
    if (httpCache_) {
        httpCache_->clear();
    }
    const QString cacheBase = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!cacheBase.isEmpty()) {
        QFile::remove(libraryItemsCachePath());
        QFile::remove(QDir(cacheBase).filePath(
            QStringLiteral("spotify-saved-tracks-check.json")));
        QDir trackPagesDirectory(QDir(cacheBase).filePath(QStringLiteral("spotify-track-pages")));
        if (trackPagesDirectory.exists()) {
            trackPagesDirectory.removeRecursively();
        }
    }
}

void SpotifyConnection::startPlaybackCompanion()
{
    if (accessToken_.isEmpty()) {
        if (playbackStatusHandler_) {
            playbackStatusHandler_(QStringLiteral("Waiting for a Spotify access token before starting playback."));
        }
        return;
    }

    if (!playbackServer_->isListening()) {
        if (!playbackServer_->listen(QHostAddress::LocalHost, 0)) {
            if (playbackStatusHandler_) {
                playbackStatusHandler_(QStringLiteral("Could not start the local playback bridge: %1")
                                           .arg(playbackServer_->errorString()));
            }
            return;
        }
        playbackKey_ = randomUrlSafeString(32);
    }

    if (playbackPageOpened_) {
        return;
    }

    if (playbackBrowserProcess_->state() != QProcess::NotRunning) {
        playbackPageOpened_ = true;
        return;
    }

    const QString browserExecutable = findHeadlessChromium();
    if (browserExecutable.isEmpty()) {
        if (playbackStatusHandler_) {
            playbackStatusHandler_(QStringLiteral(
                "Install Google Chrome, Microsoft Edge, or Chromium to use Spotify playback."));
        }
        return;
    }

    QUrl playbackUrl;
    playbackUrl.setScheme(QStringLiteral("http"));
    playbackUrl.setHost(QStringLiteral("127.0.0.1"));
    playbackUrl.setPort(playbackServer_->serverPort());
    playbackUrl.setPath(QStringLiteral("/"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("key"), playbackKey_);
    playbackUrl.setQuery(query);

    QString cacheBasePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheBasePath.isEmpty()) {
        cacheBasePath = QDir::tempPath();
    }
    playbackBrowserProfilePath_ = QDir(cacheBasePath).filePath(
        QStringLiteral("spotify-playback-chromium"));
    if (!QDir().mkpath(playbackBrowserProfilePath_)) {
        playbackBrowserProfilePath_.clear();
        if (playbackStatusHandler_) {
            playbackStatusHandler_(QStringLiteral("Could not create the isolated headless browser profile."));
        }
        return;
    }

    const QStringList arguments{
        QStringLiteral("--headless=new"),
        QStringLiteral("--autoplay-policy=no-user-gesture-required"),
        QStringLiteral("--disable-gpu"),
        QStringLiteral("--disable-extensions"),
        QStringLiteral("--disable-features=AudioFocusEnforcement,MediaSessionService"),
        QStringLiteral("--blink-settings=imagesEnabled=false"),
        QStringLiteral("--renderer-process-limit=2"),
        QStringLiteral("--disk-cache-size=67108864"),
        QStringLiteral("--media-cache-size=67108864"),
        QStringLiteral("--no-first-run"),
        QStringLiteral("--no-default-browser-check"),
        QStringLiteral("--user-data-dir=%1").arg(playbackBrowserProfilePath_),
        playbackUrl.toString(QUrl::FullyEncoded)
    };
    playbackBrowserProcess_->start(browserExecutable, arguments);
    playbackPageOpened_ = true;
    playbackStartupTimer_->start(playbackStartupTimeoutMs);
    if (playbackStatusHandler_) {
        playbackStatusHandler_(QStringLiteral("Starting Spotify's headless Chromium playback process…"));
    }
}

void SpotifyConnection::maybeStartPendingPlayback()
{
    if (!connected_ || accessToken_.isEmpty() || playbackDeviceId_.isEmpty()
        || !playbackState_.audioActivated
        || (pendingPlaybackContextUri_.isEmpty() && pendingPlaybackUris_.isEmpty())) {
        return;
    }

    QJsonObject body;
    if (!pendingPlaybackContextUri_.isEmpty()) {
        body.insert(QStringLiteral("context_uri"), pendingPlaybackContextUri_);
        if (!pendingPlaybackOffsetUri_.isEmpty()) {
            body.insert(QStringLiteral("offset"),
                        QJsonObject{{QStringLiteral("uri"), pendingPlaybackOffsetUri_}});
        }
    } else {
        QJsonArray uris;
        for (const QString &uri : pendingPlaybackUris_) {
            uris.append(uri);
        }
        body.insert(QStringLiteral("uris"), uris);
    }
    if (pendingPlaybackPositionMs_ >= 0) {
        body.insert(QStringLiteral("position_ms"), pendingPlaybackPositionMs_);
    }
    pendingPlaybackUris_.clear();
    pendingPlaybackContextUri_.clear();
    pendingPlaybackOffsetUri_.clear();
    pendingPlaybackPositionMs_ = -1;
    QUrl url(QStringLiteral("https://api.spotify.com/v1/me/player/play"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("device_id"), playbackDeviceId_);
    url.setQuery(query);
    sendPlayerCommand(QByteArrayLiteral("PUT"), url,
                      QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void SpotifyConnection::queueLocalPlaybackCommand(const QString &name, int value)
{
    if (!connected_ || playbackDeviceId_.isEmpty() || !playbackState_.deviceReady
        || !playbackState_.audioActivated) {
        if (playbackStatusHandler_) {
            playbackStatusHandler_(QStringLiteral("The local Spotify playback device is not ready yet."));
        }
        return;
    }

    QJsonObject commandObject{{QStringLiteral("name"), name}};
    if (name == QStringLiteral("seek")) {
        commandObject.insert(QStringLiteral("position_ms"), value);
    } else if (name == QStringLiteral("volume")) {
        commandObject.insert(QStringLiteral("volume"), qBound(0, value, 100) / 100.0);
    }

    if (name == QStringLiteral("seek") || name == QStringLiteral("volume")) {
        for (qsizetype index = pendingPlaybackCommands_.size() - 1; index >= 0; --index) {
            if (pendingPlaybackCommands_.at(index).value(QStringLiteral("name")).toString() == name) {
                pendingPlaybackCommands_.removeAt(index);
                break;
            }
        }
    }
    if (pendingPlaybackCommands_.size() >= 64) {
        pendingPlaybackCommands_.removeFirst();
    }
    pendingPlaybackCommands_.append(commandObject);
}

void SpotifyConnection::sendPlayerCommand(const QByteArray &method, const QUrl &url,
                                          const QByteArray &body)
{
    if (!connected_ || accessToken_.isEmpty()) {
        if (playbackStatusHandler_) {
            playbackStatusHandler_(QStringLiteral("Connect Spotify before controlling playback."));
        }
        return;
    }
    if (apiRateLimitActive()) {
        if (playbackStatusHandler_) {
            playbackStatusHandler_(apiRateLimitMessage());
        }
        return;
    }

    QNetworkRequest request(url);
    setRequestTimeout(request);
    request.setRawHeader("Authorization", "Bearer " + accessToken_);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = method == QByteArrayLiteral("POST")
        ? networkManager_->post(request, body)
        : networkManager_->put(request, body);
    observeApiReply(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() == QNetworkReply::NoError && statusCode >= 200 && statusCode < 300) {
            reply->deleteLater();
            return;
        }

        QString message;
        if (statusCode == 403) {
            message = QStringLiteral("Spotify denied playback. Reconnect and grant playback access; the Web Playback SDK also requires Premium.");
        } else if (statusCode == 404) {
            message = QStringLiteral("Spotify could not find the headless playback device. If it stays offline, disconnect and reconnect Spotify.");
        } else if (statusCode == 429) {
            message = apiFailureMessage(reply, response);
        } else {
            const QJsonObject apiError = response.value(QStringLiteral("error")).toObject();
            message = apiError.value(QStringLiteral("message")).toString();
            if (message.isEmpty()) {
                message = reply->errorString();
            }
            if (statusCode != 0) {
                message = QStringLiteral("Spotify playback request failed (HTTP %1): %2")
                              .arg(statusCode).arg(message);
            }
        }

        reply->deleteLater();
        if (playbackStatusHandler_) {
            playbackStatusHandler_(message);
        }
    });
}

void SpotifyConnection::notifyPlaybackState()
{
    if (playbackStateHandler_) {
        playbackStateHandler_(playbackState_);
    }
}

void SpotifyConnection::handlePlaybackConnection()
{
    while (playbackServer_->hasPendingConnections()) {
        QTcpSocket *socket = playbackServer_->nextPendingConnection();
        if (!socket) {
            continue;
        }

        const auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
            if (socket->property("playbackRequestHandled").toBool()) {
                return;
            }

            buffer->append(socket->readAll());
            if (buffer->size() > 64 * 1024) {
                socket->setProperty("playbackRequestHandled", true);
                respondToPlaybackBrowser(socket, 413, QByteArrayLiteral("Payload Too Large"),
                                         QByteArrayLiteral("text/plain; charset=utf-8"), {});
                return;
            }

            const qsizetype headerEnd = buffer->indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                return;
            }

            int contentLength = 0;
            const QList<QByteArray> lines = buffer->left(headerEnd).split('\n');
            for (qsizetype index = 1; index < lines.size(); ++index) {
                const QByteArray line = lines.at(index).trimmed();
                const qsizetype separator = line.indexOf(':');
                if (separator > 0 && line.left(separator).trimmed().toLower() == "content-length") {
                    contentLength = line.mid(separator + 1).trimmed().toInt();
                    break;
                }
            }
            if (contentLength < 0 || contentLength > 32 * 1024) {
                socket->setProperty("playbackRequestHandled", true);
                respondToPlaybackBrowser(socket, 413, QByteArrayLiteral("Payload Too Large"),
                                         QByteArrayLiteral("text/plain; charset=utf-8"), {});
                return;
            }
            if (buffer->size() < headerEnd + 4 + contentLength) {
                return;
            }

            socket->setProperty("playbackRequestHandled", true);
            handlePlaybackRequest(socket, *buffer);
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void SpotifyConnection::handlePlaybackRequest(QTcpSocket *socket, const QByteArray &requestData)
{
    const qsizetype headerEnd = requestData.indexOf("\r\n\r\n");
    const QList<QByteArray> lines = requestData.left(headerEnd).split('\n');
    if (lines.isEmpty()) {
        respondToPlaybackBrowser(socket, 400, QByteArrayLiteral("Bad Request"),
                                 QByteArrayLiteral("text/plain; charset=utf-8"), {});
        return;
    }

    const QList<QByteArray> requestParts = lines.first().trimmed().split(' ');
    if (requestParts.size() < 2) {
        respondToPlaybackBrowser(socket, 400, QByteArrayLiteral("Bad Request"),
                                 QByteArrayLiteral("text/plain; charset=utf-8"), {});
        return;
    }

    const QByteArray method = requestParts.at(0).toUpper();
    const QUrl requestUrl = QUrl::fromEncoded(requestParts.at(1));
    const QString path = requestUrl.path();
    const QByteArray body = requestData.mid(headerEnd + 4);
    QByteArray suppliedKey;
    for (qsizetype index = 1; index < lines.size(); ++index) {
        const QByteArray line = lines.at(index).trimmed();
        const qsizetype separator = line.indexOf(':');
        if (separator > 0 && line.left(separator).trimmed().toLower() == "x-musicplayer-key") {
            suppliedKey = line.mid(separator + 1).trimmed();
            break;
        }
    }

    const QUrlQuery requestQuery(requestUrl);
    if (method == "GET" && path == QStringLiteral("/")) {
        if (requestQuery.queryItemValue(QStringLiteral("key")).toLatin1() != playbackKey_) {
            respondToPlaybackBrowser(socket, 403, QByteArrayLiteral("Forbidden"),
                                     QByteArrayLiteral("text/plain; charset=utf-8"), {});
            return;
        }
        respondToPlaybackBrowser(socket, 200, QByteArrayLiteral("OK"),
                                 QByteArrayLiteral("text/html; charset=utf-8"),
                                 playbackPageHtml(preferredPlaybackVolumePercent_));
        return;
    }

    if (playbackKey_.isEmpty() || suppliedKey != playbackKey_.toLatin1()) {
        respondToPlaybackBrowser(socket, 403, QByteArrayLiteral("Forbidden"),
                                 QByteArrayLiteral("text/plain; charset=utf-8"), {});
        return;
    }

    if (method == "GET" && path == QStringLiteral("/token")) {
        if (accessToken_.isEmpty()) {
            respondToPlaybackBrowser(socket, 401, QByteArrayLiteral("Unauthorized"),
                                     QByteArrayLiteral("application/json"),
                                     QByteArrayLiteral("{}"));
            return;
        }
        const QJsonObject token{{QStringLiteral("access_token"), QString::fromUtf8(accessToken_)}};
        respondToPlaybackBrowser(socket, 200, QByteArrayLiteral("OK"),
                                 QByteArrayLiteral("application/json"),
                                 QJsonDocument(token).toJson(QJsonDocument::Compact));
        return;
    }

    if (method == "GET" && path == QStringLiteral("/health")) {
        respondToPlaybackBrowser(socket, 200, QByteArrayLiteral("OK"),
                                 QByteArrayLiteral("application/json"), QByteArrayLiteral("{}"));
        return;
    }

    if (method == "GET" && path == QStringLiteral("/commands")) {
        QJsonArray commands;
        for (const QJsonObject &command : pendingPlaybackCommands_) {
            commands.append(command);
        }
        pendingPlaybackCommands_.clear();
        respondToPlaybackBrowser(socket, 200, QByteArrayLiteral("OK"),
                                 QByteArrayLiteral("application/json"),
                                 QJsonDocument(commands).toJson(QJsonDocument::Compact));
        return;
    }

    if (method == "POST" && path == QStringLiteral("/command-error")) {
        const QJsonObject payload = QJsonDocument::fromJson(body).object();
        const QString command = payload.value(QStringLiteral("name")).toString();
        const QString detail = payload.value(QStringLiteral("message")).toString();
        if (playbackStatusHandler_) {
            playbackStatusHandler_(QStringLiteral("Local playback command '%1' failed: %2")
                                       .arg(command, detail));
        }
        respondToPlaybackBrowser(socket, 204, QByteArrayLiteral("No Content"),
                                 QByteArrayLiteral("application/json"), {});
        return;
    }

    if (method == "POST" && path == QStringLiteral("/ready")) {
        const QJsonObject payload = QJsonDocument::fromJson(body).object();
        const QString deviceId = payload.value(QStringLiteral("device_id")).toString();
        if (deviceId.isEmpty()) {
            respondToPlaybackBrowser(socket, 400, QByteArrayLiteral("Bad Request"),
                                     QByteArrayLiteral("application/json"), QByteArrayLiteral("{}"));
            return;
        }
        playbackDeviceId_ = deviceId;
        localPlaybackState_.deviceId = deviceId;
        localPlaybackState_.deviceName = QStringLiteral("MusicPlayer");
        localPlaybackState_.deviceType = QStringLiteral("Computer");
        localPlaybackState_.deviceReady = true;
        playbackPageOpened_ = true;
        updateEffectivePlaybackState();
        if (playbackStatusHandler_) {
            playbackStatusHandler_(QStringLiteral("Headless Spotify playback device is ready."));
        }
        maybeStartPendingPlayback();
        respondToPlaybackBrowser(socket, 204, QByteArrayLiteral("No Content"),
                                 QByteArrayLiteral("application/json"), {});
        return;
    }

    if (method == "POST" && path == QStringLiteral("/activated")) {
        playbackStartupTimer_->stop();
        localPlaybackState_.audioActivated = true;
        updateEffectivePlaybackState();
        if (playbackStatusHandler_) {
            playbackStatusHandler_(QStringLiteral("Spotify audio is enabled in the headless playback browser."));
        }
        maybeStartPendingPlayback();
        respondToPlaybackBrowser(socket, 204, QByteArrayLiteral("No Content"),
                                 QByteArrayLiteral("application/json"), {});
        return;
    }

    if (method == "POST" && path == QStringLiteral("/state")) {
        const QJsonObject payload = QJsonDocument::fromJson(body).object();
        localPlaybackState_.deviceId = playbackDeviceId_;
        localPlaybackState_.deviceName = QStringLiteral("MusicPlayer");
        localPlaybackState_.deviceType = QStringLiteral("Computer");
        localPlaybackState_.active = payload.value(QStringLiteral("active")).toBool();
        localPlaybackState_.paused = payload.value(QStringLiteral("paused")).toBool(true);
        localPlaybackState_.positionMs = qMax(0, payload.value(QStringLiteral("position_ms")).toInt());
        localPlaybackState_.durationMs = qMax(0, payload.value(QStringLiteral("duration_ms")).toInt());
        localPlaybackState_.trackName.clear();
        localPlaybackState_.trackUri.clear();
        localPlaybackState_.artists.clear();
        localPlaybackState_.artistId.clear();
        localPlaybackState_.album.clear();
        localPlaybackState_.albumId.clear();
        localPlaybackState_.artworkUrl.clear();
        if (localPlaybackState_.active) {
            const QJsonObject track = payload.value(QStringLiteral("track")).toObject();
            localPlaybackState_.trackName = track.value(QStringLiteral("name")).toString();
            localPlaybackState_.trackUri = track.value(QStringLiteral("uri")).toString();
            localPlaybackState_.album = track.value(QStringLiteral("album")).toString();
            localPlaybackState_.artistId = track.value(QStringLiteral("artist_id")).toString();
            localPlaybackState_.albumId = track.value(QStringLiteral("album_id")).toString();
            localPlaybackState_.artworkUrl = track.value(QStringLiteral("artwork_url")).toString();
            QStringList artists;
            for (const QJsonValue &artist : track.value(QStringLiteral("artists")).toArray()) {
                const QString name = artist.toString();
                if (!name.isEmpty()) {
                    artists.append(name);
                }
            }
            localPlaybackState_.artists = artists.join(QStringLiteral(", "));
            apiPlaybackState_.deviceId = playbackDeviceId_;
            apiPlaybackState_.deviceName = QStringLiteral("MusicPlayer");
            apiPlaybackState_.deviceType = QStringLiteral("Computer");
            apiPlaybackState_.externalDevice = false;
            apiPlaybackStateKnown_ = true;
        }
        updateEffectivePlaybackState();
        respondToPlaybackBrowser(socket, 204, QByteArrayLiteral("No Content"),
                                 QByteArrayLiteral("application/json"), {});
        return;
    }

    if (method == "POST" && path == QStringLiteral("/error")) {
        playbackStartupTimer_->stop();
        const QJsonObject payload = QJsonDocument::fromJson(body).object();
        const QString type = payload.value(QStringLiteral("type")).toString();
        const QString detail = payload.value(QStringLiteral("message")).toString();
        QString message = detail;
        if (type == QStringLiteral("account_error")) {
            message = QStringLiteral("Spotify Web Playback requires a Premium account.");
        } else if (type == QStringLiteral("authentication_error")) {
            message = QStringLiteral("Spotify playback authorization failed. Reconnect and grant updated access.");
        } else if (type == QStringLiteral("initialization_error")) {
            message = QStringLiteral("The headless Chromium playback browser could not initialize Spotify's protected audio (EME/Widevine). Ensure Chrome, Edge, or Chromium is up to date. %1")
                          .arg(detail);
        } else if (type == QStringLiteral("playback_error")) {
            message = QStringLiteral("Spotify could not play this track: %1").arg(detail);
        }
        if (playbackStatusHandler_) {
            playbackStatusHandler_(message);
        }
        respondToPlaybackBrowser(socket, 204, QByteArrayLiteral("No Content"),
                                 QByteArrayLiteral("application/json"), {});
        return;
    }

    if (method == "POST" && path == QStringLiteral("/stopped")) {
        pendingPlaybackCommands_.clear();
        const QString stoppedDeviceId = playbackDeviceId_;
        playbackDeviceId_.clear();
        localPlaybackState_ = {};
        if (!stoppedDeviceId.isEmpty() && apiPlaybackState_.deviceId == stoppedDeviceId) {
            apiPlaybackState_ = {};
            apiPlaybackStateKnown_ = false;
        }
        playbackPageOpened_ = false;
        updateEffectivePlaybackState();
        if (playbackStatusHandler_) {
            playbackStatusHandler_(QStringLiteral("Headless playback process stopped."));
        }
        respondToPlaybackBrowser(socket, 204, QByteArrayLiteral("No Content"),
                                 QByteArrayLiteral("application/json"), {});
        return;
    }

    respondToPlaybackBrowser(socket, 404, QByteArrayLiteral("Not Found"),
                             QByteArrayLiteral("text/plain; charset=utf-8"), {});
}

void SpotifyConnection::handleIncomingConnection()
{
    while (callbackServer_->hasPendingConnections()) {
        QTcpSocket *socket = callbackServer_->nextPendingConnection();
        if (!socket) {
            continue;
        }

        const auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
            if (socket->property("spotifyCallbackHandled").toBool()) {
                return;
            }

            buffer->append(socket->readAll());
            const qsizetype headerEnd = buffer->indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                return;
            }
            socket->setProperty("spotifyCallbackHandled", true);
            handleCallback(socket, buffer->left(buffer->indexOf("\r\n")));
        });
    }
}

void SpotifyConnection::handleCallback(QTcpSocket *socket, const QByteArray &requestLine)
{
    const QList<QByteArray> requestParts = requestLine.split(' ');
    if (requestParts.size() < 2 || requestParts.at(0) != "GET") {
        respondToBrowser(socket, 400, "Bad Request",
                         "<html><body>Invalid Spotify callback. Return to MusicPlayer.</body></html>");
        fail("Spotify returned an invalid sign-in callback.");
        return;
    }

    const QUrl callbackUrl(QStringLiteral("http://127.0.0.1") + QString::fromUtf8(requestParts.at(1)));
    if (callbackUrl.path() != QStringLiteral("/")) {
        respondToBrowser(socket, 404, "Not Found",
                         "<html><body>Return to MusicPlayer to finish Spotify sign-in.</body></html>");
        return;
    }

    const QUrlQuery query(callbackUrl);
    const QString returnedState = query.queryItemValue(QStringLiteral("state"));
    if (returnedState.isEmpty() || returnedState != state_) {
        respondToBrowser(socket, 400, "Bad Request",
                         "<html><body>Spotify sign-in could not be verified. Return to MusicPlayer.</body></html>");
        fail("Spotify sign-in could not be verified. Please try again.");
        return;
    }

    const QString authorizationError = query.queryItemValue(QStringLiteral("error"));
    if (!authorizationError.isEmpty()) {
        respondToBrowser(socket, 200, "OK",
                         "<html><body>Spotify sign-in was cancelled. You can close this tab.</body></html>");
        fail(authorizationError == QStringLiteral("access_denied")
                 ? QStringLiteral("Spotify access was declined.")
                 : QStringLiteral("Spotify sign-in failed: %1").arg(authorizationError));
        return;
    }

    const QString code = query.queryItemValue(QStringLiteral("code"));
    if (code.isEmpty()) {
        respondToBrowser(socket, 400, "Bad Request",
                         "<html><body>No authorization code was returned. Return to MusicPlayer.</body></html>");
        fail("Spotify did not return an authorization code.");
        return;
    }

    respondToBrowser(socket, 200, "OK",
                     "<html><body>Spotify connected. You can close this tab and return to MusicPlayer.</body></html>");
    loginTimeout_->stop();
    callbackServer_->close();
    exchangeAuthorizationCode(code);
}

void SpotifyConnection::exchangeAuthorizationCode(const QString &code)
{
    const QByteArray body = formBody({
        {QStringLiteral("client_id"), clientId_},
        {QStringLiteral("grant_type"), QStringLiteral("authorization_code")},
        {QStringLiteral("code"), code},
        {QStringLiteral("redirect_uri"), redirectUri_},
        {QStringLiteral("code_verifier"), codeVerifier_}
    });
    codeVerifier_.clear();
    state_.clear();

    QNetworkRequest request(QUrl(QStringLiteral("https://accounts.spotify.com/api/token")));
    setRequestTimeout(request);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    QNetworkReply *reply = networkManager_->post(request, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleTokenReply(reply, false);
    });
}

void SpotifyConnection::refreshAccessToken()
{
    if (refreshToken_.isEmpty()) {
        fail("Your Spotify session expired. Please connect again.");
        return;
    }

    setState("Refreshing Spotify connection…", connected_, true);
    const QByteArray body = formBody({
        {QStringLiteral("client_id"), clientId_},
        {QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
        {QStringLiteral("refresh_token"), QString::fromUtf8(refreshToken_)}
    });

    QNetworkRequest request(QUrl(QStringLiteral("https://accounts.spotify.com/api/token")));
    setRequestTimeout(request);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    QNetworkReply *reply = networkManager_->post(request, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleTokenReply(reply, true);
    });
}

void SpotifyConnection::requestCurrentUserProfile()
{
    if (accessToken_.isEmpty()) {
        return;
    }
    if (apiRateLimitActive()) {
        const int retrySeconds = qMin(apiRateLimitRemainingSeconds(), 24 * 60 * 60);
        profileRetryTimer_->start(retrySeconds * 1000);
        if (connected_) {
            setState(QStringLiteral("Connected to Spotify; profile lookup is rate limited. Retrying in about %1 seconds.")
                         .arg(retrySeconds), true, false);
        }
        return;
    }
    QNetworkReply *reply = authenticatedGet(
        QUrl(QStringLiteral("https://api.spotify.com/v1/me")), false, true);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject profile = document.object();
        if (reply->error() != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300
            || profile.isEmpty()) {
            const bool networkFailure = reply->error() != QNetworkReply::NoError;
            const QString message = replyError(reply, profile);
            if (statusCode == 429) {
                const int retrySeconds = qMax(1, apiRateLimitRemainingSeconds());
                reply->deleteLater();
                markConnected(QStringLiteral("%1 Retrying profile details in about %2 seconds.")
                                  .arg(apiRateLimitMessage()).arg(retrySeconds));
                profileRetryTimer_->start(qMin(retrySeconds, 24 * 60 * 60) * 1000);
                return;
            }
            reply->deleteLater();
            if (networkFailure || statusCode >= 500) {
                markConnected(QStringLiteral("Connected to Spotify; profile request failed: %1. Retrying shortly.")
                                  .arg(message));
                profileRetryTimer_->start(15000);
                return;
            }
            if (statusCode != 401) {
                markConnected(QStringLiteral("Connected to Spotify, but its account profile could not be loaded: %1")
                                  .arg(message));
                return;
            }
            fail("Could not load your Spotify profile: " + message);
            return;
        }

        QString displayName = profile.value("display_name").toString();
        if (displayName.isEmpty()) {
            displayName = profile.value("id").toString();
        }
        if (displayName.isEmpty()) {
            displayName = QStringLiteral("Spotify user");
        }
        currentUserDisplayName_ = displayName;
        currentUserImageUrl_ = bestImageUrl(profile.value(QStringLiteral("images")).toArray(), 256);

        profileRetryTimer_->stop();
        reply->deleteLater();
        markConnected(QStringLiteral("Connected as %1").arg(displayName));
    });
}

void SpotifyConnection::markConnected(const QString &message)
{
    connected_ = true;
    setState(message, true, false);
    playbackPollTimer_->start();
    startPlaybackCompanion();
    pollPlaybackState();
    refreshAvailableDevices();
}

void SpotifyConnection::handleTokenReply(QNetworkReply *reply, bool refreshing)
{
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
    const QJsonObject token = document.object();
    if (reply->error() != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300
        || token.value("access_token").toString().isEmpty()) {
        if (statusCode == 429 && refreshing) {
            bool retryAfterOk = false;
            const int serverRetrySeconds = reply->rawHeader(QByteArrayLiteral("Retry-After"))
                                               .toInt(&retryAfterOk);
            const int retrySeconds = retryAfterOk && serverRetrySeconds > 0
                ? qBound(1, serverRetrySeconds, 24 * 60 * 60) : 60;
            reply->deleteLater();
            refreshTimer_->start(retrySeconds * 1000);
            setState(QStringLiteral("Spotify's token service is rate limiting requests; retrying in about %1 seconds.")
                         .arg(retrySeconds), connected_, false);
            return;
        }
        const QString message = replyError(reply, token);
        reply->deleteLater();
        fail("Spotify token request failed: " + message);
        return;
    }

    accessToken_ = token.value("access_token").toString().toUtf8();
    const QString newRefreshToken = token.value("refresh_token").toString();
    if (!newRefreshToken.isEmpty()) {
        refreshToken_ = newRefreshToken.toUtf8();
    }

    const int expiresIn = token.value("expires_in").toInt();
    if (expiresIn <= 0) {
        reply->deleteLater();
        fail("Spotify returned an invalid token expiration time.");
        return;
    }
    refreshTimer_->start(qMax(1000, (expiresIn - 60) * 1000));
    reply->deleteLater();

    QString sessionError;
    if (!refreshToken_.isEmpty() && !saveSession(&sessionError)
        && !sessionError.isEmpty() && errorHandler_) {
        errorHandler_(sessionError);
    }

    if (!connected_) {
        startPlaybackCompanion();
    }

    if (refreshing && connected_) {
        setState("Connected to Spotify", true, false);
        startPlaybackCompanion();
        pollPlaybackState();
        refreshAvailableDevices();
    } else {
        requestCurrentUserProfile();
    }
}

void SpotifyConnection::fail(const QString &message)
{
    loginTimeout_->stop();
    refreshTimer_->stop();
    profileRetryTimer_->stop();
    callbackServer_->close();
    codeVerifier_.clear();
    state_.clear();
    redirectUri_.clear();
    accessToken_.clear();
    refreshToken_.clear();
    connected_ = false;
    playbackPollTimer_->stop();
    stopPlaybackCompanion();
    clearApiCache();
    resetImageMemoryCache();
    playbackServer_->close();
    playbackKey_.clear();
    playbackDeviceId_.clear();
    pendingPlaybackUris_.clear();
    pendingPlaybackContextUri_.clear();
    pendingPlaybackOffsetUri_.clear();
    pendingPlaybackPositionMs_ = -1;
    playbackPageOpened_ = false;
    playbackState_ = {};
    localPlaybackState_ = {};
    apiPlaybackState_ = {};
    apiPlaybackStateKnown_ = false;
    currentUserDisplayName_.clear();
    currentUserImageUrl_.clear();
    availableDevices_.clear();
    availableDevicesError_.clear();
    playbackScopeWarningShown_ = false;
    notifyDevicesUpdated();
    notifyPlaybackState();
    if (playbackStatusHandler_) {
        playbackStatusHandler_(QStringLiteral("Connect Spotify to enable playback."));
    }
    setState("Spotify not connected", false, false);
    if (errorHandler_) {
        errorHandler_(message);
    }
}

void SpotifyConnection::setState(const QString &message, bool connected, bool busy)
{
    if (stateHandler_) {
        stateHandler_(message, connected, busy);
    }
}
