#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <functional>

class QNetworkAccessManager;
class QNetworkDiskCache;
class QNetworkReply;
class QProcess;
class QUrl;
class QTcpSocket;
class QTcpServer;
class QTimer;

struct SpotifyTrackResult
{
    QString name;
    QString artists;
    QString artistId;
    QString album;
    QString albumId;
    QString albumImageUrl;
    QString addedAt;
    QString spotifyUri;
    QString spotifyUrl;
    int durationMs = 0;
};

struct SpotifyArtistProfile
{
    QString id;
    QString name;
    QString imageUrl;
    QString spotifyUrl;
    QStringList genres;
    int followers = -1;
};

struct SpotifyAlbumProfile
{
    QString id;
    QString name;
    QString artists;
    QString artistId;
    QString imageUrl;
    QString spotifyUrl;
    QString releaseDate;
    QString albumType;
    int totalTracks = 0;
};

struct SpotifyPlaybackState
{
    QString trackName;
    QString trackUri;
    QString artists;
    QString artistId;
    QString album;
    QString albumId;
    QString artworkUrl;
    QString deviceId;
    QString deviceName;
    QString deviceType;
    QString repeatState = QStringLiteral("off");
    int durationMs = 0;
    int positionMs = 0;
    int deviceVolumePercent = -1;
    bool paused = true;
    bool active = false;
    bool deviceReady = false;
    bool audioActivated = false;
    bool externalDevice = false;
    bool deviceRestricted = false;
    bool supportsVolume = false;
};

struct SpotifyDeviceResult
{
    QString id;
    QString name;
    QString type;
    int volumePercent = -1;
    bool active = false;
    bool restricted = false;
    bool supportsVolume = false;
};

struct SpotifyPlaylistResult
{
    QString id;
    QString name;
    QString imageUrl;
    QString spotifyUrl;
    int itemCount = 0;
};

enum class SpotifyLibraryItemType
{
    LikedSongs,
    Playlist,
    Album,
    Episode,
    Show
};

struct SpotifyLibraryItem
{
    SpotifyLibraryItemType type = SpotifyLibraryItemType::Playlist;
    QString id;
    QString name;
    QString creator;
    QString category;
    QString imageUrl;
    QString spotifyUri;
    QString spotifyUrl;
    int itemCount = 0;
    int durationMs = 0;
};

class SpotifyConnection final : public QObject
{
public:
    using StateHandler = std::function<void(const QString &, bool, bool)>;
    using ErrorHandler = std::function<void(const QString &)>;
    using SearchHandler = std::function<void(const QList<SpotifyTrackResult> &,
                                             const QList<SpotifyArtistProfile> &,
                                             const QString &)>;
    using PlaylistsHandler = std::function<void(const QList<SpotifyPlaylistResult> &, const QString &)>;
    using LibraryItemsHandler = std::function<void(const QList<SpotifyLibraryItem> &,
                                                   const QString &)>;
    using TracksPageHandler = std::function<void(const QList<SpotifyTrackResult> &, int, int,
                                                 const QString &)>;
    using PlaybackStateHandler = std::function<void(const SpotifyPlaybackState &)>;
    using PlaybackStatusHandler = std::function<void(const QString &)>;
    using DevicesUpdatedHandler = std::function<void(const QList<SpotifyDeviceResult> &)>;
    using ImageHandler = std::function<void(const QImage &)>;
    using ArtistProfileHandler = std::function<void(const SpotifyArtistProfile &, const QString &)>;
    using ArtistAlbumsHandler = std::function<void(const QList<SpotifyAlbumProfile> &, int, int,
                                                   bool, const QString &)>;
    using AlbumProfileHandler = std::function<void(const SpotifyAlbumProfile &, const QString &)>;

    explicit SpotifyConnection(QObject *parent = nullptr);
    ~SpotifyConnection() override;

    void setStateHandler(StateHandler handler);
    void setErrorHandler(ErrorHandler handler);
    void setPlaybackStateHandler(PlaybackStateHandler handler);
    void setPlaybackStatusHandler(PlaybackStatusHandler handler);
    void setDevicesUpdatedHandler(DevicesUpdatedHandler handler);
    void beginLogin(const QString &clientId, quint16 callbackPort);
    QString restoreSession();
    void disconnectAccount();
    bool isConnected() const;
    void invalidateLibraryCache();
    void playTracks(const QStringList &trackUris, const QString &contextUri = {},
                    const QString &offsetUri = {});
    void pausePlayback();
    void resumePlayback();
    void nextTrack();
    void previousTrack();
    void setRepeatMode(const QString &mode);
    void seekPlayback(int positionMs);
    void setPlaybackVolume(int volumePercent);
    void transferPlaybackToDevice(const QString &deviceId);
    void refreshAvailableDevices(std::function<void()> completion = {});
    QList<SpotifyDeviceResult> availableDevices() const;
    QString localPlaybackDeviceId() const;
    void searchCatalog(const QString &query, SearchHandler handler);
    void fetchCurrentUserPlaylists(PlaylistsHandler handler, bool forceNetwork = false);
    void fetchCurrentUserLibraryItems(LibraryItemsHandler handler, bool forceNetwork = false);
    void fetchSavedTracks(int offset, TracksPageHandler handler, bool forceNetwork = false);
    void fetchPlaylistTracks(const QString &playlistId, int offset, TracksPageHandler handler,
                             bool forceNetwork = false);
    void fetchArtistProfile(const QString &artistId, ArtistProfileHandler handler);
    void fetchArtistAlbums(const QString &artistId, int offset, ArtistAlbumsHandler handler);
    void fetchAlbumProfile(const QString &albumId, AlbumProfileHandler handler);
    void fetchAlbumTracks(const QString &albumId, int offset, TracksPageHandler handler);
    void requestImage(const QString &imageUrl, ImageHandler handler);
    void setImageCacheMaxSizeMb(int megabytes);
    QString availableDevicesError() const;
    QString currentUserDisplayName() const;
    QString currentUserImageUrl() const;

private:
    struct CachedTrackPage
    {
        QList<SpotifyTrackResult> tracks;
        int nextOffset = 0;
        int total = 0;
        qint64 expiresAtMs = 0;
        qint64 retryAfterMs = 0;
    };

    void handleIncomingConnection();
    void handleCallback(QTcpSocket *socket, const QByteArray &requestLine);
    void handlePlaybackConnection();
    void handlePlaybackRequest(QTcpSocket *socket, const QByteArray &requestData);
    void exchangeAuthorizationCode(const QString &code);
    void refreshAccessToken();
    void requestCurrentUserProfile();
    void markConnected(const QString &message);
    void handleTokenReply(QNetworkReply *reply, bool refreshing);
    void observeApiReply(QNetworkReply *reply);
    bool apiRateLimitActive() const;
    int apiRateLimitRemainingSeconds() const;
    QString apiRateLimitMessage() const;
    QNetworkReply *authenticatedGet(const QUrl &url, bool cacheable = false,
                                    bool forceNetwork = false);
    void fetchPlaylistPage(int offset, QList<SpotifyPlaylistResult> playlists,
                           PlaylistsHandler handler, bool forceNetwork);
    void fetchLibraryItemPage(const QString &endpoint, SpotifyLibraryItemType type,
                              int offset, QList<SpotifyLibraryItem> items,
                              LibraryItemsHandler handler, bool forceNetwork);
    void fetchTrackPage(const QUrl &url, int requestedOffset, TracksPageHandler handler,
                       bool forceNetwork, qint64 cacheLifetimeMs = -1);
    QString trackPageCachePath(const QString &cacheKey) const;
    bool loadTrackPageFromDisk(const QString &cacheKey, CachedTrackPage *page) const;
    void saveTrackPageToDisk(const QString &cacheKey, const CachedTrackPage &page) const;
    QString metadataCachePath(const QString &cacheKey) const;
    bool loadMetadataFromDisk(const QString &cacheKey, QJsonObject *value,
                              qint64 *expiresAtMs) const;
    void saveMetadataToDisk(const QString &cacheKey, const QJsonObject &value,
                            qint64 lifetimeMs) const;
    void clearApiCache();
    QString imageCachePath(const QString &imageUrl) const;
    bool loadImageFromDisk(const QString &imageUrl, QImage *image, qint64 *expiresAtMs);
    void saveImageToDisk(const QString &imageUrl, const QByteArray &data);
    void pruneImageCache(const QString &newFilePath, qint64 newFileSize);
    void fetchImageFromNetwork(const QString &imageUrl, quint64 generation);
    void resetImageMemoryCache();
    void startPlaybackCompanion();
    void stopPlaybackCompanion();
    void pollPlaybackState();
    void updateEffectivePlaybackState();
    void notifyDevicesUpdated();
    void maybeStartPendingPlayback();
    void queueLocalPlaybackCommand(const QString &command, int value = -1);
    void sendPlayerCommand(const QByteArray &method, const QUrl &url, const QByteArray &body);
    void notifyPlaybackState();
    bool saveSession(QString *errorMessage = nullptr) const;
    QString sessionFilePath() const;
    void fail(const QString &message);
    void setState(const QString &message, bool connected, bool busy);

    QNetworkAccessManager *networkManager_ = nullptr;
    QNetworkDiskCache *httpCache_ = nullptr;
    QTcpServer *callbackServer_ = nullptr;
    QTcpServer *playbackServer_ = nullptr;
    QProcess *playbackBrowserProcess_ = nullptr;
    QTimer *loginTimeout_ = nullptr;
    QTimer *refreshTimer_ = nullptr;
    QTimer *profileRetryTimer_ = nullptr;
    QTimer *playbackPollTimer_ = nullptr;
    QTimer *playbackStartupTimer_ = nullptr;

    QString clientId_;
    QString codeVerifier_;
    QString state_;
    QString redirectUri_;
    QString playbackKey_;
    QString playbackDeviceId_;
    QString playbackBrowserProfilePath_;
    void *playbackBrowserJob_ = nullptr;
    QStringList pendingPlaybackUris_;
    QList<QJsonObject> pendingPlaybackCommands_;
    QList<SpotifyDeviceResult> availableDevices_;
    QString availableDevicesError_;
    QList<std::function<void()>> devicesRefreshCallbacks_;
    QString pendingPlaybackContextUri_;
    QString pendingPlaybackOffsetUri_;
    QByteArray accessToken_;
    QByteArray refreshToken_;
    QHash<QString, CachedTrackPage> trackPageCache_;
    struct CachedImage
    {
        QImage image;
        qint64 expiresAtMs = 0;
    };
    QHash<QString, CachedImage> imageMemoryCache_;
    QHash<QString, QList<ImageHandler>> pendingImageHandlers_;
    SpotifyPlaybackState localPlaybackState_;
    SpotifyPlaybackState apiPlaybackState_;
    qint64 lastDevicesRefreshMs_ = 0;
    qint64 apiRateLimitUntilMs_ = 0;
    qint64 lastApiRateLimitMs_ = 0;
    int preferredPlaybackVolumePercent_ = 25;
    int consecutiveApiRateLimits_ = 0;
    bool apiPlaybackStateKnown_ = false;
    bool playbackPollInFlight_ = false;
    bool devicesPollInFlight_ = false;
    bool playbackScopeWarningShown_ = false;
    bool apiRateLimitStatusShown_ = false;
    bool connected_ = false;
    bool playbackPageOpened_ = false;
    QString currentUserDisplayName_;
    QString currentUserImageUrl_;
    QString apiRateLimitReason_;
    qint64 imageCacheMaxBytes_ = 10LL * 1024 * 1024 * 1024;
    quint64 imageCacheGeneration_ = 0;
    SpotifyPlaybackState playbackState_;

    StateHandler stateHandler_;
    ErrorHandler errorHandler_;
    PlaybackStateHandler playbackStateHandler_;
    PlaybackStatusHandler playbackStatusHandler_;
    DevicesUpdatedHandler devicesUpdatedHandler_;
};
