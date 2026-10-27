#include "main_window.h"
#include "spotify_connection.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractTableModel>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QBrush>
#include <QEasingCurve>
#include <QColor>
#include <QCursor>
#include <QChildEvent>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QElapsedTimer>
#include <QEvent>
#include <QFontMetrics>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGraphicsOpacityEffect>
#include <QHoverEvent>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLinearGradient>
#include <QList>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLocale>
#include <QMouseEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointer>
#include <QPair>
#include <QPropertyAnimation>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QResizeEvent>
#include <QRegion>
#include <QRegularExpression>
#include <QScreen>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QScrollBar>
#include <QScrollArea>
#include <QSettings>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QSplitter>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QStringList>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWidget>
#include <QMessageBox>
#include <QWheelEvent>
#include <QWindow>

#include <functional>
#include <algorithm>
#include <cmath>
#include <limits>

class MarqueeLabel final : public QLabel
{
public:
    explicit MarqueeLabel(const QString &text = {}, QWidget *parent = nullptr)
        : QLabel(parent), animationTimer_(new QTimer(this))
    {
        animationTimer_->setInterval(25);
        animationTimer_->setTimerType(Qt::PreciseTimer);
        connect(animationTimer_, &QTimer::timeout, this, [this] { advanceMarquee(); });
        setTextFormat(Qt::PlainText);
        setWordWrap(false);
        setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        setMinimumWidth(0);
        setMinimumHeight(16);
        setMaximumHeight(28);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setText(text);
    }

    void setText(const QString &text)
    {
        if (QLabel::text() == text) {
            return;
        }
        QLabel::setText(text);
        offset_ = 0.0;
        direction_ = 1;
        pauseRemainingMs_ = 500;
        updateHoveredState(clickable_ && isPointOverText(mapFromGlobal(QCursor::pos())));
        updateMarqueeState();
        update();
    }

    void setClickHandler(std::function<void()> handler)
    {
        clickHandler_ = std::move(handler);
    }

    void setClickable(bool clickable)
    {
        clickable_ = clickable && static_cast<bool>(clickHandler_);
        setMouseTracking(clickable_);
        setAttribute(Qt::WA_Hover, clickable_);
        setCursor(clickable_ ? Qt::PointingHandCursor : Qt::ArrowCursor);
        updateHoveredState(clickable_ && isPointOverText(mapFromGlobal(QCursor::pos())));
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (clickable_ && event->button() == Qt::LeftButton
            && isPointOverText(event->position())) {
            clickHandler_();
            event->accept();
            return;
        }
        QLabel::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        updateHoveredState(clickable_ && isPointOverText(event->position()));
        QLabel::mouseMoveEvent(event);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::Enter) {
            updateHoveredState(clickable_ && isPointOverText(mapFromGlobal(QCursor::pos())));
        } else if (event->type() == QEvent::Leave) {
            updateHoveredState(false);
        } else if (event->type() == QEvent::HoverMove) {
            const auto *hoverEvent = static_cast<QHoverEvent *>(event);
            updateHoveredState(clickable_ && isPointOverText(hoverEvent->position()));
        }
        return QLabel::event(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setClipRect(contentsRect());
        QFont textFont = font();
        textFont.setUnderline(clickable_ && hovered_);
        painter.setFont(textFont);
        painter.setPen(palette().color(QPalette::WindowText));

        const QRect area = contentsRect();
        const QFontMetrics metrics(textFont);
        const int baseline = area.top() + (area.height() - metrics.height()) / 2
            + metrics.ascent();
        const qreal textLeft = area.left() - offset_;
        painter.drawText(QPointF(textLeft, baseline), text());
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        updateHoveredState(clickable_ && isPointOverText(mapFromGlobal(QCursor::pos())));
        updateMarqueeState();
    }

    void showEvent(QShowEvent *event) override
    {
        QLabel::showEvent(event);
        updateMarqueeState();
    }

    void changeEvent(QEvent *event) override
    {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
            updateMarqueeState();
        }
    }

private:
    void updateMarqueeState()
    {
        const int textWidth = QFontMetrics(font()).horizontalAdvance(text());
        const int viewportWidth = contentsRect().width();
        if (textWidth <= viewportWidth || viewportWidth <= 0) {
            animationTimer_->stop();
            offset_ = 0.0;
            update();
            return;
        }

        if (!animationTimer_->isActive()) {
            animationTimer_->start();
        }
    }

    void advanceMarquee()
    {
        if (pauseRemainingMs_ > 0) {
            pauseRemainingMs_ -= animationTimer_->interval();
            return;
        }

        const qreal maxOffset = QFontMetrics(font()).horizontalAdvance(text())
            - contentsRect().width();
        if (maxOffset <= 0.0) {
            updateMarqueeState();
            return;
        }

        offset_ += direction_ * 0.375;
        if (offset_ >= maxOffset) {
            offset_ = maxOffset;
            direction_ = -1;
            pauseRemainingMs_ = 700;
        } else if (offset_ <= 0.0) {
            offset_ = 0.0;
            direction_ = 1;
            pauseRemainingMs_ = 500;
        }
        updateHoveredState(clickable_ && isPointOverText(mapFromGlobal(QCursor::pos())));
        update();
    }

    QRectF textBounds() const
    {
        const QRect area = contentsRect();
        const QFontMetrics metrics(font());
        const int baseline = area.top() + (area.height() - metrics.height()) / 2
            + metrics.ascent();
        return QRectF(area.left() - offset_, baseline - metrics.ascent(),
                      metrics.horizontalAdvance(text()), metrics.height());
    }

    bool isPointOverText(const QPointF &point) const
    {
        return contentsRect().contains(point.toPoint()) && textBounds().contains(point);
    }

    void updateHoveredState(bool hovered)
    {
        if (hovered_ == hovered) {
            return;
        }
        hovered_ = hovered;
        update();
    }

    QTimer *animationTimer_ = nullptr;
    qreal offset_ = 0.0;
    qreal pauseRemainingMs_ = 500.0;
    int direction_ = 1;
    std::function<void()> clickHandler_;
    bool clickable_ = false;
    bool hovered_ = false;
};

class PanelImageLabel final : public QLabel
{
public:
    using QLabel::QLabel;

    void setImage(const QImage &image)
    {
        image_ = image;
        QLabel::setText(image_.isNull() ? QStringLiteral("♪") : QString{});
        update();
    }

    void clearImage()
    {
        image_ = {};
        QLabel::setText(QStringLiteral("♪"));
        update();
    }

    void setRoundedBottomCorners(bool rounded)
    {
        roundedBottomCorners_ = rounded;
        update();
    }

    void setTopFadeEnabled(bool enabled)
    {
        topFadeEnabled_ = enabled;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        if (image_.isNull() || width() <= 0 || height() <= 0) {
            QLabel::paintEvent(event);
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.fillRect(rect(), QColor(QStringLiteral("#121212")));
        QPainterPath clipPath;
        const qreal radius = 6.0;
        const qreal right = width();
        const qreal bottom = height();
        clipPath.moveTo(radius, 0);
        clipPath.lineTo(right - radius, 0);
        clipPath.quadTo(right, 0, right, radius);
        clipPath.lineTo(right, bottom - (roundedBottomCorners_ ? radius : 0));
        if (roundedBottomCorners_) {
            clipPath.quadTo(right, bottom, right - radius, bottom);
        } else {
            clipPath.lineTo(right, bottom);
        }
        clipPath.lineTo(radius, bottom);
        if (roundedBottomCorners_) {
            clipPath.quadTo(0, bottom, 0, bottom - radius);
        } else {
            clipPath.lineTo(0, bottom);
        }
        clipPath.lineTo(0, radius);
        clipPath.quadTo(0, 0, radius, 0);
        clipPath.closeSubpath();
        painter.setClipPath(clipPath);

        const QImage scaled = image_.scaled(size(), Qt::KeepAspectRatioByExpanding,
                                            Qt::SmoothTransformation);
        painter.drawImage(QPoint((width() - scaled.width()) / 2, 0), scaled);
        if (topFadeEnabled_) {
            QLinearGradient gradient(0, 0, 0, height() * 0.62);
            gradient.setColorAt(0.0, QColor(0, 0, 0, 165));
            gradient.setColorAt(0.5, QColor(0, 0, 0, 72));
            gradient.setColorAt(1.0, QColor(0, 0, 0, 0));
            painter.fillRect(rect(), gradient);
        }
    }

private:
    QImage image_;
    bool roundedBottomCorners_ = true;
    bool topFadeEnabled_ = false;
};

namespace {

constexpr int defaultSpotifyCallbackPort = 43827;
constexpr int previousDefaultSpotifyCallbackPort = 8888;
constexpr int trackNumberColumn = 0;
constexpr int trackTitleColumn = 1;
constexpr int trackAlbumColumn = 2;
constexpr int trackAddedAtColumn = 3;
constexpr int trackDurationColumn = 4;
constexpr int trackPlayingRole = Qt::UserRole + 2;
constexpr int trackPausedRole = Qt::UserRole + 3;
constexpr int trackArtistRole = Qt::UserRole + 4;
constexpr int trackArtistIdRole = Qt::UserRole + 5;
constexpr int trackAlbumIdRole = Qt::UserRole + 6;
constexpr int trackAlbumNameRole = Qt::UserRole + 7;
constexpr int libraryHeroTitleRole = Qt::UserRole + 20;
constexpr int libraryHeroSummaryRole = Qt::UserRole + 21;
constexpr int libraryHeroRowRole = Qt::UserRole + 22;
constexpr int libraryColumnHeaderRowRole = Qt::UserRole + 23;

int volumeSliderPositionToPercent(int sliderPosition)
{
    const double normalized = qBound(0, sliderPosition, 100) / 100.0;
    const int percent = qRound(normalized * normalized * 100.0);
    return sliderPosition > 0 ? qMax(1, percent) : 0;
}

int volumePercentToSliderPosition(int volumePercent)
{
    const double normalized = qBound(0, volumePercent, 100) / 100.0;
    return qRound(std::sqrt(normalized) * 100.0);
}
constexpr int libraryHeroCreatorRole = Qt::UserRole + 30;
constexpr int artistReleaseDateRole = Qt::UserRole + 31;
constexpr int libraryItemTypeRole = Qt::UserRole + 24;
constexpr int libraryItemCreatorRole = Qt::UserRole + 25;
constexpr int libraryItemCategoryRole = Qt::UserRole + 26;
constexpr int libraryItemUriRole = Qt::UserRole + 27;
constexpr int libraryItemUrlRole = Qt::UserRole + 28;
constexpr int libraryItemArtworkUrlRole = Qt::UserRole + 29;
constexpr int searchArtistImageUrlRole = Qt::UserRole + 40;
constexpr int homePageIndex = 0;
constexpr int searchPageIndex = 1;
constexpr int libraryPageIndex = 2;
constexpr int settingsPageIndex = 3;
constexpr int artistPageIndex = 4;
constexpr int albumPageIndex = 5;

class TrackTableView final : public QTableView
{
public:
    explicit TrackTableView(QWidget *parent = nullptr)
        : QTableView(parent)
    {
        titleClickTimer_.setSingleShot(true);
        connect(&titleClickTimer_, &QTimer::timeout, this, [this] {
            const QString albumId = pendingTrackAlbumId_;
            const QString albumName = pendingTrackAlbumName_;
            pendingTrackAlbumId_.clear();
            pendingTrackAlbumName_.clear();
            if (!albumId.isEmpty() && albumClickHandler_) {
                albumClickHandler_(albumId, albumName);
            }
        });
    }

    void setResponsiveColumnWeights(const QVector<int> &weights)
    {
        columnWeights_ = weights;
        resizeWeightedColumns();
    }

    void setRightPaddingWidth(int width)
    {
        rightPaddingWidth_ = qMax(0, width);
        resizeWeightedColumns();
    }

    void setMinimumNumberColumnWidth(int width)
    {
        minimumNumberColumnWidth_ = qMax(0, width);
        resizeWeightedColumns();
    }

    void setRoundedCornerRadius(int radius)
    {
        roundedCornerRadius_ = qMax(0, radius);
        updateRoundedMask();
    }

    void setLibraryArtwork(const QImage &image)
    {
        libraryArtwork_ = image;
        QColor upper(40, 43, 40);
        QColor lower(34, 36, 34);
        if (!image.isNull()) {
            const QImage samples = image.scaled(16, 16, Qt::IgnoreAspectRatio,
                                                Qt::FastTransformation)
                                       .convertToFormat(QImage::Format_RGB32);
            qint64 upperR = 0, upperG = 0, upperB = 0, upperCount = 0;
            qint64 lowerR = 0, lowerG = 0, lowerB = 0, lowerCount = 0;
            for (int y = 0; y < samples.height(); ++y) {
                for (int x = 0; x < samples.width(); ++x) {
                    const QColor color = samples.pixelColor(x, y);
                    if (y < samples.height() / 2) {
                        upperR += color.red(); upperG += color.green(); upperB += color.blue();
                        ++upperCount;
                    } else {
                        lowerR += color.red(); lowerG += color.green(); lowerB += color.blue();
                        ++lowerCount;
                    }
                }
            }
            const auto tint = [](qint64 red, qint64 green, qint64 blue, qint64 count,
                                 qreal imageWeight) {
                const QColor neutral(24, 24, 24);
                if (count <= 0) return neutral;
                return QColor(qRound((red / static_cast<qreal>(count)) * imageWeight
                                     + neutral.red() * (1.0 - imageWeight)),
                              qRound((green / static_cast<qreal>(count)) * imageWeight
                                     + neutral.green() * (1.0 - imageWeight)),
                              qRound((blue / static_cast<qreal>(count)) * imageWeight
                                     + neutral.blue() * (1.0 - imageWeight)));
            };
            upper = tint(upperR, upperG, upperB, upperCount, 0.55);
            lower = tint(lowerR, lowerG, lowerB, lowerCount, 0.45);
        }
        libraryBackdropUpper_ = upper;
        libraryBackdropLower_ = lower;
        viewport()->update();
    }

    const QImage &libraryArtwork() const { return libraryArtwork_; }
    QColor libraryBackdropUpper() const { return libraryBackdropUpper_; }
    QColor libraryBackdropLower() const { return libraryBackdropLower_; }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QTableView::resizeEvent(event);
        resizeWeightedColumns();
        updateRoundedMask();
    }

    bool viewportEvent(QEvent *event) override
    {
        if (event->type() == QEvent::Enter) {
            const QPoint position = viewport()->mapFromGlobal(QCursor::pos());
            const int row = indexAt(position).row();
            if (property("hoveredRow").toInt() != row) {
                setProperty("hoveredRow", row);
                viewport()->update();
            }
            updateHoveredArtist(position);
        } else if (event->type() == QEvent::MouseMove) {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            const QPoint position = mouseEvent->position().toPoint();
            const int row = indexAt(position).row();
            if (property("hoveredRow").toInt() != row) {
                setProperty("hoveredRow", row);
                viewport()->update();
            }
            updateHoveredArtist(position);
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            const QModelIndex index = indexAt(mouseEvent->position().toPoint());
            if (index.isValid() && index.column() == trackNumberColumn) {
                suppressNextActionRelease_ = true;
            }
            if (index.isValid() && index.column() == trackTitleColumn
                && trackTitleTextRect(index).contains(mouseEvent->position().toPoint())) {
                titleClickTimer_.stop();
                pendingTrackAlbumId_.clear();
                pendingTrackAlbumName_.clear();
                suppressNextTitleAlbumClickRelease_ = true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            const QModelIndex index = indexAt(mouseEvent->position().toPoint());
            const bool activateArtist = mouseEvent->button() == Qt::LeftButton
                && index.isValid() && index.column() == trackTitleColumn
                && !index.data(trackArtistIdRole).toString().isEmpty()
                && artistTextRect(index).contains(mouseEvent->position().toPoint());
            const QString artistId = activateArtist
                ? index.data(trackArtistIdRole).toString() : QString{};
            const QString artistName = activateArtist
                ? index.data(trackArtistRole).toString().section(QStringLiteral(", "), 0, 0).trimmed()
                : QString{};
            const bool activateAlbum = mouseEvent->button() == Qt::LeftButton
                && index.isValid() && index.column() == trackAlbumColumn
                && !index.data(trackAlbumIdRole).toString().isEmpty();
            const QString albumId = activateAlbum
                ? index.data(trackAlbumIdRole).toString() : QString{};
            const QString albumName = activateAlbum
                ? index.data(Qt::DisplayRole).toString() : QString{};
            const bool activateTrackAlbum = mouseEvent->button() == Qt::LeftButton
                && index.isValid() && index.column() == trackTitleColumn
                && !index.data(trackAlbumIdRole).toString().isEmpty()
                && trackTitleTextRect(index).contains(mouseEvent->position().toPoint());
            const QString trackAlbumId = activateTrackAlbum
                ? index.data(trackAlbumIdRole).toString() : QString{};
            const QString trackAlbumName = activateTrackAlbum
                ? index.data(trackAlbumNameRole).toString() : QString{};
            const bool activateTrack = mouseEvent->button() == Qt::LeftButton
                && index.isValid() && index.column() == trackNumberColumn;
            const bool activateSortHeader = mouseEvent->button() == Qt::LeftButton
                && index.isValid() && index.data(libraryColumnHeaderRowRole).toBool()
                && index.column() >= trackTitleColumn
                && index.column() <= trackDurationColumn;
            const int row = index.row();
            const int sortColumn = index.column();
            const bool handled = QTableView::viewportEvent(event);
            if (activateSortHeader && headerSortHandler_) {
                headerSortHandler_(sortColumn);
            }
            if (activateArtist && artistClickHandler_) {
                artistClickHandler_(artistId, artistName);
            }
            if (activateAlbum && albumClickHandler_) {
                albumClickHandler_(albumId, albumName);
            }
            const bool suppressTitleAlbumClick = suppressNextTitleAlbumClickRelease_;
            if (activateTrackAlbum && !suppressTitleAlbumClick) {
                pendingTrackAlbumId_ = trackAlbumId;
                pendingTrackAlbumName_ = trackAlbumName;
                titleClickTimer_.start(QApplication::doubleClickInterval());
            } else if (mouseEvent->button() == Qt::LeftButton
                       && !suppressTitleAlbumClick) {
                titleClickTimer_.stop();
                pendingTrackAlbumId_.clear();
                pendingTrackAlbumName_.clear();
            }
            if (activateTrack && trackActionHandler_ && !suppressNextActionRelease_) {
                trackActionHandler_(row);
            }
            suppressNextActionRelease_ = false;
            suppressNextTitleAlbumClickRelease_ = false;
            return handled;
        } else if (event->type() == QEvent::Leave && property("hoveredRow").toInt() != -1) {
            setProperty("hoveredRow", -1);
            viewport()->update();
        }
        if (event->type() == QEvent::Leave) {
            updateHoveredArtist({-1, -1});
        }
        return QTableView::viewportEvent(event);
    }
public:
    void setTrackActionHandler(std::function<void(int)> handler)
    {
        trackActionHandler_ = handler;
    }

    void setHeaderSortHandler(std::function<void(int)> handler)
    {
        headerSortHandler_ = handler;
    }

    void setArtistClickHandler(std::function<void(const QString &, const QString &)> handler)
    {
        artistClickHandler_ = std::move(handler);
    }

    void setAlbumClickHandler(std::function<void(const QString &, const QString &)> handler)
    {
        albumClickHandler_ = std::move(handler);
    }

private:
    QRect artistTextRect(const QModelIndex &index) const
    {
        if (!index.isValid() || index.column() != trackTitleColumn
            || index.data(trackArtistIdRole).toString().isEmpty()) {
            return {};
        }
        const QRect itemRect = visualRect(index);
        const int textLeft = itemRect.left() + 9 + 42;
        const int textWidth = qMax(0, itemRect.right() - 8 - textLeft + 1);
        QFont artistFont = font();
        artistFont.setBold(false);
        const QString visibleText = QFontMetrics(artistFont).elidedText(
            index.data(trackArtistRole).toString(), Qt::ElideRight, textWidth);
        const int textWidthVisible = QFontMetrics(artistFont).horizontalAdvance(visibleText);
        return QRect(textLeft, itemRect.top() + 27, textWidthVisible + 2, 18);
    }

    void updateHoveredArtist(const QPoint &position)
    {
        const QModelIndex index = indexAt(position);
        const bool hoveredArtist = index.isValid() && artistTextRect(index).contains(position);
        const bool hoveredTrackTitle = index.isValid()
            && !index.data(trackAlbumIdRole).toString().isEmpty()
            && trackTitleTextRect(index).contains(position);
        const bool hoveredAlbum = index.isValid() && index.column() == trackAlbumColumn
            && !index.data(trackAlbumIdRole).toString().isEmpty();
        const int row = hoveredArtist ? index.row() : -1;
        const int trackTitleRow = hoveredTrackTitle ? index.row() : -1;
        if (property("hoveredArtistRow").toInt() != row
            || property("hoveredArtistText").toBool() != hoveredArtist
            || property("hoveredTrackTitleRow").toInt() != trackTitleRow
            || property("hoveredAlbumRow").toInt() != (hoveredAlbum ? index.row() : -1)) {
            setProperty("hoveredArtistRow", row);
            setProperty("hoveredArtistText", hoveredArtist);
            setProperty("hoveredTrackTitleRow", trackTitleRow);
            setProperty("hoveredAlbumRow", hoveredAlbum ? index.row() : -1);
            viewport()->update();
        }
        viewport()->setCursor(hoveredArtist || hoveredTrackTitle || hoveredAlbum
                                  ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }

    QRect trackTitleTextRect(const QModelIndex &index) const
    {
        if (!index.isValid() || index.column() != trackTitleColumn
            || index.data(trackAlbumIdRole).toString().isEmpty()) {
            return {};
        }
        const QRect itemRect = visualRect(index);
        const int textLeft = itemRect.left() + 9 + 42;
        const int textWidth = qMax(0, itemRect.right() - 8 - textLeft + 1);
        QFont titleFont = font();
        titleFont.setBold(index.data(trackPlayingRole).toBool());
        const QFontMetrics metrics(titleFont);
        const QString visibleTitle = metrics.elidedText(
            index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textWidth);
        const int visibleWidth = metrics.horizontalAdvance(visibleTitle);
        const bool hasArtist = !index.data(trackArtistRole).toString().isEmpty();
        const int top = itemRect.top() + (hasArtist ? 5 : 0);
        const int height = hasArtist ? 21 : itemRect.height();
        return QRect(textLeft, top, qMin(textWidth, visibleWidth + 2), height);
    }

    void updateRoundedMask()
    {
        if (roundedCornerRadius_ <= 0 || width() <= 0 || height() <= 0) {
            clearMask();
            return;
        }
        QPainterPath roundedRect;
        roundedRect.addRoundedRect(QRectF(rect()), roundedCornerRadius_,
                                   roundedCornerRadius_);
        setMask(QRegion(roundedRect.toFillPolygon().toPolygon()));
    }

    void resizeWeightedColumns()
    {
        if (columnWeights_.isEmpty() || !model() || viewport()->width() <= 0) {
            return;
        }
        const int weightedColumnCount = qMin(model()->columnCount(), columnWeights_.size());
        const bool hasRightPadding = rightPaddingWidth_ > 0
            && model()->columnCount() > weightedColumnCount;
        int totalWeight = 0;
        for (int column = 0; column < weightedColumnCount; ++column) {
            totalWeight += qMax(0, columnWeights_.at(column));
        }
        if (totalWeight <= 0) return;

        // Leave a pixel of slack for Qt's header/viewport rounding so the
        // final section never creates a horizontal scrollbar.
        const int contentWidth = qMax(1, viewport()->width() - 1);
        const int paddingWidth = hasRightPadding
            ? qMin(rightPaddingWidth_, qMax(0, contentWidth - 1)) : 0;
        const int availableWidth = qMax(1, contentWidth - paddingWidth);
        const int firstColumnWidth = weightedColumnCount > 1
            ? qMin(qMax(1, availableWidth - (weightedColumnCount - 1)),
                   qMax(minimumNumberColumnWidth_,
                        availableWidth * qMax(0, columnWeights_.at(0)) / totalWeight))
            : availableWidth;
        int usedWidth = 0;
        int remainingWeight = qMax(1, totalWeight - qMax(0, columnWeights_.at(0)));
        for (int column = 0; column < weightedColumnCount; ++column) {
            int width = firstColumnWidth;
            if (column > 0 && column < weightedColumnCount - 1) {
                const int remainingWidth = qMax(1, availableWidth - usedWidth);
                const int slotsAfter = weightedColumnCount - column - 1;
                width = qBound(1,
                    remainingWidth * qMax(0, columnWeights_.at(column))
                        / remainingWeight,
                    qMax(1, remainingWidth - slotsAfter));
                remainingWeight -= qMax(0, columnWeights_.at(column));
            } else if (column == weightedColumnCount - 1) {
                width = qMax(1, availableWidth - usedWidth);
            }
            setColumnWidth(column, width);
            usedWidth += width;
        }
        if (hasRightPadding) {
            setColumnWidth(weightedColumnCount, paddingWidth);
        }
    }

    std::function<void(int)> trackActionHandler_;
    std::function<void(int)> headerSortHandler_;
    std::function<void(const QString &, const QString &)> artistClickHandler_;
    std::function<void(const QString &, const QString &)> albumClickHandler_;
    QTimer titleClickTimer_;
    QString pendingTrackAlbumId_;
    QString pendingTrackAlbumName_;
    bool suppressNextActionRelease_ = false;
    bool suppressNextTitleAlbumClickRelease_ = false;
    QVector<int> columnWeights_;
    int roundedCornerRadius_ = 0;
    int rightPaddingWidth_ = 0;
    int minimumNumberColumnWidth_ = 0;
    QImage libraryArtwork_;
    QColor libraryBackdropUpper_{40, 43, 40};
    QColor libraryBackdropLower_{34, 36, 34};
};

QIcon trackActionIcon(bool pause, const QColor &color);
QIcon durationHeaderIcon();
QIcon nowPlayingPanelIcon();
QPixmap circularAvatarPixmap(const QImage &image, const QSize &size);
QIcon likedSongsIcon();

class HoverScrollBarBehavior final : public QObject
{
public:
    explicit HoverScrollBarBehavior(QAbstractScrollArea *area)
        : QObject(area), area_(area)
    {
        if (!area_) return;
        installOnTree(area_);
        if (QWidget *window = area_->window()) {
            window->installEventFilter(this);
        }
        if (QApplication::instance()) {
            QApplication::instance()->installEventFilter(this);
        }
        for (QScrollBar *bar : {area_->verticalScrollBar(), area_->horizontalScrollBar()}) {
            if (!bar) continue;
            bar->setProperty("sectionHovered", false);
            bar->setProperty("sectionHoveredTarget", false);
            bar->setMouseTracking(true);
            auto *opacity = new QGraphicsOpacityEffect(bar);
            opacity->setOpacity(0.0);
            bar->setGraphicsEffect(opacity);
            auto *animation = new QPropertyAnimation(opacity, "opacity", this);
            animation->setDuration(180);
            animation->setEasingCurve(QEasingCurve::OutCubic);
            connect(animation, &QPropertyAnimation::finished, bar, [bar, opacity] {
                if (bar->property("sectionHoveredTarget").toBool()
                    || opacity->opacity() > 0.001) {
                    return;
                }
                bar->setProperty("sectionHovered", false);
                if (QStyle *barStyle = bar->style()) {
                    barStyle->unpolish(bar);
                    barStyle->polish(bar);
                }
                bar->update();
            });
            opacityEffects_.insert(bar, opacity);
            opacityAnimations_.insert(bar, animation);
            bar->update();
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (!area_) return false;
#ifdef Q_OS_MACOS
        if (watched == area_->viewport() && event->type() == QEvent::Wheel) {
            auto *wheelEvent = static_cast<QWheelEvent *>(event);
            QScrollBar *scrollBar = area_->verticalScrollBar();
            const int pixelDelta = wheelEvent->pixelDelta().y();
            const int angleDelta = wheelEvent->angleDelta().y();
            const int movement = pixelDelta != 0
                ? pixelDelta
                : qRound(static_cast<double>(angleDelta) * scrollBar->singleStep() * 3.0 / 120.0);
            const int currentValue = scrollBar->value();
            const int targetValue = qBound(scrollBar->minimum(), currentValue - movement,
                                           scrollBar->maximum());
            if (targetValue != currentValue) {
                scrollBar->setValue(targetValue);
                wheelEvent->accept();
                return true;
            }
        }
#endif
        if (event->type() == QEvent::Enter) {
            setHovered(true);
        } else if (event->type() == QEvent::Leave) {
            QPointer<QAbstractScrollArea> guard(area_);
            const bool leavingWindow = watched == area_->window();
            QTimer::singleShot(0, this, [this, guard, leavingWindow] {
                if (!guard) return;
                const QPoint cursor = QCursor::pos();
                const bool inside = leavingWindow
                    ? guard->window()->frameGeometry().contains(cursor)
                    : guard->rect().contains(guard->mapFromGlobal(cursor));
                setHovered(inside);
            });
        } else if (event->type() == QEvent::ApplicationDeactivate
                   || (watched == area_->window()
                       && (event->type() == QEvent::WindowDeactivate
                           || event->type() == QEvent::Hide))) {
            setHovered(false);
        } else if (event->type() == QEvent::ChildAdded) {
            auto *childEvent = static_cast<QChildEvent *>(event);
            if (auto *widget = qobject_cast<QWidget *>(childEvent->child())) {
                installOnTree(widget);
            }
        }
        Q_UNUSED(watched);
        return false;
    }

private:
    void installOnTree(QWidget *widget)
    {
        if (!widget) return;
        widget->installEventFilter(this);
        widget->setMouseTracking(true);
        const auto children = widget->findChildren<QWidget *>(QString{}, Qt::FindDirectChildrenOnly);
        for (QWidget *child : children) installOnTree(child);
    }

    void setHovered(bool hovered)
    {
        if (!area_) return;
        for (QScrollBar *bar : {area_->verticalScrollBar(), area_->horizontalScrollBar()}) {
            if (!bar || bar->property("sectionHoveredTarget").toBool() == hovered) continue;
            bar->setProperty("sectionHoveredTarget", hovered);
            if (hovered && !bar->property("sectionHovered").toBool()) {
                bar->setProperty("sectionHovered", true);
                if (QStyle *barStyle = bar->style()) {
                    barStyle->unpolish(bar);
                    barStyle->polish(bar);
                }
            }
            auto *effect = opacityEffects_.value(bar);
            auto *animation = opacityAnimations_.value(bar);
            if (effect && animation) {
                animation->stop();
                animation->setStartValue(effect->opacity());
                animation->setEndValue(hovered ? 1.0 : 0.0);
                animation->start();
            }
            bar->update();
        }
    }

    QPointer<QAbstractScrollArea> area_;
    QHash<QScrollBar *, QGraphicsOpacityEffect *> opacityEffects_;
    QHash<QScrollBar *, QPropertyAnimation *> opacityAnimations_;
};

class TrackRowDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void initStyleOption(QStyleOptionViewItem *option,
                         const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        const auto *table = qobject_cast<const QTableView *>(parent());
        if (!table) {
            return;
        }

        if (table->property("hoveredRow").toInt() == index.row()
            && !index.data(libraryHeroRowRole).toBool()) {
            option->state |= QStyle::State_MouseOver;
            option->backgroundBrush = QColor(QStringLiteral("#292929"));
        }
        if (index.column() == trackTitleColumn
            && index.data(trackPlayingRole).toBool()) {
            const QColor green(QStringLiteral("#1ed760"));
            option->palette.setColor(QPalette::Text, green);
            option->palette.setColor(QPalette::HighlightedText, green);
            option->font.setBold(true);
        }
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const auto *table = qobject_cast<const QTableView *>(parent());
        const bool hovered = table && table->property("hoveredRow").toInt() == index.row()
            && !index.data(libraryHeroRowRole).toBool();
        if (table && table->objectName() == QStringLiteral("libraryTracks")) {
            paintLibraryItem(painter, option, index, dynamic_cast<const TrackTableView *>(table),
                             hovered);
            return;
        }
        if (index.column() == 0 && index.data(libraryHeroRowRole).toBool()) {
            painter->save();
            painter->fillRect(option.rect, QColor(QStringLiteral("#181818")));
            const QRect textRect = option.rect.adjusted(24, 12, -24, -8);
            const QRect titleRect(textRect.left(), textRect.top(), textRect.width(), 42);
            const QRect summaryRect(textRect.left(), textRect.top() + 48,
                                    textRect.width(), 24);
            QFont titleFont = option.font;
            titleFont.setPointSizeF(qMax(22.0, titleFont.pointSizeF() + 11.0));
            titleFont.setBold(true);
            painter->setFont(titleFont);
            painter->setPen(Qt::white);
            painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(titleFont).elidedText(
                                  index.data(libraryHeroTitleRole).toString(),
                                  Qt::ElideRight, titleRect.width()));
            QFont summaryFont = option.font;
            summaryFont.setPointSizeF(qMax(9.0, summaryFont.pointSizeF() - 1.0));
            painter->setFont(summaryFont);
            painter->setPen(QColor(QStringLiteral("#b3b3b3")));
            painter->drawText(summaryRect, Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(summaryFont).elidedText(
                                  index.data(libraryHeroSummaryRole).toString(),
                                  Qt::ElideRight, summaryRect.width()));
            painter->restore();
            return;
        }
        if (index.data(libraryColumnHeaderRowRole).toBool()) {
            QStyleOptionViewItem headerOption(option);
            initStyleOption(&headerOption, index);
            headerOption.backgroundBrush = QColor(QStringLiteral("#181818"));
            headerOption.palette.setColor(QPalette::Text, QColor(QStringLiteral("#b3b3b3")));
            headerOption.font.setPointSizeF(qMax(8.0, headerOption.font.pointSizeF() - 1.0));
            if (index.column() == trackNumberColumn || index.column() == trackDurationColumn) {
                headerOption.displayAlignment = Qt::AlignRight | Qt::AlignVCenter;
            } else {
                headerOption.displayAlignment = Qt::AlignLeft | Qt::AlignVCenter;
            }
            painter->save();
            painter->fillRect(option.rect, QColor(QStringLiteral("#181818")));
            QStyledItemDelegate::paint(painter, headerOption, index);
            painter->restore();
            return;
        }
        if (index.column() == trackTitleColumn) {
            QStyleOptionViewItem backgroundOption(option);
            initStyleOption(&backgroundOption, index);
            backgroundOption.text.clear();
            backgroundOption.icon = {};
            table->style()->drawControl(QStyle::CE_ItemViewItem, &backgroundOption,
                                        painter, table);

            painter->save();
            const QRect contentRect = option.rect.adjusted(9, 0, -8, 0);
            const QRect artworkRect(contentRect.left(),
                                    option.rect.center().y() - 16, 32, 32);
            const QIcon artwork = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
            if (!artwork.isNull()) {
                const QPixmap pixmap = artwork.pixmap(QSize(32, 32), QIcon::Normal, QIcon::Off);
                painter->drawPixmap(artworkRect, pixmap);
            }

            const QRect textRect = contentRect.adjusted(42, 0, 0, 0);
            const QString title = index.data(Qt::DisplayRole).toString();
            const QString artist = index.data(trackArtistRole).toString();
            const bool isPlaying = index.data(trackPlayingRole).toBool();
            QFont titleFont = option.font;
            titleFont.setBold(isPlaying);
            titleFont.setUnderline(!index.data(trackAlbumIdRole).toString().isEmpty()
                && table->property("hoveredTrackTitleRow").toInt() == index.row());
            painter->setFont(titleFont);
            painter->setPen(isPlaying ? QColor(QStringLiteral("#1ed760"))
                                      : option.palette.color(QPalette::Text));
            const QFontMetrics titleMetrics(titleFont);
            if (artist.isEmpty()) {
                painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                                  titleMetrics.elidedText(title, Qt::ElideRight,
                                                          textRect.width()));
            } else {
                const QRect titleRect(textRect.left(), textRect.top() + 5,
                                      textRect.width(), 21);
                const QRect artistRect(textRect.left(), textRect.top() + 27,
                                       textRect.width(), 18);
                painter->drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft,
                                  titleMetrics.elidedText(title, Qt::ElideRight,
                                                          titleRect.width()));
                QFont artistFont = option.font;
                artistFont.setBold(false);
                if (table->property("hoveredArtistText").toBool()
                    && table->property("hoveredArtistRow").toInt() == index.row()) {
                    artistFont.setUnderline(true);
                }
                painter->setFont(artistFont);
                painter->setPen(QColor(QStringLiteral("#b3b3b3")));
                const QFontMetrics artistMetrics(artistFont);
                painter->drawText(artistRect, Qt::AlignVCenter | Qt::AlignLeft,
                                  artistMetrics.elidedText(artist, Qt::ElideRight,
                                                           artistRect.width()));
            }
            painter->restore();
        } else if (hovered && index.column() == trackNumberColumn) {
            QStyleOptionViewItem numberOption(option);
            initStyleOption(&numberOption, index);
            numberOption.text.clear();
            table->style()->drawControl(QStyle::CE_ItemViewItem, &numberOption,
                                        painter, table);
            const bool isPlaying = index.data(trackPlayingRole).toBool();
            const bool isPaused = index.data(trackPausedRole).toBool();
            const QColor color = isPlaying ? QColor(QStringLiteral("#1ed760"))
                                           : QColor(QStringLiteral("#b3b3b3"));
            const QIcon icon = trackActionIcon(isPlaying && !isPaused, color);
            const QRect iconRect(option.rect.center().x() - 10,
                                 option.rect.center().y() - 10, 20, 20);
            icon.paint(painter, iconRect, Qt::AlignCenter);
        } else {
            QStyledItemDelegate::paint(painter, option, index);
        }
        if (!hovered) {
            return;
        }

    }

private:
    static void paintLibraryBackground(QPainter *painter,
                                       const QStyleOptionViewItem &option,
                                       const TrackTableView *table,
                                       bool hovered)
    {
        const QColor neutral(QStringLiteral("#181818"));
        const int fadeEnd = 118 + 40 + 6 * 54;
        const int contentTop = table ? table->rowViewportPosition(0) : 0;
        QLinearGradient gradient(QPointF(0, contentTop),
                                 QPointF(0, contentTop + fadeEnd));
        gradient.setColorAt(0.0, table ? table->libraryBackdropUpper() : neutral);
        gradient.setColorAt(0.23, table ? table->libraryBackdropLower() : neutral);
        gradient.setColorAt(1.0, neutral);
        painter->fillRect(option.rect, gradient);
        if (hovered) {
            painter->fillRect(option.rect, QColor(0, 0, 0, 42));
        }
    }

    static void paintLibraryItem(QPainter *painter,
                                const QStyleOptionViewItem &option,
                                const QModelIndex &index,
                                const TrackTableView *table,
                                bool hovered)
    {
        painter->save();
        paintLibraryBackground(painter, option, table, hovered);

        if (index.data(libraryHeroRowRole).toBool()) {
            const QRect content = option.rect.adjusted(24, 9, -24, -9);
            const int artworkSize = qMin(96, content.height());
            const QRect artworkRect(content.left(),
                                    content.center().y() - artworkSize / 2,
                                    artworkSize, artworkSize);
            const QImage artwork = table ? table->libraryArtwork() : QImage{};
            if (!artwork.isNull()) {
                const QPixmap scaled = QPixmap::fromImage(artwork).scaled(
                    artworkRect.size(), Qt::KeepAspectRatioByExpanding,
                    Qt::SmoothTransformation);
                const QRect source((scaled.width() - artworkRect.width()) / 2,
                                   (scaled.height() - artworkRect.height()) / 2,
                                   artworkRect.width(), artworkRect.height());
                painter->drawPixmap(artworkRect, scaled, source);
            } else {
                painter->fillRect(artworkRect, QColor(QStringLiteral("#282828")));
                const QPixmap fallback = likedSongsIcon().pixmap(artworkRect.size());
                if (!fallback.isNull()) {
                    painter->drawPixmap(artworkRect, fallback);
                }
            }

            const int textLeft = artworkRect.right() + 18;
            const QRect textRect(textLeft, content.top(),
                                 qMax(0, content.right() - textLeft + 1), content.height());
            const QRect titleRect(textRect.left(), textRect.top() + 8,
                                  textRect.width(), 43);
            QFont titleFont = option.font;
            titleFont.setPointSizeF(qMax(22.0, titleFont.pointSizeF() + 11.0));
            titleFont.setBold(true);
            painter->setFont(titleFont);
            painter->setPen(Qt::white);
            painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(titleFont).elidedText(
                                  index.data(libraryHeroTitleRole).toString(),
                                  Qt::ElideRight, titleRect.width()));

            const QString creator = index.data(libraryHeroCreatorRole).toString();
            const QString summary = index.data(libraryHeroSummaryRole).toString();
            const QRect detailsRect(textRect.left(), textRect.top() + 58,
                                    textRect.width(), 24);
            QFont detailsFont = option.font;
            detailsFont.setPointSizeF(qMax(9.0, detailsFont.pointSizeF() - 1.0));
            painter->setFont(detailsFont);
            const QFontMetrics detailsMetrics(detailsFont);
            const QString visibleCreator = detailsMetrics.elidedText(
                creator, Qt::ElideRight, qMin(detailsRect.width() / 2,
                                              detailsMetrics.horizontalAdvance(creator)));
            painter->setPen(Qt::white);
            painter->drawText(detailsRect, Qt::AlignLeft | Qt::AlignVCenter, visibleCreator);
            int detailsX = detailsRect.left() + detailsMetrics.horizontalAdvance(visibleCreator);
            if (!visibleCreator.isEmpty() && !summary.isEmpty()) {
                const QString separator = QStringLiteral("  ·  ");
                painter->setPen(QColor(QStringLiteral("#b3b3b3")));
                painter->drawText(detailsRect.left() + detailsMetrics.horizontalAdvance(visibleCreator),
                                  detailsRect.top(),
                                  detailsMetrics.horizontalAdvance(separator), detailsRect.height(),
                                  Qt::AlignLeft | Qt::AlignVCenter, separator);
                detailsX += detailsMetrics.horizontalAdvance(separator);
            }
            const QRect summaryRect(detailsX, detailsRect.top(),
                                    qMax(0, detailsRect.right() - detailsX + 1),
                                    detailsRect.height());
            painter->setPen(QColor(QStringLiteral("#b3b3b3")));
            painter->drawText(summaryRect, Qt::AlignLeft | Qt::AlignVCenter,
                              detailsMetrics.elidedText(summary, Qt::ElideRight,
                                                        summaryRect.width()));
            painter->restore();
            return;
        }

        const bool isHeader = index.data(libraryColumnHeaderRowRole).toBool();
        const QString text = index.data(Qt::DisplayRole).toString();
        const QRect textRect = option.rect.adjusted(9, 0, -9, 0);
        if (isHeader) {
            QFont headerFont = option.font;
            headerFont.setPointSizeF(qMax(8.0, headerFont.pointSizeF() - 1.0));
            painter->setFont(headerFont);
            painter->setPen(QColor(QStringLiteral("#b3b3b3")));
            if (index.column() == trackDurationColumn) {
                const QRect iconRect(textRect.right() - 17, textRect.center().y() - 9,
                                     18, 18);
                durationHeaderIcon().paint(painter, iconRect, Qt::AlignCenter);
                if (!text.isEmpty()) {
                    painter->drawText(textRect.adjusted(0, 0, -21, 0),
                                      Qt::AlignRight | Qt::AlignVCenter, text);
                }
            } else {
                const Qt::Alignment alignment = index.column() == trackNumberColumn
                    ? Qt::AlignRight | Qt::AlignVCenter
                    : Qt::AlignLeft | Qt::AlignVCenter;
                painter->drawText(textRect, alignment,
                                  QFontMetrics(headerFont).elidedText(text, Qt::ElideRight,
                                                                      textRect.width()));
            }
            painter->restore();
            return;
        }

        if (index.column() == trackNumberColumn && hovered
            && !index.data(Qt::DisplayRole).toString().isEmpty()) {
            const bool isPlaying = index.data(trackPlayingRole).toBool();
            const bool isPaused = index.data(trackPausedRole).toBool();
            const QColor color = isPlaying ? QColor(QStringLiteral("#1ed760"))
                                           : QColor(QStringLiteral("#b3b3b3"));
            trackActionIcon(isPlaying && !isPaused, color).paint(
                painter, QRect(option.rect.center().x() - 10,
                               option.rect.center().y() - 10, 20, 20), Qt::AlignCenter);
        } else if (index.column() == trackTitleColumn) {
            const QRect contentRect = option.rect.adjusted(9, 0, -8, 0);
            const QRect artworkRect(contentRect.left(), option.rect.center().y() - 16,
                                    32, 32);
            const QIcon artwork = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
            if (!artwork.isNull()) {
                painter->drawPixmap(artworkRect,
                                    artwork.pixmap(QSize(32, 32), QIcon::Normal, QIcon::Off));
            }

            const QRect titleTextRect = contentRect.adjusted(42, 0, 0, 0);
            const QString title = index.data(Qt::DisplayRole).toString();
            const QString artist = index.data(trackArtistRole).toString();
            const bool isPlaying = index.data(trackPlayingRole).toBool();
            QFont titleFont = option.font;
            titleFont.setBold(isPlaying);
            titleFont.setUnderline(!index.data(trackAlbumIdRole).toString().isEmpty()
                && table->property("hoveredTrackTitleRow").toInt() == index.row());
            painter->setFont(titleFont);
            painter->setPen(isPlaying ? QColor(QStringLiteral("#1ed760"))
                                      : option.palette.color(QPalette::Text));
            const QFontMetrics titleMetrics(titleFont);
            if (artist.isEmpty()) {
                painter->drawText(titleTextRect, Qt::AlignVCenter | Qt::AlignLeft,
                                  titleMetrics.elidedText(title, Qt::ElideRight,
                                                          titleTextRect.width()));
            } else {
                const QRect titleRect(titleTextRect.left(), titleTextRect.top() + 5,
                                      titleTextRect.width(), 21);
                const QRect artistRect(titleTextRect.left(), titleTextRect.top() + 27,
                                       titleTextRect.width(), 18);
                painter->drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft,
                                  titleMetrics.elidedText(title, Qt::ElideRight,
                                                          titleRect.width()));
                QFont artistFont = option.font;
                artistFont.setBold(false);
                if (table->property("hoveredArtistText").toBool()
                    && table->property("hoveredArtistRow").toInt() == index.row()) {
                    artistFont.setUnderline(true);
                }
                painter->setFont(artistFont);
                painter->setPen(QColor(QStringLiteral("#b3b3b3")));
                painter->drawText(artistRect, Qt::AlignVCenter | Qt::AlignLeft,
                                  QFontMetrics(artistFont).elidedText(
                                      artist, Qt::ElideRight, artistRect.width()));
            }
        } else if (!text.isEmpty()) {
            const QVariant foreground = index.data(Qt::ForegroundRole);
            QFont textFont = option.font;
            if (index.column() == trackAlbumColumn
                && table->property("hoveredAlbumRow").toInt() == index.row()) {
                textFont.setUnderline(true);
            }
            painter->setFont(textFont);
            painter->setPen(foreground.canConvert<QColor>()
                                ? foreground.value<QColor>()
                                : QColor(QStringLiteral("#b3b3b3")));
            const Qt::Alignment alignment = (index.column() == trackNumberColumn
                                               || index.column() == trackDurationColumn)
                ? Qt::AlignRight | Qt::AlignVCenter
                : Qt::AlignLeft | Qt::AlignVCenter;
            painter->drawText(textRect, alignment,
                              QFontMetrics(textFont).elidedText(text, Qt::ElideRight,
                                                                textRect.width()));
        }

        painter->restore();
    }
};

class LibrarySidebarItemDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem background(option);
        initStyleOption(&background, index);
        background.text.clear();
        background.icon = {};
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);

        painter->save();
        const QRect content = option.rect.adjusted(8, 4, -8, -4);
        const QRect artworkRect(content.left(), content.center().y() - 21, 42, 42);
        const QIcon artwork = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        if (!artwork.isNull()) {
            painter->drawPixmap(artworkRect, artwork.pixmap(QSize(42, 42)));
        }

        const int textLeft = artworkRect.right() + 9;
        const QRect titleRect(textLeft, content.top() + 5,
                              qMax(0, content.right() - textLeft + 1), 20);
        const QRect subtitleRect(textLeft, content.top() + 26,
                                 qMax(0, content.right() - textLeft + 1), 18);
        const QFont titleFont = option.font;
        painter->setFont(titleFont);
        painter->setPen(QColor(QStringLiteral("#ffffff")));
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(titleFont).elidedText(index.data(Qt::DisplayRole).toString(),
                                                             Qt::ElideRight, titleRect.width()));

        const QString category = index.data(libraryItemCategoryRole).toString();
        const QString creator = index.data(libraryItemCreatorRole).toString();
        const QString subtitle = creator.isEmpty() ? category
            : QStringLiteral("%1 · %2").arg(category, creator);
        QFont subtitleFont = option.font;
        subtitleFont.setBold(false);
        subtitleFont.setPointSizeF(qMax(8.0, subtitleFont.pointSizeF() - 1.0));
        painter->setFont(subtitleFont);
        painter->setPen(QColor(QStringLiteral("#b3b3b3")));
        painter->drawText(subtitleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(subtitleFont).elidedText(subtitle, Qt::ElideRight,
                                                                subtitleRect.width()));
        painter->restore();
    }
};

class DelayedTextTooltipBehavior final : public QObject
{
public:
    explicit DelayedTextTooltipBehavior(QAbstractItemView *view)
        : QObject(view), view_(view), timer_(new QTimer(this))
    {
        if (!view_ || !view_->viewport()) return;
        view_->viewport()->installEventFilter(this);
        view_->viewport()->setMouseTracking(true);
        timer_->setSingleShot(true);
        timer_->setInterval(1000);
        connect(timer_, &QTimer::timeout, this, [this] { showPendingTooltip(); });
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (!view_ || watched != view_->viewport()) return false;
        if (event->type() == QEvent::Enter) {
            updateHover(view_->viewport()->mapFromGlobal(QCursor::pos()));
        } else if (event->type() == QEvent::MouseMove) {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            updateHover(mouseEvent->position().toPoint());
        } else if (event->type() == QEvent::Leave
                   || event->type() == QEvent::MouseButtonPress) {
            clearPendingTooltip();
        } else if (event->type() == QEvent::ToolTip) {
            // Suppress QAbstractItemView's immediate whole-cell tooltip. The
            // delayed handler below only opens it when the pointer is on text.
            return true;
        }
        return false;
    }

private:
    QVector<QRect> textRectsFor(const QModelIndex &index) const
    {
        QVector<QRect> result;
        if (!view_ || !index.isValid()) return result;
        const QRect itemRect = view_->visualRect(index);
        if (itemRect.isEmpty()) return result;

        if (auto *table = qobject_cast<QTableView *>(view_.data())) {
            if (index.column() == trackTitleColumn) {
                const int textLeft = itemRect.left() + 9 + 32 + 10;
                const int textWidth = qMax(0, itemRect.right() - 8 - textLeft + 1);
                const QString title = index.data(Qt::DisplayRole).toString();
                const QString visibleTitle = QFontMetrics(table->font()).elidedText(
                    title, Qt::ElideRight, textWidth);
                const int titleWidth = QFontMetrics(table->font())
                    .horizontalAdvance(visibleTitle);
                const QString artist = index.data(trackArtistRole).toString();
                if (artist.isEmpty()) {
                    result.append(QRect(textLeft, itemRect.top(), titleWidth + 2,
                                        itemRect.height()));
                } else {
                    result.append(QRect(textLeft, itemRect.top() + 5, titleWidth + 2, 21));
                    QFont artistFont = table->font();
                    artistFont.setBold(false);
                    const QString visibleArtist = QFontMetrics(artistFont).elidedText(
                        artist, Qt::ElideRight, textWidth);
                    const int artistWidth = QFontMetrics(artistFont)
                        .horizontalAdvance(visibleArtist);
                    result.append(QRect(textLeft, itemRect.top() + 27,
                                        artistWidth + 2, 18));
                }
            } else if (index.column() == trackAlbumColumn) {
                const int textLeft = itemRect.left() + 4;
                const int textWidth = qMax(0, itemRect.right() - 4 - textLeft + 1);
                const QString text = index.data(Qt::DisplayRole).toString();
                const QString visibleText = QFontMetrics(table->font()).elidedText(
                    text, Qt::ElideRight, textWidth);
                const int width = QFontMetrics(table->font()).horizontalAdvance(visibleText);
                result.append(QRect(textLeft, itemRect.top(), width + 2, itemRect.height()));
            }
            return result;
        }

        if (auto *list = qobject_cast<QListWidget *>(view_.data())) {
            const int textLeft = itemRect.left() + 8 + 41 + 9;
            const int textWidth = qMax(0, itemRect.right() - 8 - textLeft + 1);
            const QString title = index.data(Qt::DisplayRole).toString();
            const QString visibleTitle = QFontMetrics(list->font()).elidedText(
                title, Qt::ElideRight, textWidth);
            const int titleWidth = QFontMetrics(list->font()).horizontalAdvance(visibleTitle);
            result.append(QRect(textLeft, itemRect.top() + 9, titleWidth + 2, 20));

            const QString category = index.data(libraryItemCategoryRole).toString();
            const QString creator = index.data(libraryItemCreatorRole).toString();
            const QString subtitle = creator.isEmpty() ? category
                : QStringLiteral("%1 · %2").arg(category, creator);
            QFont subtitleFont = list->font();
            subtitleFont.setBold(false);
            subtitleFont.setPointSizeF(qMax(8.0, subtitleFont.pointSizeF() - 1.0));
            const QString visibleSubtitle = QFontMetrics(subtitleFont).elidedText(
                subtitle, Qt::ElideRight, textWidth);
            const int subtitleWidth = QFontMetrics(subtitleFont)
                .horizontalAdvance(visibleSubtitle);
            if (!subtitle.isEmpty()) {
                result.append(QRect(textLeft, itemRect.top() + 30,
                                    subtitleWidth + 2, 18));
            }
        }
        return result;
    }

    bool isOverText(const QModelIndex &index, const QPoint &position) const
    {
        const auto textRects = textRectsFor(index);
        return std::any_of(textRects.cbegin(), textRects.cend(),
                           [&position](const QRect &rect) { return rect.contains(position); });
    }

    void updateHover(const QPoint &position)
    {
        const QModelIndex index = view_->indexAt(position);
        if (!index.isValid() || !isOverText(index, position)) {
            clearPendingTooltip();
            return;
        }
        QString tooltip;
        if (auto *table = qobject_cast<QTableView *>(view_.data());
            table && index.column() == trackTitleColumn) {
            const QString artist = index.data(trackArtistRole).toString().trimmed();
            const auto textRects = textRectsFor(index);
            if (!artist.isEmpty() && textRects.size() > 1
                && textRects.at(1).contains(position)) {
                tooltip = artist;
            } else {
                tooltip = index.data(Qt::ToolTipRole).toString();
            }
        } else {
            tooltip = index.data(Qt::ToolTipRole).toString();
        }
        tooltip = tooltip.trimmed();
        if (tooltip.isEmpty()) {
            clearPendingTooltip();
            return;
        }
        if (index == pendingIndex_ && tooltip == pendingTooltip_) return;

        clearPendingTooltip();
        pendingIndex_ = index;
        pendingTooltip_ = tooltip;
        timer_->start();
    }

    void showPendingTooltip()
    {
        if (!view_ || !pendingIndex_.isValid()) return;
        const QPoint position = view_->viewport()->mapFromGlobal(QCursor::pos());
        if (view_->indexAt(position) != pendingIndex_
            || !isOverText(pendingIndex_, position)) {
            clearPendingTooltip();
            return;
        }
        QToolTip::showText(QCursor::pos(), pendingTooltip_, view_->viewport());
        tooltipVisible_ = true;
    }

    void clearPendingTooltip()
    {
        timer_->stop();
        pendingIndex_ = {};
        pendingTooltip_.clear();
        if (tooltipVisible_) {
            QToolTip::hideText();
            tooltipVisible_ = false;
        }
    }

    QPointer<QAbstractItemView> view_;
    QTimer *timer_ = nullptr;
    QModelIndex pendingIndex_;
    QString pendingTooltip_;
    bool tooltipVisible_ = false;
};

QIcon trackActionIcon(bool pause, const QColor &color)
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    if (pause) {
        painter.drawRoundedRect(QRectF(6.5, 5.0, 4.5, 14.0), 1.2, 1.2);
        painter.drawRoundedRect(QRectF(13.0, 5.0, 4.5, 14.0), 1.2, 1.2);
    } else {
        QPolygonF triangle;
        triangle << QPointF(7.0, 4.5) << QPointF(19.0, 12.0) << QPointF(7.0, 19.5);
        painter.drawPolygon(triangle);
    }
    return QIcon(pixmap);
}

QIcon playbackControlIcon(
    int glyph,
    const QColor &normalColor = QColor(QStringLiteral("#b3b3b3")),
    const QColor &activeColor = QColor(Qt::white))
{
    QIcon icon;
    for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Active, QIcon::Disabled}) {
        QPixmap pixmap(24, 24);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor color = mode == QIcon::Disabled ? QColor(QStringLiteral("#6a6a6a"))
            : mode == QIcon::Active ? activeColor : normalColor;
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);

        if (glyph == 0) { // Previous is the next glyph rotated 180 degrees.
            painter.translate(12.0, 12.0);
            painter.rotate(180.0);
            painter.translate(-12.0, -12.0);
        }
        if (glyph == 0 || glyph == 3) {
            QPolygonF triangle;
            triangle << QPointF(5.0, 5.5) << QPointF(16.0, 12.0) << QPointF(5.0, 18.5);
            painter.drawPolygon(triangle);
            painter.drawRoundedRect(QRectF(18.0, 6.0, 2.5, 12.0), 1.0, 1.0);
        } else if (glyph == 1) { // Play
            QPolygonF triangle;
            triangle << QPointF(5.0, 3.5) << QPointF(20.0, 12.0) << QPointF(5.0, 20.5);
            painter.drawPolygon(triangle);
        } else if (glyph == 2) { // Pause
            painter.drawRoundedRect(QRectF(5.0, 4.0, 5.5, 16.0), 1.2, 1.2);
            painter.drawRoundedRect(QRectF(13.5, 4.0, 5.5, 16.0), 1.2, 1.2);
        }
        icon.addPixmap(pixmap, mode, QIcon::Off);
    }
    return icon;
}

QIcon lyricsMicrophoneIcon(bool selected = false)
{
    QIcon icon;
    for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Active, QIcon::Disabled}) {
        QPixmap pixmap(24, 24);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor color = mode == QIcon::Disabled ? QColor(QStringLiteral("#6a6a6a"))
            : selected ? QColor(QStringLiteral("#1ed760"))
            : mode == QIcon::Active ? QColor(Qt::white)
                                    : QColor(QStringLiteral("#b3b3b3"));
        painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(9.0, 3.0, 6.0, 12.0), 3.0, 3.0);
        painter.drawArc(QRectF(5.5, 7.5, 13.0, 12.0), 180 * 16, 180 * 16);
        painter.drawLine(QPointF(12.0, 19.0), QPointF(12.0, 22.0));
        painter.drawLine(QPointF(8.0, 22.0), QPointF(16.0, 22.0));
        icon.addPixmap(pixmap, mode, QIcon::Off);
    }
    return icon;
}

QIcon volumeControlIcon(bool muted)
{
    QIcon icon;
    for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Active, QIcon::Disabled}) {
        QPixmap pixmap(24, 24);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor color = mode == QIcon::Active ? QColor(Qt::white)
            : mode == QIcon::Disabled ? QColor(QStringLiteral("#6a6a6a"))
                                      : QColor(QStringLiteral("#b3b3b3"));
        painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(color);
        QPolygonF speaker;
        speaker << QPointF(3.0, 9.0) << QPointF(7.0, 9.0) << QPointF(12.0, 5.0)
                << QPointF(12.0, 19.0) << QPointF(7.0, 15.0) << QPointF(3.0, 15.0);
        painter.drawPolygon(speaker);
        painter.setBrush(Qt::NoBrush);
        if (muted) {
            painter.drawLine(QPointF(15.0, 9.0), QPointF(21.0, 15.0));
            painter.drawLine(QPointF(21.0, 9.0), QPointF(15.0, 15.0));
        } else {
            QPainterPath waves;
            waves.moveTo(14.0, 8.0);
            waves.cubicTo(18.0, 10.0, 18.0, 14.0, 14.0, 16.0);
            waves.moveTo(16.0, 5.0);
            waves.cubicTo(22.0, 9.0, 22.0, 15.0, 16.0, 19.0);
            painter.drawPath(waves);
        }
        icon.addPixmap(pixmap, mode, QIcon::Off);
    }
    return icon;
}

QIcon repeatModeIcon(const QString &mode)
{
    const bool enabled = mode != QStringLiteral("off");
    QIcon icon;
    for (const QIcon::Mode iconMode : {QIcon::Normal, QIcon::Active, QIcon::Disabled}) {
        QPixmap pixmap(24, 24);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor color = iconMode == QIcon::Disabled ? QColor(QStringLiteral("#6a6a6a"))
            : iconMode == QIcon::Active ? QColor(Qt::white)
            : enabled ? QColor(QStringLiteral("#1ed760"))
                      : QColor(QStringLiteral("#b3b3b3"));
        painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        QPainterPath loop;
        loop.moveTo(5.0, 8.0);
        loop.lineTo(15.0, 8.0);
        loop.cubicTo(20.0, 8.0, 20.0, 16.0, 15.0, 16.0);
        loop.lineTo(9.0, 16.0);
        loop.cubicTo(4.0, 16.0, 4.0, 8.0, 9.0, 8.0);
        painter.drawPath(loop);
        painter.drawLine(QPointF(14.0, 5.5), QPointF(17.0, 8.0));
        painter.drawLine(QPointF(17.0, 8.0), QPointF(14.0, 10.5));
        painter.drawLine(QPointF(10.0, 13.5), QPointF(7.0, 16.0));
        painter.drawLine(QPointF(7.0, 16.0), QPointF(10.0, 18.5));
        if (mode == QStringLiteral("track")) {
            painter.setFont(QFont(QStringLiteral("Arial"), 7, QFont::Bold));
            painter.setPen(color);
            painter.drawText(QRectF(9.0, 9.0, 6.0, 7.0), Qt::AlignCenter,
                             QStringLiteral("1"));
        }
        icon.addPixmap(pixmap, iconMode, QIcon::Off);
    }
    return icon;
}

QIcon devicesIcon()
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#b3b3b3")), 1.7,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(2.7, 4.0, 14.8, 11.7), 1.8, 1.8);
    painter.drawLine(QPointF(7.4, 19.2), QPointF(12.8, 19.2));
    painter.drawLine(QPointF(10.1, 15.8), QPointF(10.1, 19.0));
    painter.drawRoundedRect(QRectF(16.2, 10.0, 5.2, 10.0), 1.1, 1.1);
    painter.drawLine(QPointF(18.0, 17.7), QPointF(19.6, 17.7));
    return QIcon(pixmap);
}

QIcon durationHeaderIcon()
{
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#b3b3b3")), 1.7,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QRectF(3.2, 3.2, 13.6, 13.6));
    painter.drawLine(QPointF(10.0, 6.3), QPointF(10.0, 10.0));
    painter.drawLine(QPointF(10.0, 10.0), QPointF(13.0, 11.8));
    return QIcon(pixmap);
}

QPixmap circularAvatarPixmap(const QImage &image, const QSize &size)
{
    QPixmap result(size);
    result.fill(Qt::transparent);
    if (image.isNull() || size.isEmpty()) {
        return result;
    }

    const QImage scaled = image.scaled(size, Qt::KeepAspectRatioByExpanding,
                                       Qt::SmoothTransformation);
    const QRect sourceRect((scaled.width() - size.width()) / 2,
                           (scaled.height() - size.height()) / 2,
                           size.width(), size.height());
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addEllipse(QRectF(0.0, 0.0, size.width(), size.height()));
    painter.setClipPath(clip);
    painter.drawImage(QRect(QPoint(0, 0), size), scaled, sourceRect);
    return result;
}

QIcon searchIcon()
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#b3b3b3")), 2.0,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawEllipse(QRectF(4.0, 4.0, 11.5, 11.5));
    painter.drawLine(QPointF(14.0, 14.0), QPointF(20.0, 20.0));
    return QIcon(pixmap);
}

QIcon likedSongsIcon()
{
    QPixmap pixmap(48, 48);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient gradient(0, 0, 48, 48);
    gradient.setColorAt(0.0, QColor(QStringLiteral("#5338a8")));
    gradient.setColorAt(1.0, QColor(QStringLiteral("#1e8c54")));
    painter.setBrush(gradient);
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(QRectF(0.5, 0.5, 47, 47), 5, 5);
    painter.setPen(QPen(Qt::white, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawLine(QPointF(18, 16), QPointF(18, 32));
    painter.drawLine(QPointF(18, 16), QPointF(31, 12));
    painter.drawLine(QPointF(31, 12), QPointF(31, 28));
    painter.setBrush(Qt::white);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QRectF(11, 29, 8, 6));
    painter.drawEllipse(QRectF(24, 25, 8, 6));
    return QIcon(pixmap);
}

QIcon homeIcon()
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#b3b3b3")), 1.9,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    QPainterPath house;
    house.moveTo(3.6, 10.2);
    house.lineTo(12.0, 3.5);
    house.lineTo(20.4, 10.2);
    house.moveTo(5.8, 9.3);
    house.lineTo(5.8, 20.2);
    house.lineTo(18.2, 20.2);
    house.lineTo(18.2, 9.3);
    painter.drawPath(house);
    painter.drawLine(QPointF(10.0, 20.0), QPointF(10.0, 13.6));
    painter.drawLine(QPointF(14.0, 20.0), QPointF(14.0, 13.6));
    painter.drawLine(QPointF(10.0, 13.6), QPointF(14.0, 13.6));
    return QIcon(pixmap);
}

QIcon nowPlayingPanelIcon()
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#b3b3b3")), 1.7,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(3.5, 4.5, 17, 15), 2.2, 2.2);
    painter.drawLine(QPointF(14.5, 5.5), QPointF(14.5, 18.5));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#b3b3b3")));
    painter.drawRoundedRect(QRectF(16.5, 7.0, 2.1, 10), 0.7, 0.7);
    return QIcon(pixmap);
}

class SmoothWheelScrollFilter final : public QObject
{
public:
    SmoothWheelScrollFilter(QAbstractItemView *view, int linePixels)
        : QObject(view), view_(view), linePixels_(linePixels)
    {
        view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        view_->verticalScrollBar()->setSingleStep(linePixels_);
        timer_.setSingleShot(true);
        timer_.setTimerType(Qt::PreciseTimer);
        connect(&timer_, &QTimer::timeout, this, [this] { advanceScroll(); });
        view_->viewport()->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != view_->viewport() || event->type() != QEvent::Wheel) {
            return QObject::eventFilter(watched, event);
        }

        auto *wheelEvent = static_cast<QWheelEvent *>(event);
        const int pixelDelta = wheelEvent->pixelDelta().y();
        const int angleDelta = wheelEvent->angleDelta().y();
        if (pixelDelta == 0 && angleDelta == 0) {
            return QObject::eventFilter(watched, event);
        }

        QScrollBar *scrollBar = view_->verticalScrollBar();
        const int baseTarget = timer_.isActive() ? targetValue_ : scrollBar->value();
        const int movement = pixelDelta != 0
            ? pixelDelta
            : qRound(static_cast<double>(angleDelta) * linePixels_ * 3.0 / 120.0);
        targetValue_ = qBound(scrollBar->minimum(), baseTarget - movement,
                              scrollBar->maximum());
        startValue_ = scrollBar->value();
        durationMs_ = pixelDelta != 0 ? 180 : 260;
        elapsed_.restart();
        framePeriodMs_ = currentFramePeriodMs();
        nextFrameDeadlineMs_ = framePeriodMs_;
        scheduleNextFrame();
        wheelEvent->accept();
        return true;
    }

private:
    qreal currentFramePeriodMs() const
    {
        const QScreen *screen = view_->window()->screen();
        const qreal rate = screen ? screen->refreshRate() : 60.0;
        return 1000.0 / (rate > 1.0 ? rate : 60.0);
    }

    void scheduleNextFrame()
    {
        const qreal nowMs = elapsed_.nsecsElapsed() / 1000000.0;
        framePeriodMs_ = currentFramePeriodMs();
        if (nextFrameDeadlineMs_ <= nowMs) {
            const qreal missedFrames = std::floor((nowMs - nextFrameDeadlineMs_) / framePeriodMs_) + 1.0;
            nextFrameDeadlineMs_ += missedFrames * framePeriodMs_;
        }
        timer_.start(qMax(1, qCeil(nextFrameDeadlineMs_ - nowMs)));
        nextFrameDeadlineMs_ += framePeriodMs_;
    }

    void advanceScroll()
    {
        const qreal elapsedMs = elapsed_.nsecsElapsed() / 1000000.0;
        const qreal progress = qBound<qreal>(0.0, elapsedMs / durationMs_, 1.0);
        const qreal eased = QEasingCurve(QEasingCurve::OutCubic).valueForProgress(progress);
        const int value = qRound(startValue_ + (targetValue_ - startValue_) * eased);
        view_->verticalScrollBar()->setValue(value);
        if (progress >= 1.0) {
            view_->verticalScrollBar()->setValue(targetValue_);
            return;
        }
        scheduleNextFrame();
    }

    QAbstractItemView *view_ = nullptr;
    int linePixels_ = 0;
    int targetValue_ = 0;
    int startValue_ = 0;
    int durationMs_ = 0;
    QElapsedTimer elapsed_;
    QTimer timer_;
    qreal framePeriodMs_ = 16.67;
    qreal nextFrameDeadlineMs_ = 0.0;
};

class WindowDragHandle final : public QWidget
{
public:
    explicit WindowDragHandle(QMainWindow *window, QWidget *parent = nullptr)
        : QWidget(parent), window_(window)
    {
        setObjectName(QStringLiteral("windowDragArea"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && window_->windowHandle()
            && window_->windowHandle()->startSystemMove()) {
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            window_->isMaximized() ? window_->showNormal() : window_->showMaximized();
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

private:
    QMainWindow *window_ = nullptr;
};

class FramelessResizeFilter final : public QObject
{
public:
    FramelessResizeFilter(QWidget *root, QMainWindow *window)
        : QObject(root), root_(root), window_(window)
    {
        root_->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != root_ || window_->isMaximized()) {
            return QObject::eventFilter(watched, event);
        }
        if (event->type() == QEvent::Leave) {
            root_->unsetCursor();
            return QObject::eventFilter(watched, event);
        }
        if (event->type() != QEvent::MouseMove && event->type() != QEvent::MouseButtonPress) {
            return QObject::eventFilter(watched, event);
        }

        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        const QPoint position = mouseEvent->position().toPoint();
        const int edgeWidth = 7;
        const bool left = position.x() < edgeWidth;
        const bool right = position.x() >= root_->width() - edgeWidth;
        const bool top = position.y() < edgeWidth;
        const bool bottom = position.y() >= root_->height() - edgeWidth;
        Qt::Edges edges;
        if (left) edges |= Qt::LeftEdge;
        if (right) edges |= Qt::RightEdge;
        if (top) edges |= Qt::TopEdge;
        if (bottom) edges |= Qt::BottomEdge;

        if (event->type() == QEvent::MouseMove) {
            if (edges == (Qt::TopEdge | Qt::LeftEdge)
                || edges == (Qt::BottomEdge | Qt::RightEdge)) {
                root_->setCursor(Qt::SizeFDiagCursor);
            } else if (edges == (Qt::TopEdge | Qt::RightEdge)
                       || edges == (Qt::BottomEdge | Qt::LeftEdge)) {
                root_->setCursor(Qt::SizeBDiagCursor);
            } else if (left || right) {
                root_->setCursor(Qt::SizeHorCursor);
            } else if (top || bottom) {
                root_->setCursor(Qt::SizeVerCursor);
            } else {
                root_->unsetCursor();
            }
            return QObject::eventFilter(watched, event);
        }

        if (mouseEvent->button() == Qt::LeftButton && edges != Qt::Edges{}
            && window_->windowHandle()
            && window_->windowHandle()->startSystemResize(edges)) {
            mouseEvent->accept();
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget *root_ = nullptr;
    QMainWindow *window_ = nullptr;
};

void configureTrackTable(QTableView *table)
{
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(false);
    table->setShowGrid(false);
    table->setWordWrap(false);
    table->setTextElideMode(Qt::ElideRight);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(54);
    table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
#ifndef Q_OS_MACOS
    new SmoothWheelScrollFilter(table, 54);
#endif
    table->horizontalHeader()->setHighlightSections(false);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->horizontalHeader()->setSectionsClickable(true);
    table->horizontalHeader()->setSortIndicatorShown(true);
    table->horizontalHeader()->setStretchLastSection(false);
    for (int column = 0; column < 6; ++column) {
        table->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Fixed);
    }
    table->horizontalHeader()->setIconSize(QSize(18, 18));
    const int numberColumnMinimumWidth = qMax(
        56, QFontMetrics(table->font()).horizontalAdvance(QStringLiteral("9999...")) + 18);
    table->setColumnWidth(trackNumberColumn, numberColumnMinimumWidth);
    table->setColumnWidth(trackAlbumColumn, 170);
    table->setColumnWidth(trackAddedAtColumn, 120);
    if (auto *responsiveTable = dynamic_cast<TrackTableView *>(table)) {
        responsiveTable->setMinimumNumberColumnWidth(numberColumnMinimumWidth);
        responsiveTable->setRightPaddingWidth(16);
        responsiveTable->setResponsiveColumnWeights({6, 45, 22, 18, 9});
    }
    table->setItemDelegate(new TrackRowDelegate(table));
    new DelayedTextTooltipBehavior(table);
    table->setIconSize(QSize(32, 32));
    table->setProperty("hoveredRow", -1);
    table->setMouseTracking(true);
    table->viewport()->setMouseTracking(true);
    new HoverScrollBarBehavior(table);
    table->setMinimumHeight(300);
}

class ClickToSetSlider final : public QSlider
{
public:
    using QSlider::QSlider;

    void setHoverHandler(std::function<void(bool, int, const QPoint &)> handler)
    {
        hoverHandler_ = std::move(handler);
    }

protected:
    bool event(QEvent *event) override
    {
        const bool handled = QSlider::event(event);
        if (event->type() == QEvent::Enter) {
            setHoveredAt(mapFromGlobal(QCursor::pos()));
        } else if (event->type() == QEvent::HoverEnter
                   || event->type() == QEvent::HoverMove) {
            setHoveredAt(static_cast<QHoverEvent *>(event)->position().toPoint());
        } else if (event->type() == QEvent::Leave) {
            hovered_ = false;
            update();
            notifyHover(false);
        } else if (event->type() == QEvent::HoverLeave) {
            hovered_ = false;
            update();
            notifyHover(false);
        }
        return handled;
    }

    void paintEvent(QPaintEvent *event) override
    {
        QSlider::paintEvent(event);
        if (orientation() != Qt::Horizontal || !isEnabled()) {
            return;
        }

        QStyleOptionSlider option;
        initStyleOption(&option);
        const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option,
                                                      QStyle::SC_SliderGroove, this);
        const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option,
                                                      QStyle::SC_SliderHandle, this);
        const int handleLength = handle.width();
        const int span = qMax(0, groove.width() - handleLength);
        const int minCenter = groove.left() + handleLength / 2;
        const int maxCenter = groove.right() - handleLength / 2;
        const int sliderOffset = QStyle::sliderPositionFromValue(
            minimum(), maximum(), sliderPosition(), span, option.upsideDown);
        const int currentCenter = minCenter + sliderOffset;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        const QRectF filledTrack(groove.left(), groove.center().y() - 2.0,
                                 qMax(0, currentCenter - groove.left()), 4.0);
        painter.setBrush(hovered_ ? QColor(QStringLiteral("#1ed760")) : Qt::white);
        painter.drawRoundedRect(filledTrack, 2.0, 2.0);

        if (!hovered_) {
            return;
        }

        const int hoverTarget = qBound(minCenter, hoverPosition_.x(), maxCenter);
        const int previewStart = currentCenter + handleLength / 2;
        if (hoverTarget > previewStart) {
            painter.setBrush(Qt::white);
            const qreal previewY = groove.center().y() - 1.0;
            painter.drawRoundedRect(QRectF(previewStart, previewY,
                                           hoverTarget - previewStart, 4.0),
                                    2.0, 2.0);
        }

        painter.setBrush(Qt::white);
        painter.drawEllipse(QPointF(currentCenter, groove.center().y()), 5.0, 5.0);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        setHoveredAt(event->position().toPoint());
        if (event->button() == Qt::LeftButton
            && !handleRect().contains(event->position().toPoint())) {
            trackClickActive_ = true;
            setSliderDown(true);
            setValueAt(event->position().toPoint());
            event->accept();
            return;
        }
        QSlider::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        setHoveredAt(event->position().toPoint());
        if (trackClickActive_ && (event->buttons() & Qt::LeftButton)) {
            setValueAt(event->position().toPoint());
            event->accept();
            return;
        }
        QSlider::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        setHoveredAt(event->position().toPoint());
        if (trackClickActive_ && event->button() == Qt::LeftButton) {
            setValueAt(event->position().toPoint());
            trackClickActive_ = false;
            setSliderDown(false);
            event->accept();
            return;
        }
        QSlider::mouseReleaseEvent(event);
    }

private:
    void setHoveredAt(const QPoint &position)
    {
        hoverPosition_ = position;
        hovered_ = true;
        update();
        notifyHover(true);
    }

    int valueAt(const QPoint &point) const
    {
        QStyleOptionSlider option;
        initStyleOption(&option);
        const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option,
                                                      QStyle::SC_SliderGroove, this);
        const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option,
                                                      QStyle::SC_SliderHandle, this);
        const bool horizontal = orientation() == Qt::Horizontal;
        const int handleLength = horizontal ? handle.width() : handle.height();
        const int grooveLength = horizontal ? groove.width() : groove.height();
        const int pointer = horizontal ? point.x() : point.y();
        const int grooveStart = horizontal ? groove.x() : groove.y();
        const int span = qMax(0, grooveLength - handleLength);
        const int position = pointer - grooveStart - handleLength / 2;
        return span > 0
            ? QStyle::sliderValueFromPosition(minimum(), maximum(), position, span,
                                               option.upsideDown)
            : minimum();
    }

    void notifyHover(bool visible)
    {
        if (hoverHandler_) {
            hoverHandler_(visible, visible ? valueAt(hoverPosition_) : 0, hoverPosition_);
        }
    }

    QRect handleRect() const
    {
        QStyleOptionSlider option;
        initStyleOption(&option);
        return style()->subControlRect(QStyle::CC_Slider, &option,
                                       QStyle::SC_SliderHandle, this);
    }

    void setValueAt(const QPoint &point)
    {
        QStyleOptionSlider option;
        initStyleOption(&option);
        const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option,
                                                      QStyle::SC_SliderGroove, this);
        const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option,
                                                      QStyle::SC_SliderHandle, this);
        const bool horizontal = orientation() == Qt::Horizontal;
        const int handleLength = horizontal ? handle.width() : handle.height();
        const int grooveLength = horizontal ? groove.width() : groove.height();
        const int pointer = horizontal ? point.x() : point.y();
        const int grooveStart = horizontal ? groove.x() : groove.y();
        const int span = qMax(0, grooveLength - handleLength);
        const int position = pointer - grooveStart - handleLength / 2;
        const int target = span > 0
            ? QStyle::sliderValueFromPosition(minimum(), maximum(), position, span,
                                               option.upsideDown)
            : minimum();
        setSliderPosition(target);
        setValue(target);
    }

    bool trackClickActive_ = false;
    bool hovered_ = false;
    QPoint hoverPosition_;
    std::function<void(bool, int, const QPoint &)> hoverHandler_;
};

class FocusWheelSpinBox final : public QSpinBox
{
public:
    using QSpinBox::QSpinBox;

    QLineEdit *editField() const { return lineEdit(); }

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        QWidget *focusedWidget = QApplication::focusWidget();
        if (!hasFocus() && (!focusedWidget
                            || (focusedWidget != this && !isAncestorOf(focusedWidget)))) {
            event->ignore();
            return;
        }
        QSpinBox::wheelEvent(event);
    }
};

QLabel *makeLabel(const QString &text, const QString &objectName = {})
{
    auto *label = new QLabel(text);
    label->setObjectName(objectName);
    label->setWordWrap(true);
    return label;
}

MarqueeLabel *makeMarqueeLabel(const QString &text, const QString &objectName = {})
{
    auto *label = new MarqueeLabel(text);
    label->setObjectName(objectName);
    return label;
}

QFrame *makeSurface(QWidget *parent = nullptr)
{
    auto *surface = new QFrame(parent);
    surface->setObjectName("surface");
    surface->setFrameShape(QFrame::NoFrame);
    return surface;
}

QWidget *makePage(const QString &heading, const QString &description, QWidget *parent = nullptr)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 18, 8, 8);
    layout->setSpacing(8);

    layout->addWidget(makeLabel(heading, "pageHeading"));
    layout->addWidget(makeLabel(description, "pageDescription"));
    layout->addSpacing(16);
    return page;
}

QToolButton *makeNavButton(const QString &text, QStyle::StandardPixmap icon, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setObjectName("navButton");
    button->setText(text);
    button->setIcon(parent->style()->standardIcon(icon));
    button->setIconSize(QSize(18, 18));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumHeight(42);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return button;
}

QToolButton *makeMediaButton(QStyle::StandardPixmap icon, const QString &description, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setIcon(parent->style()->standardIcon(icon));
    button->setIconSize(QSize(18, 18));
    button->setToolTip(description);
    button->setAccessibleName(description);
    button->setFixedSize(38, 38);
    button->setEnabled(false);
    return button;
}

QString formatPlaybackTime(int milliseconds)
{
    const int totalSeconds = qMax(0, milliseconds) / 1000;
    return QStringLiteral("%1:%2")
        .arg(totalSeconds / 60)
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
}

QString formatRelativeAddedAt(const QString &value)
{
    QDateTime addedAt = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!addedAt.isValid()) {
        addedAt = QDateTime::fromString(value, Qt::ISODate);
    }
    if (!addedAt.isValid()) {
        return {};
    }

    const qint64 ageSeconds = qMax<qint64>(0, addedAt.toUTC().secsTo(QDateTime::currentDateTimeUtc()));
    if (ageSeconds < 60) {
        return QStringLiteral("just now");
    }

    const auto withUnit = [](qint64 count, const QString &unit) {
        return QStringLiteral("%1 %2 ago").arg(count).arg(unit + (count == 1 ? QString{} : QStringLiteral("s")));
    };
    if (ageSeconds < 60 * 60) {
        return withUnit(ageSeconds / 60, QStringLiteral("minute"));
    }
    if (ageSeconds < 24 * 60 * 60) {
        return withUnit(ageSeconds / (60 * 60), QStringLiteral("hour"));
    }
    if (ageSeconds < 7 * 24 * 60 * 60) {
        return withUnit(ageSeconds / (24 * 60 * 60), QStringLiteral("day"));
    }
    if (ageSeconds <= 28 * 24 * 60 * 60) {
        return withUnit(ageSeconds / (7 * 24 * 60 * 60), QStringLiteral("week"));
    }
    return addedAt.toLocalTime().toString(QStringLiteral("MMM d, yyyy"));
}

} // namespace

class TrackTableModel final : public QAbstractTableModel
{
public:
    using ImageProvider = std::function<void(const QString &,
                                              std::function<void(const QImage &)>)>;

    explicit TrackTableModel(QObject *parent = nullptr, bool libraryHeaderRows = false)
        : QAbstractTableModel(parent), libraryHeaderRows_(libraryHeaderRows)
    {
        if (libraryHeaderRows_) {
            sortColumn_ = trackAddedAtColumn;
            sortOrder_ = Qt::DescendingOrder;
            sortPending_ = true;
        }
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : displayedTrackRowCount() + prefixRowCount();
    }

    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : 6;
    }

    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override
    {
        if (column < trackTitleColumn || column > trackDurationColumn) {
            return;
        }
        if (sortColumn_ == column && sortOrder_ == order && !sortPending_) {
            return;
        }
        sortColumn_ = column;
        sortOrder_ = order;
        const bool complete = !libraryHeaderRows_ || totalRows_ == tracks_.size();
        if (complete && !tracks_.isEmpty()) {
            sortPending_ = false;
            rebuildDisplayOrderPreservingPersistentIndexes();
        } else {
            sortPending_ = true;
        }
        updateSortHeader();
    }

    int sortColumn() const { return sortColumn_; }
    Qt::SortOrder sortOrder() const { return sortOrder_; }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if (!index.isValid()) {
            return Qt::NoItemFlags;
        }
        if (libraryHeaderRows_ && index.row() < prefixRowCount()) {
            return Qt::ItemIsEnabled;
        }
        if (libraryHeaderRows_
            && index.row() >= prefixRowCount() + totalRows_) {
            return Qt::NoItemFlags;
        }
        return QAbstractTableModel::flags(index);
    }

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override
    {
        if (libraryHeaderRows_) {
            return {};
        }
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            static const QStringList headers{
                QStringLiteral("#"), QStringLiteral("Title"), QStringLiteral("Album"),
                QStringLiteral("Date added"), QString{}, QString{}};
            return section >= 0 && section < headers.size() ? headers.at(section) : QVariant{};
        }
        if (orientation == Qt::Horizontal && role == Qt::DecorationRole
            && section == trackDurationColumn) {
            return durationHeaderIcon();
        }
        if (orientation == Qt::Horizontal && role == Qt::ToolTipRole
            && section == trackDurationColumn) {
            return QStringLiteral("Track duration");
        }
        if (orientation == Qt::Horizontal && role == Qt::TextAlignmentRole
            && (section == trackNumberColumn || section == trackDurationColumn)) {
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }
        return {};
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
            return {};
        }

        if (libraryHeaderRows_ && index.row() == 0) {
            if (role == libraryHeroTitleRole) return libraryTitle_;
            if (role == libraryHeroCreatorRole) return libraryCreator_;
            if (role == libraryHeroSummaryRole) return librarySummary_;
            if (role == libraryHeroRowRole) return true;
            if (role == Qt::DisplayRole) return libraryTitle_;
            return {};
        }
        if (libraryHeaderRows_ && index.row() == 1) {
            if (role == libraryColumnHeaderRowRole) return true;
            if (role == Qt::DisplayRole) {
                static const QStringList headers{
                    QStringLiteral("#"), QStringLiteral("Title"), QStringLiteral("Album"),
                    QStringLiteral("Date added"), QString{}, QString{}};
                if (index.column() < 0 || index.column() >= headers.size()) return {};
                if (index.column() == trackDurationColumn && sortColumn_ == trackDurationColumn) {
                    return sortOrder_ == Qt::AscendingOrder ? QStringLiteral("▲")
                                                            : QStringLiteral("▼");
                }
                QString header = headers.at(index.column());
                if (index.column() == sortColumn_ && !header.isEmpty()) {
                    header += sortOrder_ == Qt::AscendingOrder
                        ? QStringLiteral("  ▲") : QStringLiteral("  ▼");
                }
                return header;
            }
            if (role == Qt::TextAlignmentRole
                && (index.column() == trackNumberColumn || index.column() == trackDurationColumn)) {
                return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
            }
            return {};
        }

        const int displayTrackRow = index.row() - prefixRowCount();
        if (displayTrackRow < 0 || displayTrackRow >= totalRows_) {
            return {};
        }
        const int trackRow = sourceTrackRowForDisplayRow(displayTrackRow);
        if (trackRow < 0) {
            return {};
        }
        if (role == Qt::DisplayRole && index.column() == trackNumberColumn) {
            const QString number = QString::number(displayTrackRow + 1);
            return number.size() <= 4 ? number : number.left(4) + QStringLiteral("...");
        }
        if (role == Qt::TextAlignmentRole
            && (index.column() == trackNumberColumn || index.column() == trackDurationColumn)) {
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }

        const auto found = tracks_.constFind(trackRow);
        if (found == tracks_.cend()) {
            if (index.column() == trackTitleColumn && role == Qt::DisplayRole) {
                return QStringLiteral("Loading…");
            }
            if (index.column() == trackTitleColumn && role == Qt::ForegroundRole) {
                return QColor(QStringLiteral("#6a6a6a"));
            }
            return {};
        }

        const SpotifyTrackResult &track = found.value();
        if (role == Qt::DecorationRole && index.column() == trackTitleColumn) {
            auto icon = artworkIcons_.constFind(track.albumImageUrl);
            if (icon != artworkIcons_.cend()) {
                return icon.value();
            }
            if (!track.albumImageUrl.isEmpty() && imageProvider_
                && !pendingArtworkUrls_.contains(track.albumImageUrl)) {
                pendingArtworkUrls_.insert(track.albumImageUrl);
                QPointer<TrackTableModel> guard(const_cast<TrackTableModel *>(this));
                imageProvider_(track.albumImageUrl, [guard, url = track.albumImageUrl](const QImage &image) {
                    if (!guard) {
                        return;
                    }
                    guard->pendingArtworkUrls_.remove(url);
                    if (image.isNull()) {
                        return;
                    }
                    constexpr qsizetype maximumRowArtworkIcons = 1024;
                    if (!guard->artworkIcons_.contains(url)) {
                        while (guard->artworkIcons_.size() >= maximumRowArtworkIcons
                               && !guard->artworkIconOrder_.isEmpty()) {
                            guard->artworkIcons_.remove(guard->artworkIconOrder_.takeFirst());
                        }
                        guard->artworkIconOrder_.append(url);
                    }
                    const QPixmap pixmap = QPixmap::fromImage(image).scaled(
                        QSize(32, 32), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    guard->artworkIcons_.insert(url, QIcon(pixmap));
                    const QSet<int> sourceRows = guard->artworkSourceRows_.value(url);
                    for (const int sourceRow : sourceRows) {
                        const int displayRow = guard->displayRowForSourceTrack(sourceRow);
                        if (displayRow < 0) continue;
                        const int modelRow = displayRow + guard->prefixRowCount();
                        emit guard->dataChanged(guard->index(modelRow, trackTitleColumn),
                                                guard->index(modelRow, trackTitleColumn),
                                                {Qt::DecorationRole});
                    }
                });
            }
            return {};
        }
        if (role == trackArtistRole && index.column() == trackTitleColumn) {
            return track.artists;
        }
        if (role == trackArtistIdRole && index.column() == trackTitleColumn) {
            return track.artistId;
        }
        if (role == trackAlbumIdRole && index.column() == trackAlbumColumn) {
            return track.albumId;
        }
        if (role == trackAlbumIdRole && index.column() == trackTitleColumn) {
            return track.albumId;
        }
        if (role == trackAlbumNameRole && index.column() == trackTitleColumn) {
            return track.album;
        }
        if (role == trackPlayingRole) {
            return matchesPlaybackTrack(track);
        }
        if (role == trackPausedRole) {
            return playbackPaused_;
        }
        if (index.column() == trackTitleColumn && role == Qt::UserRole) {
            return track.spotifyUrl;
        }
        if (index.column() == trackTitleColumn && role == Qt::UserRole + 1) {
            return track.spotifyUri;
        }
        if (role == Qt::ToolTipRole) {
            switch (index.column()) {
            case trackTitleColumn: return track.name;
            case trackAlbumColumn: return track.album;
            default: break;
            }
        }
        if (role == Qt::ForegroundRole && index.column() != trackTitleColumn) {
            return QColor(QStringLiteral("#b3b3b3"));
        }
        if (role != Qt::DisplayRole) {
            return {};
        }

        switch (index.column()) {
        case trackTitleColumn:
            return track.name;
        case trackAlbumColumn:
            return track.album;
        case trackAddedAtColumn: {
            return formatRelativeAddedAt(track.addedAt);
        }
        case trackDurationColumn: {
            const int seconds = track.durationMs / 1000;
            return QStringLiteral("%1:%2")
                .arg(seconds / 60)
                .arg(seconds % 60, 2, 10, QLatin1Char('0'));
        }
        default:
            return {};
        }
    }

    void clearRows()
    {
        beginResetModel();
        totalRows_ = 0;
        tracks_.clear();
        sortKeys_.clear();
        artworkSourceRows_.clear();
        displayOrder_.clear();
        displayRowsBySource_.clear();
        sortPending_ = libraryHeaderRows_ && sortColumn_ >= 0;
        playbackUri_.clear();
        playbackActive_ = false;
        playbackPaused_ = true;
        playbackUriFound_ = false;
        playbackName_.clear();
        playbackArtists_.clear();
        playbackDurationMs_ = 0;
        endResetModel();
    }

    void setImageProvider(ImageProvider provider)
    {
        imageProvider_ = std::move(provider);
    }

    void clearArtworkCache()
    {
        artworkIcons_.clear();
        artworkIconOrder_.clear();
        pendingArtworkUrls_.clear();
        if (totalRows_ > 0) {
            const int firstTrackRow = prefixRowCount();
            emit dataChanged(index(firstTrackRow, trackTitleColumn),
                             index(firstTrackRow + totalRows_ - 1, trackTitleColumn),
                             {Qt::DecorationRole});
        }
    }

    void setLibraryHeader(const QString &title, const QString &creator,
                          const QString &summary)
    {
        if (!libraryHeaderRows_ || (libraryTitle_ == title && libraryCreator_ == creator
                                    && librarySummary_ == summary)) {
            return;
        }
        libraryTitle_ = title;
        libraryCreator_ = creator;
        librarySummary_ = summary;
        emit dataChanged(index(0, 0), index(0, 0),
                         {Qt::DisplayRole, libraryHeroTitleRole, libraryHeroCreatorRole,
                          libraryHeroSummaryRole});
    }

    void setTotalRows(int count)
    {
        count = qMax(0, count);
        if (count == totalRows_) {
            return;
        }
        const int prefix = prefixRowCount();
        const int oldDisplayedCount = displayedTrackRowCount();
        const int newDisplayedCount = displayedTrackRowCount(count);
        if (newDisplayedCount > oldDisplayedCount) {
            beginInsertRows({}, oldDisplayedCount + prefix,
                            newDisplayedCount + prefix - 1);
            totalRows_ = count;
            endInsertRows();
        } else if (newDisplayedCount < oldDisplayedCount) {
            beginRemoveRows({}, newDisplayedCount + prefix,
                            oldDisplayedCount + prefix - 1);
            totalRows_ = count;
            removeTracksAtOrAfter(count);
            endRemoveRows();
        } else {
            totalRows_ = count;
            removeTracksAtOrAfter(count);
        }
        for (auto it = sortKeys_.begin(); it != sortKeys_.end();) {
            if (it.key() >= count) {
                it = sortKeys_.erase(it);
            } else {
                ++it;
            }
        }
        displayOrder_.clear();
        displayRowsBySource_.clear();
        updatePlaybackUriPresence();
        sortPending_ = libraryHeaderRows_ && sortColumn_ >= 0
            && totalRows_ > 0 && tracks_.size() < totalRows_;
        if (newDisplayedCount > 0) {
            emit dataChanged(index(prefix, 0),
                             index(prefix + newDisplayedCount - 1, columnCount() - 1));
        }
    }

    void setTracks(int offset, const QList<SpotifyTrackResult> &tracks)
    {
        if (offset < 0 || offset >= totalRows_ || tracks.isEmpty()) {
            return;
        }
        const int lastRow = qMin(totalRows_ - 1, offset + tracks.size() - 1);
        for (int index = 0; index <= lastRow - offset; ++index) {
            const int sourceRow = offset + index;
            const SpotifyTrackResult &track = tracks.at(index);
            const auto previousTrack = tracks_.constFind(sourceRow);
            if (previousTrack != tracks_.cend()) {
                removeArtworkSourceRow(sourceRow, previousTrack->albumImageUrl);
            }
            tracks_.insert(sourceRow, track);
            sortKeys_.insert(sourceRow, makeSortKey(track));
            addArtworkSourceRow(sourceRow, track.albumImageUrl);
        }
        updatePlaybackUriPresence();
        const int prefix = prefixRowCount();
        const bool finishSort = sortPending_ && sortColumn_ >= 0 && totalRows_ > 0
            && tracks_.size() >= totalRows_;
        if (finishSort) {
            sortPending_ = false;
            rebuildDisplayOrderPreservingPersistentIndexes();
            emit dataChanged(this->index(prefix, 0),
                             this->index(prefix + displayedTrackRowCount() - 1,
                                         columnCount() - 1));
        } else if (!displayOrder_.isEmpty()) {
            emit dataChanged(this->index(prefix, 0),
                             this->index(prefix + displayOrder_.size() - 1,
                                         columnCount() - 1));
        } else {
            emit dataChanged(this->index(offset + prefix, 0),
                             this->index(lastRow + prefix, columnCount() - 1));
        }
    }

    void setSearchTracks(const QList<SpotifyTrackResult> &tracks)
    {
        beginResetModel();
        totalRows_ = tracks.size();
        tracks_.clear();
        sortKeys_.clear();
        artworkSourceRows_.clear();
        displayOrder_.clear();
        displayRowsBySource_.clear();
        for (int index = 0; index < tracks.size(); ++index) {
            tracks_.insert(index, tracks.at(index));
            sortKeys_.insert(index, makeSortKey(tracks.at(index)));
            addArtworkSourceRow(index, tracks.at(index).albumImageUrl);
        }
        updatePlaybackUriPresence();
        if (sortColumn_ >= 0 && !tracks_.isEmpty()) {
            rebuildDisplayOrder();
        }
        sortPending_ = false;
        endResetModel();
    }

    void setPlaybackState(const SpotifyPlaybackState &state)
    {
        const QString previousUri = playbackUri_;
        const QString previousName = playbackName_;
        const QString previousArtists = playbackArtists_;
        const int previousDurationMs = playbackDurationMs_;
        const bool previousActive = playbackActive_;
        const bool previousPaused = playbackPaused_;
        playbackUri_ = state.trackUri;
        playbackName_ = state.trackName;
        playbackArtists_ = state.artists;
        playbackDurationMs_ = state.durationMs;
        playbackActive_ = state.active;
        playbackPaused_ = state.paused;
        updatePlaybackUriPresence();
        if (previousUri == playbackUri_ && previousActive == playbackActive_
            && previousPaused == playbackPaused_ && previousName == playbackName_
            && previousArtists == playbackArtists_
            && previousDurationMs == playbackDurationMs_) {
            return;
        }
        notifyTrack(previousUri);
        notifyPlaybackTrack();
    }

    const SpotifyTrackResult *trackAt(int row) const
    {
        const int trackRow = sourceTrackRowForDisplayRow(row - prefixRowCount());
        if (trackRow < 0) return nullptr;
        const auto found = tracks_.constFind(trackRow);
        return found == tracks_.cend() ? nullptr : &found.value();
    }

    int rowForUri(const QString &uri, int preferredDisplayRow = -1) const
    {
        if (uri.isEmpty()) {
            return -1;
        }
        int bestDisplayRow = -1;
        int bestDistance = std::numeric_limits<int>::max();
        for (auto it = tracks_.cbegin(); it != tracks_.cend(); ++it) {
            if (it.value().spotifyUri == uri) {
                const int displayRow = displayRowForSourceTrack(it.key());
                if (displayRow < 0) {
                    continue;
                }
                const int modelRow = displayRow + prefixRowCount();
                const int distance = preferredDisplayRow >= 0
                    ? qAbs(modelRow - preferredDisplayRow) : modelRow;
                if (distance < bestDistance
                    || (distance == bestDistance
                        && (bestDisplayRow < 0 || modelRow < bestDisplayRow))) {
                    bestDisplayRow = modelRow;
                    bestDistance = distance;
                }
            }
        }
        return bestDisplayRow;
    }

    int rowForPlayback(const SpotifyPlaybackState &state,
                       int preferredDisplayRow = -1) const
    {
        const int exactUriRow = rowForUri(state.trackUri, preferredDisplayRow);
        if (exactUriRow >= 0) {
            return exactUriRow;
        }

        int bestDisplayRow = -1;
        int bestDistance = std::numeric_limits<int>::max();
        int bestDurationDifference = std::numeric_limits<int>::max();
        for (auto it = tracks_.cbegin(); it != tracks_.cend(); ++it) {
            if (!matchesPlaybackTrack(it.value(), state, false)) {
                continue;
            }
            const int displayRow = displayRowForSourceTrack(it.key());
            if (displayRow < 0) {
                continue;
            }
            const int modelRow = displayRow + prefixRowCount();
            const int distance = preferredDisplayRow >= 0
                ? qAbs(modelRow - preferredDisplayRow) : modelRow;
            const int durationDifference = state.durationMs > 0 && it->durationMs > 0
                ? qAbs(state.durationMs - it->durationMs) : 0;
            if (durationDifference < bestDurationDifference
                || (durationDifference == bestDurationDifference
                    && (distance < bestDistance
                        || (distance == bestDistance
                            && (bestDisplayRow < 0 || modelRow < bestDisplayRow))))) {
                bestDisplayRow = modelRow;
                bestDistance = distance;
                bestDurationDifference = durationDifference;
            }
        }
        return bestDisplayRow;
    }

    bool sortPending() const { return sortPending_; }

    int loadedTrackCount() const { return tracks_.size(); }
    int firstTrackRow() const { return prefixRowCount(); }
    int totalTrackCount() const { return totalRows_; }

private:
    static QString normalizedTrackIdentity(const QString &value)
    {
        return value.simplified().toCaseFolded();
    }

    static bool matchesPlaybackTrack(const SpotifyTrackResult &track,
                                     const SpotifyPlaybackState &state,
                                     bool allowUriMatch = true)
    {
        if (!state.active) {
            return false;
        }
        if (allowUriMatch && !state.trackUri.isEmpty()
            && track.spotifyUri == state.trackUri) {
            return true;
        }
        if (normalizedTrackIdentity(track.name).isEmpty()
            || normalizedTrackIdentity(track.artists).isEmpty()
            || normalizedTrackIdentity(track.name)
                != normalizedTrackIdentity(state.trackName)
            || normalizedTrackIdentity(track.artists)
                != normalizedTrackIdentity(state.artists)) {
            return false;
        }
        return state.durationMs <= 0 || track.durationMs <= 0
            || qAbs(state.durationMs - track.durationMs) <= 2500;
    }

    bool matchesPlaybackTrack(const SpotifyTrackResult &track) const
    {
        if (!playbackActive_) {
            return false;
        }
        if (!playbackUri_.isEmpty() && playbackUriFound_) {
            return track.spotifyUri == playbackUri_;
        }
        SpotifyPlaybackState state;
        state.active = playbackActive_;
        state.trackUri = playbackUri_;
        state.trackName = playbackName_;
        state.artists = playbackArtists_;
        state.durationMs = playbackDurationMs_;
        return matchesPlaybackTrack(track, state);
    }

    void updatePlaybackUriPresence()
    {
        playbackUriFound_ = false;
        if (!playbackUri_.isEmpty()) {
            for (auto it = tracks_.cbegin(); it != tracks_.cend(); ++it) {
                if (it->spotifyUri == playbackUri_) {
                    playbackUriFound_ = true;
                    break;
                }
            }
        }
    }

    struct TrackSortKey {
        QString title;
        QString album;
        qint64 addedAtMs = -1;
        int durationMs = 0;
    };

    struct SortEntry {
        int sourceRow = -1;
        TrackSortKey key;
    };

    int prefixRowCount() const { return libraryHeaderRows_ ? 2 : 0; }
    void addArtworkSourceRow(int sourceRow, const QString &url)
    {
        if (!url.isEmpty()) {
            artworkSourceRows_[url].insert(sourceRow);
        }
    }

    void removeArtworkSourceRow(int sourceRow, const QString &url)
    {
        auto rows = artworkSourceRows_.find(url);
        if (rows != artworkSourceRows_.end()) {
            rows->remove(sourceRow);
            if (rows->isEmpty()) {
                artworkSourceRows_.erase(rows);
            }
        }
    }

    void removeTracksAtOrAfter(int firstRemovedSourceRow)
    {
        for (auto it = tracks_.begin(); it != tracks_.end();) {
            if (it.key() >= firstRemovedSourceRow) {
                removeArtworkSourceRow(it.key(), it->albumImageUrl);
                it = tracks_.erase(it);
            } else {
                ++it;
            }
        }
    }

    int displayedTrackRowCount(int actualCount) const
    {
        return libraryHeaderRows_ ? qMax(6, actualCount) : actualCount;
    }
    int displayedTrackRowCount() const { return displayedTrackRowCount(totalRows_); }

    static TrackSortKey makeSortKey(const SpotifyTrackResult &track)
    {
        TrackSortKey key;
        key.title = track.name.toCaseFolded();
        key.album = track.album.toCaseFolded();
        QDateTime addedAt = QDateTime::fromString(track.addedAt, Qt::ISODateWithMs);
        if (!addedAt.isValid()) addedAt = QDateTime::fromString(track.addedAt, Qt::ISODate);
        key.addedAtMs = addedAt.isValid() ? addedAt.toMSecsSinceEpoch() : -1;
        key.durationMs = track.durationMs;
        return key;
    }

    int sourceTrackRowForDisplayRow(int displayRow) const
    {
        if (displayRow < 0 || displayRow >= totalRows_) return -1;
        if (displayOrder_.isEmpty()) return displayRow;
        return displayRow < displayOrder_.size() ? displayOrder_.at(displayRow) : -1;
    }

    int displayRowForSourceTrack(int sourceRow) const
    {
        if (sourceRow < 0 || sourceRow >= totalRows_) return -1;
        return displayOrder_.isEmpty() ? sourceRow
            : sourceRow < displayRowsBySource_.size() ? displayRowsBySource_.at(sourceRow) : -1;
    }

    void rebuildDisplayOrder()
    {
        displayOrder_.clear();
        displayRowsBySource_.clear();
        if (sortColumn_ < trackTitleColumn || sortColumn_ > trackDurationColumn
            || tracks_.isEmpty()) {
            return;
        }

        QVector<SortEntry> entries;
        entries.reserve(tracks_.size());
        for (auto it = tracks_.cbegin(); it != tracks_.cend(); ++it) {
            entries.append({it.key(), sortKeys_.value(it.key())});
        }
        const bool ascending = sortOrder_ == Qt::AscendingOrder;
        std::stable_sort(entries.begin(), entries.end(), [this, ascending](
                             const SortEntry &left, const SortEntry &right) {
            bool leftBefore = false;
            bool rightBefore = false;
            switch (sortColumn_) {
            case trackTitleColumn:
                leftBefore = left.key.title < right.key.title;
                rightBefore = right.key.title < left.key.title;
                break;
            case trackAlbumColumn:
                leftBefore = left.key.album < right.key.album;
                rightBefore = right.key.album < left.key.album;
                break;
            case trackAddedAtColumn:
                if (left.key.addedAtMs < 0 || right.key.addedAtMs < 0) {
                    if ((left.key.addedAtMs < 0) != (right.key.addedAtMs < 0)) {
                        return left.key.addedAtMs >= 0;
                    }
                } else {
                    leftBefore = left.key.addedAtMs < right.key.addedAtMs;
                    rightBefore = right.key.addedAtMs < left.key.addedAtMs;
                }
                break;
            case trackDurationColumn:
                leftBefore = left.key.durationMs < right.key.durationMs;
                rightBefore = right.key.durationMs < left.key.durationMs;
                break;
            default:
                break;
            }
            if (leftBefore != rightBefore) return ascending ? leftBefore : rightBefore;
            return left.sourceRow < right.sourceRow;
        });
        displayOrder_.reserve(entries.size());
        displayRowsBySource_.fill(-1, totalRows_);
        for (const SortEntry &entry : entries) {
            displayRowsBySource_[entry.sourceRow] = displayOrder_.size();
            displayOrder_.append(entry.sourceRow);
        }
    }

    void rebuildDisplayOrderPreservingPersistentIndexes()
    {
        const QModelIndexList oldIndexes = persistentIndexList();
        QVector<int> oldSourceRows;
        oldSourceRows.reserve(oldIndexes.size());
        for (const QModelIndex &oldIndex : oldIndexes) {
            const int displayRow = oldIndex.row() - prefixRowCount();
            oldSourceRows.append(sourceTrackRowForDisplayRow(displayRow));
        }

        emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
        rebuildDisplayOrder();
        QModelIndexList newIndexes;
        newIndexes.reserve(oldIndexes.size());
        for (int index = 0; index < oldIndexes.size(); ++index) {
            int row = oldIndexes.at(index).row();
            if (oldSourceRows.at(index) >= 0) {
                const int displayRow = displayRowForSourceTrack(oldSourceRows.at(index));
                if (displayRow >= 0) row = prefixRowCount() + displayRow;
            }
            newIndexes.append(this->index(row, oldIndexes.at(index).column()));
        }
        changePersistentIndexList(oldIndexes, newIndexes);
        emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
    }

    void updateSortHeader()
    {
        if (libraryHeaderRows_ && rowCount() > 1) {
            emit dataChanged(index(1, 0), index(1, columnCount() - 1), {Qt::DisplayRole});
        }
    }

    void notifyTrack(const QString &uri)
    {
        const int row = rowForUri(uri);
        if (row >= 0) {
            emit dataChanged(index(row, 0), index(row, columnCount() - 1),
                             {trackPlayingRole, trackPausedRole});
        }
    }

    void notifyPlaybackTrack()
    {
        const int row = rowForPlayback(SpotifyPlaybackState{
            .trackName = playbackName_,
            .trackUri = playbackUri_,
            .artists = playbackArtists_,
            .durationMs = playbackDurationMs_,
            .paused = playbackPaused_,
            .active = playbackActive_});
        if (row >= 0) {
            emit dataChanged(index(row, 0), index(row, columnCount() - 1),
                             {trackPlayingRole, trackPausedRole});
        }
    }

    int totalRows_ = 0;
    bool libraryHeaderRows_ = false;
    QString libraryTitle_ = QStringLiteral("Liked Songs");
    QString libraryCreator_ = QStringLiteral("Your saved tracks");
    QString librarySummary_ = QStringLiteral("Connect Spotify to load your music.");
    QString playbackName_;
    QString playbackArtists_;
    QHash<int, SpotifyTrackResult> tracks_;
    QHash<int, TrackSortKey> sortKeys_;
    QHash<QString, QSet<int>> artworkSourceRows_;
    QVector<int> displayOrder_;
    QVector<int> displayRowsBySource_;
    int sortColumn_ = -1;
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
    bool sortPending_ = false;
    bool playbackUriFound_ = false;
    QString playbackUri_;
    int playbackDurationMs_ = 0;
    bool playbackActive_ = false;
    bool playbackPaused_ = true;
    ImageProvider imageProvider_;
    mutable QHash<QString, QIcon> artworkIcons_;
    mutable QList<QString> artworkIconOrder_;
    mutable QSet<QString> pendingArtworkUrls_;
};

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("MusicPlayer");
    setWindowFlag(Qt::FramelessWindowHint, true);
    setMinimumSize(900, 620);
    resize(1180, 760);

    auto *root = new QWidget(this);
    root->setObjectName("root");
    auto *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *body = new QWidget(root);
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    auto *bodySplitter = new QSplitter(Qt::Horizontal, body);
    bodySplitter->setObjectName(QStringLiteral("mainSplitter"));
    bodySplitter->setHandleWidth(7);
    bodySplitter->setChildrenCollapsible(false);
    bodySplitter_ = bodySplitter;

    auto *sidebar = new QFrame(body);
    sidebar->setObjectName("sidebar");
    sidebar->setFrameShape(QFrame::NoFrame);
    sidebar->setMinimumWidth(190);
    sidebar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    bodySplitter->addWidget(sidebar);

    auto *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(14, 16, 14, 14);
    sidebarLayout->setSpacing(5);

    auto *libraryTitle = makeLabel("Your Library", "librarySidebarHeading");
    sidebarLayout->addWidget(libraryTitle);

    playlistFilterEdit_ = new QLineEdit(sidebar);
    playlistFilterEdit_->setObjectName("playlistFilter");
    playlistFilterEdit_->setPlaceholderText("Search in your library");
    playlistFilterEdit_->setClearButtonEnabled(true);
    playlistFilterEdit_->setEnabled(false);
    sidebarLayout->addWidget(playlistFilterEdit_);

    playlistStatusLabel_ = makeLabel("Connect Spotify to load your library.", "mutedText");
    playlistStatusLabel_->setWordWrap(true);
    playlistStatusLabel_->setVisible(false);
    playlistsList_ = new QListWidget(sidebar);
    playlistsList_->setObjectName("playlistList");
    playlistsList_->setFrameShape(QFrame::NoFrame);
    playlistsList_->setSelectionMode(QAbstractItemView::SingleSelection);
    playlistsList_->setIconSize(QSize(42, 42));
    playlistsList_->setWordWrap(false);
    playlistsList_->setTextElideMode(Qt::ElideRight);
    playlistsList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    playlistsList_->setUniformItemSizes(true);
    playlistsList_->setItemDelegate(new LibrarySidebarItemDelegate(playlistsList_));
    new DelayedTextTooltipBehavior(playlistsList_);
    new HoverScrollBarBehavior(playlistsList_);
    new SmoothWheelScrollFilter(playlistsList_, 54);
    sidebarLayout->addWidget(playlistsList_, 1);
    sidebarLayout->addWidget(playlistStatusLabel_);
    addLikedSongsSidebarItem();

    auto *titleBar = new QWidget(root);
    titleBar->setObjectName("titleBar");
    titleBar->setFixedHeight(46);
    auto *titleBarLayout = new QHBoxLayout(titleBar);
    titleBarLayout->setContentsMargins(0, 0, 0, 0);
    titleBarLayout->setSpacing(0);

    auto *leftControls = new QWidget(titleBar);
    auto *leftControlsLayout = new QHBoxLayout(leftControls);
    leftControlsLayout->setContentsMargins(0, 0, 0, 0);
    leftControlsLayout->setSpacing(0);

    settingsButton_ = new QToolButton(leftControls);
    settingsButton_->setObjectName("titleBarButton");
    settingsButton_->setText(QStringLiteral("⋯"));
    settingsButton_->setToolTip("Settings");
    settingsButton_->setFixedSize(42, 46);
    leftControlsLayout->addWidget(settingsButton_);

    backButton_ = new QToolButton(leftControls);
    backButton_->setObjectName("titleBarButton");
    backButton_->setText(QStringLiteral("‹"));
    backButton_->setFixedSize(42, 46);
    backButton_->setEnabled(false);
    leftControlsLayout->addWidget(backButton_);

    forwardButton_ = new QToolButton(leftControls);
    forwardButton_->setObjectName("titleBarButton");
    forwardButton_->setText(QStringLiteral("›"));
    forwardButton_->setFixedSize(42, 46);
    forwardButton_->setEnabled(false);
    leftControlsLayout->addWidget(forwardButton_);
    leftControlsLayout->addWidget(new WindowDragHandle(this, leftControls), 1);
    leftControls->setFixedWidth(216 + bodySplitter->handleWidth());

    auto *mainHeader = new QWidget(titleBar);
    auto *mainHeaderLayout = new QGridLayout(mainHeader);
    mainHeaderLayout->setContentsMargins(0, 0, 0, 0);
    mainHeaderLayout->setHorizontalSpacing(0);
    mainHeaderLayout->setVerticalSpacing(0);
    mainHeaderLayout->addWidget(new WindowDragHandle(this, mainHeader), 0, 0);

    auto *centerControls = new QWidget(mainHeader);
    auto *centerControlsLayout = new QHBoxLayout(centerControls);
    centerControlsLayout->setContentsMargins(0, 0, 0, 0);
    centerControlsLayout->setSpacing(8);

    homeButton_ = new QToolButton(centerControls);
    homeButton_->setObjectName("homeButton");
    homeButton_->setIcon(homeIcon());
    homeButton_->setIconSize(QSize(21, 21));
    homeButton_->setFixedSize(36, 36);
    centerControlsLayout->addWidget(homeButton_);

    auto *searchBar = new QFrame(centerControls);
    searchBar->setObjectName("topSearchBar");
    searchBar->setFixedHeight(36);
    searchBar->setMinimumWidth(220);
    searchBar->setMaximumWidth(440);
    auto *searchBarLayout = new QHBoxLayout(searchBar);
    searchBarLayout->setContentsMargins(10, 2, 4, 2);
    searchBarLayout->setSpacing(4);
    searchEdit_ = new QLineEdit(searchBar);
    searchEdit_->setObjectName("globalSearchEdit");
    searchEdit_->setPlaceholderText("What do you want to play?");
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    searchEdit_->setEnabled(false);
    searchBarLayout->addWidget(searchEdit_, 1);
    searchButton_ = new QPushButton(searchBar);
    searchButton_->setObjectName("searchActionButton");
    searchButton_->setIcon(searchIcon());
    searchButton_->setIconSize(QSize(17, 17));
    searchButton_->setFixedSize(30, 30);
    searchButton_->setEnabled(false);
    searchBarLayout->addWidget(searchButton_);
    centerControlsLayout->addWidget(searchBar);

    mainHeaderLayout->addWidget(centerControls, 0, 1, Qt::AlignCenter);

    auto *rightHeaderArea = new QWidget(mainHeader);
    auto *rightHeaderLayout = new QHBoxLayout(rightHeaderArea);
    rightHeaderLayout->setContentsMargins(0, 0, 8, 0);
    rightHeaderLayout->setSpacing(0);
    rightHeaderLayout->addWidget(new WindowDragHandle(this, rightHeaderArea), 1);

    auto *accountControls = new QWidget(rightHeaderArea);
    auto *accountControlsLayout = new QHBoxLayout(accountControls);
    accountControlsLayout->setContentsMargins(0, 0, 0, 0);
    accountControlsLayout->setSpacing(8);
    spotifyHeaderAvatarLabel_ = makeLabel(QStringLiteral("♪"), "accountAvatar");
    spotifyHeaderAvatarLabel_->setObjectName(QStringLiteral("accountAvatar"));
    spotifyHeaderAvatarLabel_->setAlignment(Qt::AlignCenter);
    spotifyHeaderAvatarLabel_->setFixedSize(28, 28);
    spotifyHeaderAvatarLabel_->setToolTip(QStringLiteral("Spotify account"));
    accountControlsLayout->addWidget(spotifyHeaderAvatarLabel_);
    spotifyStatusLabel_ = makeLabel("Spotify not connected", "mutedText");
    spotifyStatusLabel_->setMaximumWidth(150);
    accountControlsLayout->addWidget(spotifyStatusLabel_);
    rightHeaderLayout->addWidget(accountControls);
    mainHeaderLayout->addWidget(rightHeaderArea, 0, 2);
    mainHeaderLayout->setColumnStretch(0, 1);
    mainHeaderLayout->setColumnStretch(2, 1);

    auto *minimizeButton = new QToolButton(titleBar);
    minimizeButton->setObjectName("windowControlButton");
    minimizeButton->setText(QStringLiteral("−"));
    minimizeButton->setFixedSize(35, 46);

    maximizeButton_ = new QToolButton(titleBar);
    maximizeButton_->setObjectName("windowControlButton");
    maximizeButton_->setText(QStringLiteral("□"));
    maximizeButton_->setFixedSize(35, 46);

    auto *closeButton = new QToolButton(titleBar);
    closeButton->setObjectName("windowCloseButton");
    closeButton->setText(QStringLiteral("×"));
    closeButton->setFixedSize(42, 46);
    auto *windowButtons = new QWidget(titleBar);
    windowButtons->setObjectName(QStringLiteral("windowButtons"));
    auto *windowButtonsLayout = new QHBoxLayout(windowButtons);
    windowButtonsLayout->setContentsMargins(0, 0, 0, 0);
    windowButtonsLayout->setSpacing(0);
    windowButtonsLayout->addWidget(minimizeButton);
    windowButtonsLayout->addWidget(maximizeButton_);
    windowButtonsLayout->addWidget(closeButton);
    titleBarLayout->addWidget(leftControls);
    titleBarLayout->addWidget(mainHeader, 1);
    titleBarLayout->addWidget(windowButtons);
    rootLayout->addWidget(titleBar);

    new FramelessResizeFilter(root, this);

    auto *mainPanel = new QWidget(body);
    auto *mainLayout = new QVBoxLayout(mainPanel);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    mainContentStack_ = new QStackedWidget(mainPanel);
    pageStack_ = new QStackedWidget(mainContentStack_);
    mainContentStack_->addWidget(pageStack_);

    auto *lyricsPage = new QWidget(mainContentStack_);
    lyricsPage->setObjectName(QStringLiteral("lyricsPage"));
    auto *lyricsPageLayout = new QVBoxLayout(lyricsPage);
    lyricsPageLayout->setContentsMargins(24, 20, 24, 16);
    lyricsPageLayout->setSpacing(8);
    auto *lyricsHeading = makeLabel(QStringLiteral("Lyrics"), QStringLiteral("lyricsHeading"));
    lyricsPageLayout->addWidget(lyricsHeading);
    lyricsTrackLabel_ = makeLabel(QStringLiteral("Nothing playing"),
                                  QStringLiteral("lyricsTrackTitle"));
    lyricsTrackLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    lyricsTrackLabel_->setWordWrap(false);
    lyricsPageLayout->addWidget(lyricsTrackLabel_);

    lyricsContentStack_ = new QStackedWidget(lyricsPage);
    auto *lyricsEmptyPage = new QWidget(lyricsContentStack_);
    auto *lyricsEmptyLayout = new QVBoxLayout(lyricsEmptyPage);
    lyricsEmptyLayout->addStretch(1);
    lyricsStatusLabel_ = makeLabel(QStringLiteral("Play a track to find its lyrics."),
                                   QStringLiteral("lyricsStatus"));
    lyricsStatusLabel_->setAlignment(Qt::AlignCenter);
    lyricsStatusLabel_->setWordWrap(true);
    lyricsEmptyLayout->addWidget(lyricsStatusLabel_);
    lyricsEmptyLayout->addStretch(1);
    lyricsContentStack_->addWidget(lyricsEmptyPage);

    lyricsList_ = new QListWidget(lyricsContentStack_);
    lyricsList_->setObjectName(QStringLiteral("lyricsList"));
    lyricsList_->setSelectionMode(QAbstractItemView::NoSelection);
    lyricsList_->setFocusPolicy(Qt::NoFocus);
    lyricsList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    lyricsList_->setWordWrap(true);
    lyricsList_->setUniformItemSizes(false);
    lyricsList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    lyricsScrollAnimation_ = new QPropertyAnimation(
        lyricsList_->verticalScrollBar(), QByteArrayLiteral("value"), this);
    lyricsScrollAnimation_->setDuration(480);
    lyricsScrollAnimation_->setEasingCurve(QEasingCurve::InOutCubic);
    lyricsColorAnimation_ = new QVariantAnimation(this);
    lyricsColorAnimation_->setDuration(320);
    lyricsColorAnimation_->setEasingCurve(QEasingCurve::InOutCubic);
    connect(lyricsColorAnimation_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
        const qreal progress = value.toReal();
        const int count = qMin(lyricsList_->count(),
                               qMin(lyricLineStartColors_.size(),
                                    lyricLineTargetColors_.size()));
        for (int index = 0; index < count; ++index) {
            const QColor from = lyricLineStartColors_.at(index);
            const QColor to = lyricLineTargetColors_.at(index);
            const QColor color(
                qRound(from.red() + (to.red() - from.red()) * progress),
                qRound(from.green() + (to.green() - from.green()) * progress),
                qRound(from.blue() + (to.blue() - from.blue()) * progress));
            lyricsList_->item(index)->setForeground(QBrush(color));
        }
    });
    lyricsContentStack_->addWidget(lyricsList_);
    lyricsPageLayout->addWidget(lyricsContentStack_, 1);
    lyricsSyncStatusLabel_ = makeLabel(QString{}, QStringLiteral("lyricsSyncStatus"));
    lyricsSyncStatusLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    lyricsPageLayout->addWidget(lyricsSyncStatusLabel_);
    mainContentStack_->addWidget(lyricsPage);
    mainLayout->addWidget(mainContentStack_, 1);
    new HoverScrollBarBehavior(lyricsList_);
    new SmoothWheelScrollFilter(lyricsList_, 54);

    auto *homePage = makePage("Your music, with a calmer interface.",
                              "A desktop player built with Qt Widgets. Connect your account to bring in your playlists and playback.");
    auto *homeLayout = qobject_cast<QVBoxLayout *>(homePage->layout());

    auto *connectCard = makeSurface(homePage);
    auto *connectLayout = new QVBoxLayout(connectCard);
    connectLayout->setContentsMargins(20, 18, 20, 18);
    connectLayout->setSpacing(9);
    connectLayout->addWidget(makeLabel("GET STARTED", "eyebrow"));
    connectLayout->addWidget(makeLabel("Connect Spotify", "cardHeading"));
    connectionDescription_ = makeLabel(
        "Sign in securely with Spotify to link your account and load your profile.", "mutedText");
    connectLayout->addWidget(connectionDescription_);
    auto *profileRow = new QHBoxLayout;
    spotifyProfileAvatarLabel_ = makeLabel("♪", "coverPlaceholder");
    spotifyProfileAvatarLabel_->setAlignment(Qt::AlignCenter);
    spotifyProfileAvatarLabel_->setFixedSize(40, 40);
    spotifyProfileNameLabel_ = makeLabel("Spotify profile", "mutedText");
    profileRow->addWidget(spotifyProfileAvatarLabel_);
    profileRow->addWidget(spotifyProfileNameLabel_);
    profileRow->addStretch();
    connectLayout->addLayout(profileRow);
    connectSpotifyButton_ = new QPushButton("Connect Spotify", connectCard);
    connectSpotifyButton_->setObjectName("primaryButton");
    connectLayout->addWidget(connectSpotifyButton_, 0, Qt::AlignLeft);
    homeLayout->addWidget(connectCard);

    auto *designCard = makeSurface(homePage);
    auto *designLayout = new QVBoxLayout(designCard);
    designLayout->setContentsMargins(20, 18, 20, 18);
    designLayout->setSpacing(8);
    designLayout->addWidget(makeLabel("THE FOUNDATION", "eyebrow"));
    designLayout->addWidget(makeLabel("Native interface. Separate playback layer.", "cardHeading"));
    designLayout->addWidget(makeLabel("Navigation, library views, and player controls stay in Qt. A separate headless browser process handles Spotify's protected audio stream.",
                                      "mutedText"));
    homeLayout->addWidget(designCard);
    homeLayout->addStretch();
    pageStack_->addWidget(homePage);

    auto *searchPage = makePage("Search", "Search tracks, artists, albums, and playlists from your Spotify account.");
    auto *searchLayout = qobject_cast<QVBoxLayout *>(searchPage->layout());
    auto *searchSurface = makeSurface(searchPage);
    auto *searchSurfaceLayout = new QVBoxLayout(searchSurface);
    searchSurfaceLayout->setContentsMargins(20, 18, 20, 18);
    searchSurfaceLayout->setSpacing(10);
    searchStatusLabel_ = makeLabel("Connect Spotify to search the catalog.", "mutedText");
    searchSurfaceLayout->addWidget(searchStatusLabel_);
    searchArtistResultsHeading_ = makeLabel(QStringLiteral("Artists"),
                                             QStringLiteral("cardHeading"));
    searchArtistResultsHeading_->hide();
    searchSurfaceLayout->addWidget(searchArtistResultsHeading_);
    searchArtistResultsList_ = new QListWidget(searchSurface);
    searchArtistResultsList_->setObjectName(QStringLiteral("searchArtistResults"));
    searchArtistResultsList_->setViewMode(QListView::IconMode);
    searchArtistResultsList_->setFlow(QListView::LeftToRight);
    searchArtistResultsList_->setWrapping(false);
    searchArtistResultsList_->setMovement(QListView::Static);
    searchArtistResultsList_->setResizeMode(QListView::Adjust);
    searchArtistResultsList_->setIconSize(QSize(64, 64));
    searchArtistResultsList_->setGridSize(QSize(100, 104));
    searchArtistResultsList_->setFixedHeight(112);
    searchArtistResultsList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    searchArtistResultsList_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    searchArtistResultsList_->hide();
    new HoverScrollBarBehavior(searchArtistResultsList_);
    connect(searchArtistResultsList_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) {
        if (item) {
            openArtistById(item->data(Qt::UserRole).toString(), item->text());
        }
    });
    searchSurfaceLayout->addWidget(searchArtistResultsList_);
    searchResultsTable_ = new TrackTableView(searchSurface);
    searchResultsTable_->setObjectName("searchResults");
    searchResultsModel_ = new TrackTableModel(searchResultsTable_);
    searchResultsTable_->setModel(searchResultsModel_);
    configureTrackTable(searchResultsTable_);
    connect(searchResultsTable_->horizontalHeader(), &QHeaderView::sectionClicked,
            this, [this](int column) {
                sortTrackTable(searchResultsTable_, searchResultsModel_, column);
            });
    static_cast<TrackTableView *>(searchResultsTable_)->setTrackActionHandler(
        [this](int row) { playTrackFromTable(searchResultsTable_, searchResultsModel_, row); });
    static_cast<TrackTableView *>(searchResultsTable_)->setArtistClickHandler(
        [this](const QString &id, const QString &name) { openArtistById(id, name); });
    static_cast<TrackTableView *>(searchResultsTable_)->setAlbumClickHandler(
        [this](const QString &id, const QString &name) { showAlbumPage(id, name); });
    searchResultsTable_->setEnabled(false);
    searchSurfaceLayout->addWidget(searchResultsTable_, 1);
    searchLayout->addWidget(searchSurface);
    searchLayout->addStretch();
    pageStack_->addWidget(searchPage);

    auto *libraryPage = new QWidget(pageStack_);
    auto *libraryLayout = new QVBoxLayout(libraryPage);
    libraryLayout->setContentsMargins(0, 0, 0, 0);
    libraryLayout->setSpacing(0);
    auto *librarySurface = makeSurface(libraryPage);
    auto *librarySurfaceLayout = new QVBoxLayout(librarySurface);
    librarySurfaceLayout->setContentsMargins(0, 0, 0, 0);
    librarySurfaceLayout->setSpacing(0);
    libraryTracksTable_ = new TrackTableView(librarySurface);
    libraryTracksTable_->setObjectName("libraryTracks");
    static_cast<TrackTableView *>(libraryTracksTable_)->setRoundedCornerRadius(8);
    libraryTracksModel_ = new TrackTableModel(libraryTracksTable_, true);
    libraryTracksTable_->setModel(libraryTracksModel_);
    configureTrackTable(libraryTracksTable_);
    libraryTracksTable_->horizontalHeader()->hide();
    libraryTracksTable_->setSpan(0, 0, 1, 6);
    libraryTracksTable_->setRowHeight(0, 118);
    libraryTracksTable_->setRowHeight(1, 40);
    static_cast<TrackTableView *>(libraryTracksTable_)->setTrackActionHandler(
        [this](int row) { playTrackFromTable(libraryTracksTable_, libraryTracksModel_, row); });
    static_cast<TrackTableView *>(libraryTracksTable_)->setArtistClickHandler(
        [this](const QString &id, const QString &name) { openArtistById(id, name); });
    static_cast<TrackTableView *>(libraryTracksTable_)->setAlbumClickHandler(
        [this](const QString &id, const QString &name) { showAlbumPage(id, name); });
    static_cast<TrackTableView *>(libraryTracksTable_)->setHeaderSortHandler(
        [this](int column) {
            sortTrackTable(libraryTracksTable_, libraryTracksModel_, column);
        });
    libraryTracksTable_->setEnabled(false);
    librarySurfaceLayout->addWidget(libraryTracksTable_, 1);
    libraryLayout->addWidget(librarySurface);
    pageStack_->addWidget(libraryPage);

    auto *settingsPage = makePage("Settings", "Set up the Spotify developer app used by this desktop player.");
    auto *settingsLayout = qobject_cast<QVBoxLayout *>(settingsPage->layout());
    auto *settingsSurface = makeSurface(settingsPage);
    auto *settingsSurfaceLayout = new QVBoxLayout(settingsSurface);
    settingsSurfaceLayout->setContentsMargins(20, 18, 20, 18);
    settingsSurfaceLayout->setSpacing(10);
    settingsSurfaceLayout->addWidget(makeLabel("Spotify developer app", "cardHeading"));
    settingsSurfaceLayout->addWidget(makeLabel(
        "Create an app in the Spotify Developer Dashboard, then paste its Client ID here. This is a public identifier; do not enter a Client Secret.",
        "mutedText"));
    spotifyClientIdEdit_ = new QLineEdit(settingsSurface);
    spotifyClientIdEdit_->setObjectName("spotifyClientId");
    spotifyClientIdEdit_->setPlaceholderText("Spotify Client ID");
    spotifyClientIdEdit_->setClearButtonEnabled(true);
    spotifyClientIdEdit_->setMinimumHeight(48);
    settingsSurfaceLayout->addWidget(spotifyClientIdEdit_);
    settingsSurfaceLayout->addWidget(makeLabel("Callback port", "mutedText"));
    spotifyCallbackPortEdit_ = new FocusWheelSpinBox(settingsSurface);
    spotifyCallbackPortEdit_->setObjectName("spotifyCallbackPort");
    spotifyCallbackPortEdit_->setRange(1024, 65535);
    spotifyCallbackPortEdit_->setValue(defaultSpotifyCallbackPort);
    spotifyCallbackPortEdit_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spotifyCallbackPortEdit_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto *callbackPortEditor = static_cast<FocusWheelSpinBox *>(spotifyCallbackPortEdit_);
    callbackPortEditor->editField()->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    callbackPortEditor->editField()->setContentsMargins(0, 0, 0, 0);
    spotifyCallbackPortEdit_->setMinimumHeight(48);
    settingsSurfaceLayout->addWidget(spotifyCallbackPortEdit_);
    spotifyRedirectUriLabel_ = makeLabel({}, "mutedText");
    settingsSurfaceLayout->addWidget(spotifyRedirectUriLabel_);
    settingsSurfaceLayout->addWidget(makeLabel(
        "Add the Redirect URI shown above to your Spotify app settings exactly. The port here must match the port registered with Spotify. Sign-in opens Spotify in your browser and returns here.",
        "mutedText"));
    settingsSurfaceLayout->addSpacing(10);
    settingsSurfaceLayout->addWidget(makeLabel("Artwork cache", "cardHeading"));
    settingsSurfaceLayout->addWidget(makeLabel(
        "Maximum disk space for cached Spotify artwork. Cached images expire after 365 days.",
        "mutedText"));
    imageCacheSizeEdit_ = new FocusWheelSpinBox(settingsSurface);
    imageCacheSizeEdit_->setObjectName("artworkCacheSizeMb");
    imageCacheSizeEdit_->setRange(16, 1024 * 1024);
    imageCacheSizeEdit_->setSingleStep(128);
    imageCacheSizeEdit_->setSuffix(" MB");
    imageCacheSizeEdit_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    imageCacheSizeEdit_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto *cacheSizeEditor = static_cast<FocusWheelSpinBox *>(imageCacheSizeEdit_);
    cacheSizeEditor->editField()->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    cacheSizeEditor->editField()->setContentsMargins(0, 0, 0, 0);
    imageCacheSizeEdit_->setMinimumHeight(48);
    settingsSurfaceLayout->addWidget(imageCacheSizeEdit_);
    settingsSurfaceLayout->addSpacing(10);
    settingsSurfaceLayout->addWidget(makeLabel(
        "Library and playlist access", "cardHeading"));
    settingsSurfaceLayout->addWidget(makeLabel(
        "Reconnect once to approve access to saved tracks, playlists, playback state, and playback controls. Spotify playback requires Premium and an installed browser with encrypted-media support.",
        "mutedText"));
    reauthorizeSpotifyButton_ = new QPushButton("Reconnect and grant updated access", settingsSurface);
    reauthorizeSpotifyButton_->setObjectName("primaryButton");
    settingsSurfaceLayout->addWidget(reauthorizeSpotifyButton_, 0, Qt::AlignLeft);
    libraryRefreshButton_ = new QPushButton("Refresh library", settingsSurface);
    libraryRefreshButton_->setEnabled(false);
    settingsSurfaceLayout->addWidget(libraryRefreshButton_, 0, Qt::AlignLeft);
    auto *settingsScrollArea = new QScrollArea(settingsPage);
    settingsScrollArea->setObjectName(QStringLiteral("settingsScrollArea"));
    settingsScrollArea->setFrameShape(QFrame::NoFrame);
    settingsScrollArea->setWidgetResizable(true);
    settingsScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    settingsScrollArea->setWidget(settingsSurface);
    new HoverScrollBarBehavior(settingsScrollArea);
    settingsLayout->addWidget(settingsScrollArea, 1);
    pageStack_->addWidget(settingsPage);

    auto *artistPage = new QWidget(pageStack_);
    artistPage->setObjectName(QStringLiteral("artistPage"));
    auto *artistPageLayout = new QVBoxLayout(artistPage);
    artistPageLayout->setContentsMargins(0, 0, 0, 0);
    artistPageLayout->setSpacing(0);
    auto *artistPageSurface = makeSurface(artistPage);
    auto *artistPageSurfaceLayout = new QVBoxLayout(artistPageSurface);
    artistPageSurfaceLayout->setContentsMargins(0, 0, 0, 0);
    artistPageSurfaceLayout->setSpacing(0);
    artistPageScroll_ = new QScrollArea(artistPageSurface);
    artistPageScroll_->setObjectName(QStringLiteral("artistPageScroll"));
    artistPageScroll_->setFrameShape(QFrame::NoFrame);
    artistPageScroll_->setWidgetResizable(true);
    artistPageScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *artistPageContent = new QWidget(artistPageScroll_);
    auto *artistPageContentLayout = new QVBoxLayout(artistPageContent);
    artistPageContentLayout->setContentsMargins(24, 22, 24, 24);
    artistPageContentLayout->setSpacing(18);
    auto *artistHero = new QHBoxLayout;
    artistPageArtworkLabel_ = new PanelImageLabel(artistPageContent);
    artistPageArtworkLabel_->setObjectName(QStringLiteral("artistPageArtwork"));
    artistPageArtworkLabel_->setAlignment(Qt::AlignCenter);
    artistPageArtworkLabel_->setFixedSize(190, 190);
    artistHero->addWidget(artistPageArtworkLabel_);
    auto *artistHeroText = new QVBoxLayout;
    artistHeroText->setSpacing(8);
    artistHeroText->addWidget(makeLabel(QStringLiteral("ARTIST"),
                                        QStringLiteral("eyebrow")));
    artistPageNameLabel_ = makeLabel(QStringLiteral("Artist"),
                                     QStringLiteral("artistPageName"));
    artistPageNameLabel_->setWordWrap(true);
    artistHeroText->addWidget(artistPageNameLabel_);
    artistPageInfoLabel_ = makeLabel(QStringLiteral("Artist details will appear here."),
                                     QStringLiteral("mutedText"));
    artistPageInfoLabel_->setWordWrap(true);
    artistHeroText->addWidget(artistPageInfoLabel_);
    artistHeroText->addStretch(1);
    artistHero->addLayout(artistHeroText, 1);
    artistPageContentLayout->addLayout(artistHero);

    auto *artistTopTracksHeading = makeLabel(QStringLiteral("Popular tracks"),
                                              QStringLiteral("cardHeading"));
    artistPageContentLayout->addWidget(artistTopTracksHeading);
    artistPageContentLayout->addWidget(makeLabel(
        QStringLiteral("Spotify no longer provides an artist top-tracks endpoint, so this section cannot show an official most-played ranking."),
        QStringLiteral("mutedText")));

    artistPageContentLayout->addWidget(makeLabel(QStringLiteral("Albums, EPs & singles"),
                                                  QStringLiteral("cardHeading")));
    artistPageStatusLabel_ = makeLabel(QStringLiteral("Connect Spotify to load releases."),
                                       QStringLiteral("mutedText"));
    artistPageContentLayout->addWidget(artistPageStatusLabel_);
    artistReleasesList_ = new QListWidget(artistPageContent);
    artistReleasesList_->setObjectName(QStringLiteral("artistReleases"));
    artistReleasesList_->setItemDelegate(new LibrarySidebarItemDelegate(artistReleasesList_));
    artistReleasesList_->setSpacing(2);
    artistReleasesList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    artistReleasesList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    artistReleasesList_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    artistReleasesList_->setMinimumHeight(120);
    new HoverScrollBarBehavior(artistReleasesList_);
    connect(artistReleasesList_->verticalScrollBar(), &QScrollBar::actionTriggered,
            this, [this] { restoreArtistScrollPending_ = false; });
    connect(artistReleasesList_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) {
        if (item) showAlbumPage(item->data(Qt::UserRole).toString(), item->text());
    });
    artistPageContentLayout->addWidget(artistReleasesList_);
    artistLoadMoreButton_ = new QPushButton(QStringLiteral("Load more releases"),
                                            artistPageContent);
    artistLoadMoreButton_->setObjectName(QStringLiteral("secondaryButton"));
    artistLoadMoreButton_->hide();
    connect(artistLoadMoreButton_, &QPushButton::clicked,
            this, [this] { loadMoreArtistReleases(); });
    artistPageContentLayout->addWidget(artistLoadMoreButton_, 0, Qt::AlignLeft);
    artistPageContentLayout->addStretch(1);
    artistPageScroll_->setWidget(artistPageContent);
    new HoverScrollBarBehavior(artistPageScroll_);
    connect(artistPageScroll_->verticalScrollBar(), &QScrollBar::actionTriggered,
            this, [this] { restoreArtistScrollPending_ = false; });
    artistPageSurfaceLayout->addWidget(artistPageScroll_);
    artistPageLayout->addWidget(artistPageSurface);
    pageStack_->addWidget(artistPage);

    auto *albumPage = new QWidget(pageStack_);
    albumPage->setObjectName(QStringLiteral("albumPage"));
    auto *albumPageLayout = new QVBoxLayout(albumPage);
    albumPageLayout->setContentsMargins(0, 0, 0, 0);
    albumPageLayout->setSpacing(0);
    auto *albumPageSurface = makeSurface(albumPage);
    auto *albumPageSurfaceLayout = new QVBoxLayout(albumPageSurface);
    albumPageSurfaceLayout->setContentsMargins(20, 18, 20, 16);
    albumPageSurfaceLayout->setSpacing(10);
    auto *albumHero = new QHBoxLayout;
    albumHero->setSpacing(18);
    albumPageArtworkLabel_ = new PanelImageLabel(albumPageSurface);
    albumPageArtworkLabel_->setObjectName(QStringLiteral("albumPageArtwork"));
    albumPageArtworkLabel_->setAlignment(Qt::AlignCenter);
    albumPageArtworkLabel_->setFixedSize(170, 170);
    albumHero->addWidget(albumPageArtworkLabel_);
    auto *albumHeroDetails = new QVBoxLayout;
    albumHeroDetails->setSpacing(8);
    albumHeroDetails->addWidget(makeLabel(QStringLiteral("ALBUM"),
                                          QStringLiteral("eyebrow")));
    albumPageNameLabel_ = makeLabel(QStringLiteral("Album"),
                                    QStringLiteral("artistPageName"));
    albumPageNameLabel_->setWordWrap(true);
    albumHeroDetails->addWidget(albumPageNameLabel_);
    albumPageArtistLabel_ = makeMarqueeLabel(QString{}, QStringLiteral("mutedText"));
    albumPageArtistLabel_->setMinimumHeight(22);
    albumPageArtistLabel_->setMaximumHeight(22);
    albumPageArtistLabel_->setClickHandler([this] {
        if (!currentAlbumProfile_.artistId.isEmpty()) {
            openArtistById(currentAlbumProfile_.artistId, albumPageArtistLabel_->text());
        }
    });
    albumHeroDetails->addWidget(albumPageArtistLabel_);
    albumPageInfoLabel_ = makeLabel(QString{}, QStringLiteral("mutedText"));
    albumPageInfoLabel_->setWordWrap(true);
    albumHeroDetails->addWidget(albumPageInfoLabel_);
    albumHeroDetails->addStretch(1);
    albumHero->addLayout(albumHeroDetails, 1);
    albumPageSurfaceLayout->addLayout(albumHero);
    albumPageStatusLabel_ = makeLabel(QStringLiteral("Connect Spotify to load tracks."),
                                      QStringLiteral("mutedText"));
    albumPageSurfaceLayout->addWidget(albumPageStatusLabel_);
    albumTracksTable_ = new TrackTableView(albumPageSurface);
    albumTracksTable_->setObjectName(QStringLiteral("albumTracks"));
    albumTracksModel_ = new TrackTableModel(albumTracksTable_);
    albumTracksTable_->setModel(albumTracksModel_);
    configureTrackTable(albumTracksTable_);
    albumTracksTable_->setEnabled(false);
    static_cast<TrackTableView *>(albumTracksTable_)->setTrackActionHandler(
        [this](int row) { playTrackFromTable(albumTracksTable_, albumTracksModel_, row); });
    static_cast<TrackTableView *>(albumTracksTable_)->setArtistClickHandler(
        [this](const QString &id, const QString &name) { openArtistById(id, name); });
    static_cast<TrackTableView *>(albumTracksTable_)->setAlbumClickHandler(
        [this](const QString &id, const QString &name) { showAlbumPage(id, name); });
    albumPageSurfaceLayout->addWidget(albumTracksTable_, 1);
    albumPageLayout->addWidget(albumPageSurface);
    pageStack_->addWidget(albumPage);
    new HoverScrollBarBehavior(albumTracksTable_);

    QSettings appSettings;
    const int savedPlaybackVolumeSliderPosition = qBound(
        0, appSettings.value(QStringLiteral("playback/volumeSliderPosition"), 50).toInt(), 100);
    spotifyClientIdEdit_->setText(appSettings.value("spotify/clientId").toString());
    imageCacheSizeEdit_->setValue(appSettings.value("cache/imageMaxMb", 10240).toInt());
    playlistFilterEdit_->setText(
        appSettings.value(QStringLiteral("window/libraryFilter")).toString());
    savedLibraryScrollPosition_ = qMax(0, appSettings.value(
        QStringLiteral("window/libraryScroll"), 0).toInt());
    savedSearchScrollPosition_ = qMax(0, appSettings.value(
        QStringLiteral("window/searchScroll"), 0).toInt());
    savedPlaylistSidebarScrollPosition_ = qMax(0, appSettings.value(
        QStringLiteral("window/librarySidebarScroll"), 0).toInt());
    restorePlaylistSidebarScrollPending_ = appSettings.contains(
        QStringLiteral("window/librarySidebarScroll"));
    int savedCallbackPort = appSettings.value("spotify/callbackPort", defaultSpotifyCallbackPort).toInt();
    if (savedCallbackPort == previousDefaultSpotifyCallbackPort) {
        savedCallbackPort = defaultSpotifyCallbackPort;
        appSettings.setValue("spotify/callbackPort", savedCallbackPort);
    }
    spotifyCallbackPortEdit_->setValue(savedCallbackPort);
    spotifyRedirectUriLabel_->setText(
        QStringLiteral("Redirect URI: http://127.0.0.1:%1").arg(spotifyCallbackPortEdit_->value()));

    NavigationEntry lastNavigation;
    lastNavigation.pageIndex = appSettings.value(QStringLiteral("navigation/lastPage"),
                                                 homePageIndex).toInt();
    if (lastNavigation.pageIndex < homePageIndex
        || lastNavigation.pageIndex > albumPageIndex) {
        lastNavigation.pageIndex = homePageIndex;
    }
    if (lastNavigation.pageIndex == libraryPageIndex) {
        lastNavigation.playlistId = appSettings.value(QStringLiteral("navigation/playlistId"))
                                        .toString();
        lastNavigation.playlistName = appSettings.value(QStringLiteral("navigation/playlistName"),
                                                        QStringLiteral("Liked Songs")).toString();
        activeLibraryPlaylistId_ = lastNavigation.playlistId;
        activeLibraryPlaylistName_ = lastNavigation.playlistName.isEmpty()
            ? QStringLiteral("Liked Songs") : lastNavigation.playlistName;
        librarySummaryText_ = QStringLiteral("Connect Spotify to load your music.");
    } else if (lastNavigation.pageIndex == searchPageIndex) {
        lastNavigation.searchQuery = appSettings.value(QStringLiteral("navigation/searchQuery"))
                                         .toString();
        searchEdit_->setText(lastNavigation.searchQuery);
    } else if (lastNavigation.pageIndex == artistPageIndex) {
        lastNavigation.artistId = appSettings.value(QStringLiteral("navigation/artistId"))
                                      .toString();
        lastNavigation.artistName = appSettings.value(QStringLiteral("navigation/artistName"))
                                        .toString();
        currentArtistPageId_ = lastNavigation.artistId;
        currentArtistPageName_ = lastNavigation.artistName;
        artistPageNameLabel_->setText(currentArtistPageName_.isEmpty()
                                          ? QStringLiteral("Artist")
                                          : currentArtistPageName_);
        artistPageStatusLabel_->setText(QStringLiteral("Connect Spotify to load releases."));
        artistPageScrollPosition_ = qMax(0, appSettings.value(
            QStringLiteral("window/artistPageScroll"), 0).toInt());
        artistReleaseListScrollPosition_ = qMax(0, appSettings.value(
            QStringLiteral("window/artistReleaseScroll"), 0).toInt());
        lastNavigation.primaryScrollPosition = artistPageScrollPosition_;
        lastNavigation.secondaryScrollPosition = artistReleaseListScrollPosition_;
    } else if (lastNavigation.pageIndex == albumPageIndex) {
        lastNavigation.albumId = appSettings.value(QStringLiteral("navigation/albumId"))
                                     .toString();
        lastNavigation.albumName = appSettings.value(QStringLiteral("navigation/albumName"))
                                       .toString();
        currentAlbumPageId_ = lastNavigation.albumId;
        currentAlbumPageName_ = lastNavigation.albumName;
        albumPageNameLabel_->setText(currentAlbumPageName_.isEmpty()
                                         ? QStringLiteral("Album") : currentAlbumPageName_);
        albumPageStatusLabel_->setText(QStringLiteral("Connect Spotify to load tracks."));
        albumPageScrollPosition_ = qMax(0, appSettings.value(
            QStringLiteral("window/albumPageScroll"), 0).toInt());
        lastNavigation.primaryScrollPosition = albumPageScrollPosition_;
    }
    if (lastNavigation.pageIndex == libraryPageIndex) {
        lastNavigation.primaryScrollPosition = savedLibraryScrollPosition_;
    } else if (lastNavigation.pageIndex == searchPageIndex) {
        lastNavigation.primaryScrollPosition = savedSearchScrollPosition_;
    }
    restoreLibraryScrollPending_ = lastNavigation.pageIndex == libraryPageIndex;
    restoreSearchScrollPending_ = lastNavigation.pageIndex == searchPageIndex
        && !lastNavigation.searchQuery.trimmed().isEmpty();
    restoreAlbumScrollPending_ = lastNavigation.pageIndex == albumPageIndex;
    restoreArtistScrollPending_ = lastNavigation.pageIndex == artistPageIndex;
    pageStack_->setCurrentIndex(lastNavigation.pageIndex);
    updateLibraryHeader();
    navigationHistory_.append(lastNavigation);
    navigationHistoryIndex_ = 0;
    updateHistoryButtons();

    connect(spotifyClientIdEdit_, &QLineEdit::editingFinished, this, [this] {
        QSettings settings;
        settings.setValue("spotify/clientId", spotifyClientIdEdit_->text().trimmed());
    });
    connect(spotifyCallbackPortEdit_, &QSpinBox::valueChanged, this, [this](int port) {
        spotifyRedirectUriLabel_->setText(QStringLiteral("Redirect URI: http://127.0.0.1:%1").arg(port));
        QSettings settings;
        settings.setValue("spotify/callbackPort", port);
    });

    spotifyConnection_ = new SpotifyConnection(this);
    spotifyConnection_->setPlaybackVolume(
        volumeSliderPositionToPercent(savedPlaybackVolumeSliderPosition));
    spotifyConnection_->setImageCacheMaxSizeMb(imageCacheSizeEdit_->value());
    connect(imageCacheSizeEdit_, &QSpinBox::valueChanged, this, [this](int megabytes) {
        QSettings settings;
        settings.setValue("cache/imageMaxMb", megabytes);
        spotifyConnection_->setImageCacheMaxSizeMb(megabytes);
    });
    const TrackTableModel::ImageProvider imageProvider = [this](
        const QString &url, std::function<void(const QImage &)> handler) {
        spotifyConnection_->requestImage(url, std::move(handler));
    };
    searchResultsModel_->setImageProvider(imageProvider);
    libraryTracksModel_->setImageProvider(imageProvider);
    albumTracksModel_->setImageProvider(imageProvider);
    spotifyConnection_->setStateHandler(
        [this](const QString &message, bool connected, bool busy) {
            updateSpotifyConnectionUi(message, connected, busy);
        });
    spotifyConnection_->setErrorHandler([this](const QString &message) {
        if (spotifyErrorDialog_) {
            if (spotifyErrorDialog_->text() != message) {
                spotifyErrorDialog_->setText(message);
            }
            return;
        }

        auto *dialog = new QMessageBox(QMessageBox::Warning,
                                       QStringLiteral("Spotify connection"),
                                       message, QMessageBox::Ok, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setModal(false);
        dialog->setStyleSheet(R"(
            QMessageBox { background: #181818; color: #ffffff; }
            QMessageBox QLabel { color: #ffffff; min-width: 360px; }
            QMessageBox QPushButton {
                background: #282828;
                color: #ffffff;
                border: none;
                border-radius: 5px;
                min-width: 80px;
                min-height: 30px;
                padding: 4px 12px;
            }
            QMessageBox QPushButton:hover { background: #3e3e3e; }
        )");
        QPalette palette = dialog->palette();
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#181818")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#ffffff")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#ffffff")));
        palette.setColor(QPalette::Button, QColor(QStringLiteral("#282828")));
        palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#ffffff")));
        dialog->setPalette(palette);
        spotifyErrorDialog_ = dialog;
        connect(dialog, &QObject::destroyed, this, [this] { spotifyErrorDialog_ = nullptr; });
        dialog->open();
    });
    spotifyConnection_->setPlaybackStateHandler([this](const SpotifyPlaybackState &state) {
        updatePlaybackUi(state, true);
    });
    spotifyConnection_->setPlaybackStatusHandler([this](const QString &message) {
        if (!playbackState_.active && nowPlayingArtistLabel_) {
            nowPlayingArtistLabel_->setText(message);
        }
        const QString normalized = message.toLower();
        if (startupWindowPending_
            && (normalized.contains(QStringLiteral("could not"))
                || normalized.contains(QStringLiteral("install "))
                || normalized.contains(QStringLiteral("failed"))
                || normalized.contains(QStringLiteral("timed out"))
                || normalized.contains(QStringLiteral("denied"))
                || normalized.contains(QStringLiteral("stopped unexpectedly")))) {
            showStartupWindow();
        }
    });
    spotifyConnection_->setDevicesUpdatedHandler([this](const QList<SpotifyDeviceResult> &devices) {
        if (!deviceButton_) {
            return;
        }
        deviceButton_->setEnabled(spotifyConnection_->isConnected());
        const QString error = spotifyConnection_->availableDevicesError();
        deviceButton_->setToolTip(error.isEmpty()
            ? QStringLiteral("Spotify Connect devices (%1 available)").arg(devices.size())
            : error);
    });

    playbackProgressTimer_ = new QTimer(this);
    playbackProgressTimer_->setInterval(250);
    connect(playbackProgressTimer_, &QTimer::timeout, this, [this] { updatePlaybackProgress(); });

    connect(playlistsList_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (!item) {
            return;
        }
        const auto type = static_cast<SpotifyLibraryItemType>(
            item->data(libraryItemTypeRole).toInt());
        const QString id = item->data(Qt::UserRole).toString();
        switch (type) {
        case SpotifyLibraryItemType::LikedSongs:
            showSavedTracks();
            break;
        case SpotifyLibraryItemType::Playlist:
            showPlaylist(id, item->text());
            break;
        case SpotifyLibraryItemType::Album:
            showAlbumPage(id, item->text());
            break;
        case SpotifyLibraryItemType::Episode: {
            const QString uri = item->data(libraryItemUriRole).toString();
            if (!uri.isEmpty()) {
                playbackSourceName_ = item->text();
                playbackSourcePlaylistId_.clear();
                playbackSourceAlbumId_.clear();
                spotifyConnection_->playTracks({uri});
            }
            break;
        }
        case SpotifyLibraryItemType::Show:
            break;
        }
    });
    connect(libraryRefreshButton_, &QPushButton::clicked, this, [this] {
        spotifyConnection_->invalidateLibraryCache();
        refreshPlaylistSidebar(true);
        loadLibraryTracks(0, false, true, true);
    });
    const auto installTrackContextMenu = [this](QTableView *table, TrackTableModel *model) {
        table->viewport()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(table->viewport(), &QWidget::customContextMenuRequested, this,
                [this, table, model](const QPoint &position) {
            showTrackContextMenu(table, model, position);
        });
    };
    installTrackContextMenu(searchResultsTable_, searchResultsModel_);
    installTrackContextMenu(libraryTracksTable_, libraryTracksModel_);
    installTrackContextMenu(albumTracksTable_, albumTracksModel_);
    connect(libraryTracksTable_->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this] {
                const int firstVisibleRow = libraryTracksTable_->rowAt(0);
                const int firstTrackRow = libraryTracksModel_->firstTrackRow();
                const int trackOffset = firstVisibleRow - firstTrackRow;
                if (trackOffset >= 0 && trackOffset < libraryTotalTracks_) {
                    requestLibraryRange(trackOffset);
                }
            });
    connect(libraryTracksTable_, &QTableView::doubleClicked, this,
            [this](const QModelIndex &index) {
                const int trackOffset = index.row() - libraryTracksModel_->firstTrackRow();
                if (index.column() == trackNumberColumn || trackOffset < 0
                    || trackOffset >= libraryTracksModel_->totalTrackCount()) return;
                playSelectedTrack();
            });

    connect(searchButton_, &QPushButton::clicked, this, [this] { performSearch(); });
    connect(searchEdit_, &QLineEdit::returnPressed, this, [this] { performSearch(); });
    connect(playlistFilterEdit_, &QLineEdit::textChanged,
            this, [this](const QString &query) { filterPlaylists(query); });
    connect(backButton_, &QToolButton::clicked, this, [this] { goBack(); });
    connect(forwardButton_, &QToolButton::clicked, this, [this] { goForward(); });
    connect(homeButton_, &QToolButton::clicked, this, [this] { navigateToPage(0); });
    connect(settingsButton_, &QToolButton::clicked, this, [this] {
        if (pageStack_->currentIndex() == settingsPageIndex) {
            goBack();
        } else {
            navigateToPage(settingsPageIndex);
        }
    });
    connect(minimizeButton, &QToolButton::clicked, this, [this] { showMinimized(); });
    connect(maximizeButton_, &QToolButton::clicked, this, [this] {
        isMaximized() ? showNormal() : showMaximized();
    });
    connect(closeButton, &QToolButton::clicked, this, [this] { close(); });

    connect(searchResultsTable_, &QTableView::doubleClicked, this,
            [this](const QModelIndex &index) {
                if (index.column() != trackNumberColumn
                    && index.column() != trackAlbumColumn) {
                    playSelectedTrack();
                }
            });
    connect(albumTracksTable_, &QTableView::doubleClicked, this,
            [this](const QModelIndex &index) {
                if (index.column() != trackNumberColumn
                    && index.column() != trackAlbumColumn) {
                    playTrackFromTable(albumTracksTable_, albumTracksModel_, index.row());
                }
            });

    const auto startSpotifyLogin = [this] {
        const QString clientId = spotifyClientIdEdit_->text().trimmed();
        if (clientId.isEmpty()) {
            navigateToPage(3);
            QMessageBox::information(
                this, "Spotify setup",
                "Add your Spotify Client ID in Settings, then choose Connect Spotify again.");
            return;
        }

        QSettings settings;
        settings.setValue("spotify/clientId", clientId);
        spotifyConnection_->beginLogin(clientId, static_cast<quint16>(spotifyCallbackPortEdit_->value()));
    };
    connect(connectSpotifyButton_, &QPushButton::clicked, this, [this, startSpotifyLogin] {
        if (spotifyConnection_->isConnected()) {
            spotifyConnection_->disconnectAccount();
            return;
        }
        startSpotifyLogin();
    });
    connect(reauthorizeSpotifyButton_, &QPushButton::clicked, this, startSpotifyLogin);

    mainPanel->setMinimumWidth(420);
    bodySplitter->addWidget(mainPanel);

    nowPlayingPanel_ = new QFrame(bodySplitter);
    nowPlayingPanel_->setObjectName(QStringLiteral("nowPlayingPanel"));
    nowPlayingPanel_->setMinimumWidth(260);
    nowPlayingPanel_->setMaximumWidth(360);
    auto *nowPlayingPanelLayout = new QVBoxLayout(nowPlayingPanel_);
    nowPlayingPanelLayout->setContentsMargins(16, 14, 16, 16);
    nowPlayingPanelLayout->setSpacing(8);

    nowPlayingPanelSourceLabel_ = makeMarqueeLabel(QStringLiteral("—"),
                                                    QStringLiteral("nowPlayingPanelSource"));
    nowPlayingPanelSourceLabel_->setMinimumHeight(30);
    nowPlayingPanelSourceLabel_->setMaximumHeight(30);
    nowPlayingPanelSourceLabel_->setClickHandler([this] { focusPlaybackSource(); });
    nowPlayingPanelLayout->addWidget(nowPlayingPanelSourceLabel_);
    nowPlayingPanelLayout->addSpacing(6);

    nowPlayingPanelTrackArtworkLabel_ = new PanelImageLabel(nowPlayingPanel_);
    nowPlayingPanelTrackArtworkLabel_->setObjectName(QStringLiteral("trackPanelArtwork"));
    nowPlayingPanelTrackArtworkLabel_->setAlignment(Qt::AlignCenter);
    nowPlayingPanelTrackArtworkLabel_->setMinimumWidth(0);
    nowPlayingPanelTrackArtworkLabel_->setFixedHeight(220);
    nowPlayingPanelTrackArtworkLabel_->setSizePolicy(QSizePolicy::Expanding,
                                                      QSizePolicy::Fixed);
    nowPlayingPanelTrackArtworkLabel_->setText(QStringLiteral("♪"));
    nowPlayingPanelLayout->addWidget(nowPlayingPanelTrackArtworkLabel_);
    nowPlayingPanelTrackTitleLabel_ = makeMarqueeLabel(QStringLiteral("Nothing playing"),
                                                        QStringLiteral("nowPlayingPanelTrackTitle"));
    nowPlayingPanelTrackTitleLabel_->setMinimumHeight(32);
    nowPlayingPanelTrackTitleLabel_->setMaximumHeight(34);
    nowPlayingPanelTrackTitleLabel_->setClickHandler([this] {
        if (!playbackState_.albumId.isEmpty()) {
            showAlbumPage(playbackState_.albumId, playbackState_.album);
        }
    });
    nowPlayingPanelLayout->addWidget(nowPlayingPanelTrackTitleLabel_);
    nowPlayingPanelLayout->addSpacing(16);

    auto *artistInfoCard = new QFrame(nowPlayingPanel_);
    artistInfoCard->setObjectName(QStringLiteral("artistInfoCard"));
    auto *artistInfoLayout = new QVBoxLayout(artistInfoCard);
    artistInfoLayout->setContentsMargins(0, 0, 0, 0);
    artistInfoLayout->setSpacing(0);
    auto *artistBanner = new QWidget(artistInfoCard);
    artistBanner->setObjectName(QStringLiteral("artistBanner"));
    artistBanner->setFixedHeight(220);
    artistBanner->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *artistBannerLayout = new QStackedLayout(artistBanner);
    artistBannerLayout->setContentsMargins(0, 0, 0, 0);
    artistBannerLayout->setStackingMode(QStackedLayout::StackAll);
    nowPlayingPanelArtistArtworkLabel_ = new PanelImageLabel(artistBanner);
    nowPlayingPanelArtistArtworkLabel_->setObjectName(QStringLiteral("artistInfoArtwork"));
    nowPlayingPanelArtistArtworkLabel_->setAlignment(Qt::AlignCenter);
    nowPlayingPanelArtistArtworkLabel_->setText(QStringLiteral("♪"));
    nowPlayingPanelArtistArtworkLabel_->setRoundedBottomCorners(false);
    nowPlayingPanelArtistArtworkLabel_->setTopFadeEnabled(true);
    artistBannerLayout->addWidget(nowPlayingPanelArtistArtworkLabel_);
    auto *artistBannerOverlay = new QWidget(artistBanner);
    artistBannerOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *artistBannerOverlayLayout = new QVBoxLayout(artistBannerOverlay);
    artistBannerOverlayLayout->setContentsMargins(12, 12, 12, 12);
    artistBannerOverlayLayout->addWidget(makeLabel(QStringLiteral("ABOUT THE ARTIST"),
                                                    QStringLiteral("artistBannerHeading")),
                                         0, Qt::AlignLeft | Qt::AlignTop);
    artistBannerOverlayLayout->addStretch(1);
    artistBannerLayout->addWidget(artistBannerOverlay);
    artistBannerLayout->setCurrentWidget(artistBannerOverlay);
    artistInfoLayout->addWidget(artistBanner);
    auto *artistInfoTextCard = new QFrame(artistInfoCard);
    artistInfoTextCard->setObjectName(QStringLiteral("artistInfoTextCard"));
    auto *artistInfoTextLayout = new QVBoxLayout(artistInfoTextCard);
    artistInfoTextLayout->setContentsMargins(12, 12, 12, 12);
    artistInfoTextLayout->setSpacing(6);
    nowPlayingPanelArtistNameLabel_ = makeMarqueeLabel(QStringLiteral("Artist"),
                                                        QStringLiteral("artistInfoName"));
    nowPlayingPanelArtistNameLabel_->setWordWrap(false);
    nowPlayingPanelArtistNameLabel_->setFixedHeight(28);
    nowPlayingPanelArtistNameLabel_->setClickHandler([this] {
        if (!displayedNowPlayingArtistId_.isEmpty()) {
            openArtistById(displayedNowPlayingArtistId_, nowPlayingPanelArtistNameLabel_->text());
        }
    });
    nowPlayingPanelArtistListenersLabel_ = makeLabel(
        QStringLiteral("Monthly listeners unavailable"), QStringLiteral("artistInfoListeners"));
    nowPlayingPanelArtistListenersLabel_->setWordWrap(true);
    nowPlayingPanelArtistInfoLabel_ = makeLabel(QStringLiteral("Artist details appear here."),
                                                 QStringLiteral("artistInfoDetails"));
    nowPlayingPanelArtistInfoLabel_->setWordWrap(true);
    artistInfoTextLayout->addWidget(nowPlayingPanelArtistNameLabel_);
    artistInfoTextLayout->addWidget(nowPlayingPanelArtistListenersLabel_);
    artistInfoTextLayout->addWidget(nowPlayingPanelArtistInfoLabel_);
    artistInfoLayout->addWidget(artistInfoTextCard);
    nowPlayingPanelLayout->addWidget(artistInfoCard);
    nowPlayingPanelLayout->addStretch(1);

    bodySplitter->addWidget(nowPlayingPanel_);
    nowPlayingPanel_->show();
    bodySplitter->setStretchFactor(0, 0);
    bodySplitter->setStretchFactor(1, 1);
    bodySplitter->setStretchFactor(2, 0);
    bodySplitter->setSizes({216, 650, 300});
    connect(bodySplitter, &QSplitter::splitterMoved, this,
            [leftControls, bodySplitter](int, int) {
                const QList<int> sizes = bodySplitter->sizes();
                if (!sizes.isEmpty()) {
                    leftControls->setFixedWidth(sizes.first() + bodySplitter->handleWidth());
                }
            });
    bodyLayout->addWidget(bodySplitter);
    rootLayout->addWidget(body, 1);

    auto *playerBar = new QFrame(root);
    playerBar->setObjectName("playerBar");
    playerBar->setFrameShape(QFrame::NoFrame);
    auto *playerLayout = new QHBoxLayout(playerBar);
    playerLayout->setContentsMargins(14, 10, 14, 10);
    playerLayout->setSpacing(14);

    auto *trackInfo = new QHBoxLayout;
    nowPlayingArtworkLabel_ = makeLabel("♪", "coverPlaceholder");
    nowPlayingArtworkLabel_->setAlignment(Qt::AlignCenter);
    nowPlayingArtworkLabel_->setFixedSize(48, 48);
    auto *trackText = new QVBoxLayout;
    trackText->setContentsMargins(0, 0, 0, 0);
    trackText->setSpacing(0);
    nowPlayingTitleLabel_ = makeMarqueeLabel("No track selected", "trackTitle");
    nowPlayingTitleLabel_->setClickHandler([this] {
        if (!playbackState_.albumId.isEmpty()) {
            showAlbumPage(playbackState_.albumId, playbackState_.album);
        }
    });
    nowPlayingArtistLabel_ = makeMarqueeLabel("Connect Spotify to start listening",
                                               "playerArtistText");
    nowPlayingArtistLabel_->setClickHandler([this] {
        if (!displayedNowPlayingArtistId_.isEmpty()) {
            const QString artistName = nowPlayingArtistLabel_->text()
                                           .section(QStringLiteral(", "), 0, 0).trimmed();
            openArtistById(displayedNowPlayingArtistId_, artistName);
        }
    });
    nowPlayingTitleLabel_->setFixedHeight(18);
    nowPlayingArtistLabel_->setFixedHeight(18);
    trackText->addWidget(nowPlayingTitleLabel_);
    trackText->addWidget(nowPlayingArtistLabel_);
    trackInfo->addWidget(nowPlayingArtworkLabel_);
    trackInfo->addSpacing(2);
    trackInfo->addLayout(trackText, 1);
    playerLayout->addLayout(trackInfo, 3);

    auto *playbackColumn = new QVBoxLayout;
    playbackColumn->setSpacing(5);
    auto *transportControls = new QHBoxLayout;
    transportControls->setSpacing(8);
    transportControls->addStretch();
    previousTrackButton_ = makeMediaButton(QStyle::SP_MediaSkipBackward, "Previous track", playerBar);
    previousTrackButton_->setIcon(playbackControlIcon(0));
    previousTrackButton_->setIconSize(QSize(24, 24));
    previousTrackButton_->setFixedSize(42, 42);
    previousTrackButton_->setObjectName(QStringLiteral("transportIconButton"));
    previousTrackButton_->setAutoRaise(true);
    transportControls->addWidget(previousTrackButton_);
    transportControls->addSpacing(8);
    playPauseButton_ = makeMediaButton(QStyle::SP_MediaPlay, "Play", playerBar);
    playPauseButton_->setIcon(playbackControlIcon(1, Qt::black, Qt::black));
    playPauseButton_->setIconSize(QSize(24, 24));
    playPauseButton_->setObjectName("playButton");
    transportControls->addWidget(playPauseButton_);
    transportControls->addSpacing(8);
    nextTrackButton_ = makeMediaButton(QStyle::SP_MediaSkipForward, "Next track", playerBar);
    nextTrackButton_->setIcon(playbackControlIcon(3));
    nextTrackButton_->setIconSize(QSize(24, 24));
    nextTrackButton_->setFixedSize(42, 42);
    nextTrackButton_->setObjectName(QStringLiteral("transportIconButton"));
    nextTrackButton_->setAutoRaise(true);
    transportControls->addWidget(nextTrackButton_);
    repeatButton_ = new QToolButton(playerBar);
    repeatButton_->setObjectName(QStringLiteral("repeatButton"));
    repeatButton_->setIcon(repeatModeIcon(QStringLiteral("off")));
    repeatButton_->setIconSize(QSize(24, 24));
    repeatButton_->setFixedSize(32, 36);
    repeatButton_->setCheckable(true);
    repeatButton_->setAutoRaise(true);
    repeatButton_->setAccessibleName(QStringLiteral("Repeat mode"));
    repeatButton_->setToolTip(QStringLiteral("Repeat off"));
    repeatButton_->setEnabled(false);
    transportControls->addWidget(repeatButton_);
    transportControls->addStretch();
    playbackColumn->addLayout(transportControls);

    auto *progressRow = new QHBoxLayout;
    progressRow->setSpacing(8);
    progressRow->addStretch(1);
    playbackPositionLabel_ = makeLabel("0:00", "timeLabel");
    progressRow->addWidget(playbackPositionLabel_);
    auto *progressSlider = new ClickToSetSlider(Qt::Horizontal, playerBar);
    playbackProgressSlider_ = progressSlider;
    playbackProgressSlider_->setObjectName(QStringLiteral("playbackProgressSlider"));
    playbackProgressSlider_->setTracking(true);
    playbackProgressSlider_->setRange(0, 0);
    playbackProgressSlider_->setValue(0);
    playbackProgressSlider_->setEnabled(false);
    playbackProgressSlider_->setMouseTracking(true);
    playbackProgressSlider_->setAttribute(Qt::WA_Hover, true);
    playbackProgressSlider_->setFixedHeight(20);
    playbackProgressSlider_->setMaximumWidth(560);
    auto *playbackHoverTimeLabel = new QLabel(playerBar);
    playbackHoverTimeLabel->setObjectName(QStringLiteral("playbackHoverTime"));
    playbackHoverTimeLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    playbackHoverTimeLabel->hide();
    progressSlider->setHoverHandler(
        [progressSlider, playerBar, playbackHoverTimeLabel](bool visible, int timeMs,
                                                             const QPoint &position) {
            if (!visible || !progressSlider->isEnabled()) {
                playbackHoverTimeLabel->hide();
                return;
            }
            playbackHoverTimeLabel->setText(formatPlaybackTime(timeMs));
            playbackHoverTimeLabel->adjustSize();
            const QPoint sliderOrigin = progressSlider->mapTo(playerBar, QPoint(0, 0));
            const int wantedX = sliderOrigin.x() + position.x()
                - playbackHoverTimeLabel->width() / 2;
            const int maxX = qMax(4, playerBar->width() - playbackHoverTimeLabel->width() - 4);
            playbackHoverTimeLabel->move(qBound(4, wantedX, maxX),
                                         sliderOrigin.y() - playbackHoverTimeLabel->height() - 1);
            playbackHoverTimeLabel->show();
            playbackHoverTimeLabel->raise();
        });
    // Let the slider use the available width first; once it reaches its cap,
    // the equal outer stretches center the whole time row under the controls.
    progressRow->addWidget(playbackProgressSlider_, 100);
    playbackDurationLabel_ = makeLabel("0:00", "timeLabel");
    progressRow->addWidget(playbackDurationLabel_);
    progressRow->addStretch(1);
    playbackColumn->addLayout(progressRow);
    // Keep the playback column centered as the window grows. Unequal left and
    // right stretch factors made the transport controls drift right in fullscreen.
    playerLayout->addLayout(playbackColumn, 5);

    auto *volumeRow = new QHBoxLayout;
    volumeRow->setSpacing(8);
    deviceButton_ = new QToolButton(playerBar);
    deviceButton_->setObjectName(QStringLiteral("deviceButton"));
    deviceButton_->setIcon(devicesIcon());
    deviceButton_->setIconSize(QSize(20, 20));
    deviceButton_->setFixedSize(30, 30);
    deviceButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    deviceButton_->setAccessibleName(QStringLiteral("Available devices"));
    deviceButton_->setAutoRaise(true);
    deviceButton_->setEnabled(false);
    deviceButton_->setVisible(false);
    volumeRow->addWidget(deviceButton_);
    lyricsButton_ = new QToolButton(playerBar);
    lyricsButton_->setObjectName(QStringLiteral("lyricsButton"));
    lyricsButton_->setIcon(lyricsMicrophoneIcon());
    lyricsButton_->setIconSize(QSize(20, 20));
    lyricsButton_->setFixedSize(30, 30);
    lyricsButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    lyricsButton_->setCheckable(true);
    lyricsButton_->setAccessibleName(QStringLiteral("Show lyrics"));
    lyricsButton_->setToolTip(QStringLiteral("Show lyrics"));
    lyricsButton_->setAutoRaise(true);
    volumeRow->addWidget(lyricsButton_);
    nowPlayingPanelButton_ = new QToolButton(playerBar);
    nowPlayingPanelButton_->setObjectName(QStringLiteral("nowPlayingPanelButton"));
    nowPlayingPanelButton_->setIcon(nowPlayingPanelIcon());
    nowPlayingPanelButton_->setIconSize(QSize(20, 20));
    nowPlayingPanelButton_->setFixedSize(30, 30);
    nowPlayingPanelButton_->setCheckable(true);
    nowPlayingPanelButton_->setChecked(true);
    nowPlayingPanelButton_->setAccessibleName(QStringLiteral("Toggle artist panel"));
    volumeMuteButton_ = new QToolButton(playerBar);
    volumeMuteButton_->setObjectName(QStringLiteral("volumeMuteButton"));
    volumeMuteButton_->setIcon(volumeControlIcon(false));
    volumeMuteButton_->setIconSize(QSize(20, 20));
    volumeMuteButton_->setFixedSize(28, 28);
    volumeMuteButton_->setAutoRaise(true);
    volumeMuteButton_->setAccessibleName(QStringLiteral("Mute"));
    volumeMuteButton_->setToolTip(QStringLiteral("Mute"));
    playbackVolumeSlider_ = new ClickToSetSlider(Qt::Horizontal, playerBar);
    playbackVolumeSlider_->setObjectName(QStringLiteral("playbackVolumeSlider"));
    playbackVolumeSlider_->setRange(0, 100);
    playbackVolumeSlider_->setValue(savedPlaybackVolumeSliderPosition);
    if (savedPlaybackVolumeSliderPosition > 0) {
        lastNonZeroVolume_ = savedPlaybackVolumeSliderPosition;
    }
    playbackVolumeSlider_->setEnabled(false);
    playbackVolumeSlider_->setMouseTracking(true);
    playbackVolumeSlider_->setAttribute(Qt::WA_Hover, true);
    playbackVolumeSlider_->setFixedHeight(20);
    playbackVolumeSlider_->setMaximumWidth(150);
    volumeRow->addWidget(volumeMuteButton_);
    volumeRow->addWidget(playbackVolumeSlider_);
    volumeRow->addSpacing(4);
    volumeRow->addWidget(nowPlayingPanelButton_);
    playerLayout->addLayout(volumeRow, 3);
    playerLayout->setAlignment(volumeRow, Qt::AlignRight);

    rootLayout->addWidget(playerBar);

    connect(nowPlayingPanelButton_, &QToolButton::toggled, this,
            [this, bodySplitter, mainPanel](bool visible) {
        nowPlayingPanel_->setVisible(visible);
        if (visible) {
            const QList<int> sizes = bodySplitter->sizes();
            const int sidebarWidth = sizes.value(0, 216);
            const int mainWidth = sizes.value(1, mainPanel->width());
            const int panelWidth = qBound(260, qMin(300, mainWidth - 420), 360);
            bodySplitter->setSizes({sidebarWidth, qMax(420, mainWidth - panelWidth),
                                    panelWidth});
        }
    });
    connect(lyricsButton_, &QToolButton::toggled, this, [this](bool open) {
        lyricsViewOpen_ = open;
        lyricsButton_->setIcon(lyricsMicrophoneIcon(open));
        lyricsButton_->setAccessibleName(open ? QStringLiteral("Hide lyrics")
                                              : QStringLiteral("Show lyrics"));
        lyricsButton_->setToolTip(open ? QStringLiteral("Hide lyrics")
                                       : QStringLiteral("Show lyrics"));
        mainContentStack_->setCurrentIndex(open ? 1 : 0);
        if (open) {
            updateLyricsForPlayback(playbackState_);
        }
    });
    connect(previousTrackButton_, &QToolButton::clicked, this,
            [this] { skipTrackInCurrentList(-1); });
    connect(nextTrackButton_, &QToolButton::clicked, this,
            [this] { skipTrackInCurrentList(1); });
    connect(repeatButton_, &QToolButton::clicked, this, [this] {
        QString currentMode = playbackRepeatModeOverride_.isEmpty()
            ? playbackState_.repeatState : playbackRepeatModeOverride_;
        const QString nextMode = currentMode == QStringLiteral("off")
            ? QStringLiteral("context")
            : currentMode == QStringLiteral("context")
                ? QStringLiteral("track") : QStringLiteral("off");
        playbackRepeatModeOverride_ = nextMode;
        playbackRepeatModeOverrideUntilMs_ = QDateTime::currentMSecsSinceEpoch() + 4000;
        repeatButton_->setIcon(repeatModeIcon(nextMode));
        repeatButton_->setChecked(nextMode != QStringLiteral("off"));
        repeatButton_->setToolTip(nextMode == QStringLiteral("off")
            ? QStringLiteral("Repeat off")
            : nextMode == QStringLiteral("track")
                ? QStringLiteral("Repeat track") : QStringLiteral("Repeat context"));
        spotifyConnection_->setRepeatMode(nextMode);
    });
    connect(playPauseButton_, &QToolButton::clicked, this, [this] {
        const QString selectedUri = selectedTrackUri();
        if (restoredPlaybackPending_ && playbackState_.active && playbackState_.paused
            && (selectedUri.isEmpty() || selectedUri == playbackState_.trackUri)) {
            const QString trackUri = playbackState_.trackUri;
            playbackUiCommandPending_ = true;
            playbackUiRequestedPaused_ = false;
            playbackUiRequestedTrackUri_ = trackUri;
            playbackUiCommandUntilMs_ = QDateTime::currentMSecsSinceEpoch() + 8000;
            playPauseButton_->setIcon(playbackControlIcon(2, Qt::black, Qt::black));
            playPauseButton_->setToolTip(QStringLiteral("Pause"));
            lastRequestedTrackUri_ = trackUri;
            QString contextUri;
            if (!playbackSourcePlaylistId_.isEmpty()) {
                contextUri = QStringLiteral("spotify:playlist:%1")
                                 .arg(playbackSourcePlaylistId_);
            } else if (!playbackSourceAlbumId_.isEmpty()) {
                contextUri = QStringLiteral("spotify:album:%1")
                                 .arg(playbackSourceAlbumId_);
            }
            if (contextUri.isEmpty()) {
                spotifyConnection_->playTracks({trackUri}, {}, {}, restoredPlaybackPositionMs_);
            } else {
                spotifyConnection_->playTracks({}, contextUri, trackUri,
                                               restoredPlaybackPositionMs_);
            }
            return;
        }
        const bool samePendingTrack = playbackUiCommandPending_
            && selectedUri == playbackUiRequestedTrackUri_;
        const bool startSelection = !selectedUri.isEmpty() && !samePendingTrack
            && (!playbackState_.active || (selectedUri != playbackState_.trackUri
                                           && selectedUri != lastRequestedTrackUri_));
        if (startSelection) {
            playbackUiCommandPending_ = true;
            playbackUiRequestedPaused_ = false;
            playbackUiRequestedTrackUri_ = selectedUri;
            playbackUiCommandUntilMs_ = QDateTime::currentMSecsSinceEpoch() + 5000;
            playPauseButton_->setIcon(playbackControlIcon(2, Qt::black, Qt::black));
            playPauseButton_->setToolTip(QStringLiteral("Pause"));
            playSelectedTrack();
        } else if (playbackUiCommandPending_ || playbackState_.active) {
            const bool currentlyPaused = playbackUiCommandPending_
                ? playbackUiRequestedPaused_ : playbackState_.paused;
            playbackUiCommandPending_ = true;
            playbackUiRequestedPaused_ = !currentlyPaused;
            playbackUiRequestedTrackUri_ = playbackState_.trackUri;
            playbackUiCommandUntilMs_ = QDateTime::currentMSecsSinceEpoch() + 5000;
            playPauseButton_->setIcon(playbackControlIcon(
                playbackUiRequestedPaused_ ? 1 : 2, Qt::black, Qt::black));
            playPauseButton_->setToolTip(playbackUiRequestedPaused_
                ? QStringLiteral("Play") : QStringLiteral("Pause"));
            if (currentlyPaused) {
                spotifyConnection_->resumePlayback();
            } else {
                spotifyConnection_->pausePlayback();
            }
        } else if (!selectedUri.isEmpty()) {
            playbackUiCommandPending_ = true;
            playbackUiRequestedPaused_ = false;
            playbackUiRequestedTrackUri_ = selectedUri;
            playbackUiCommandUntilMs_ = QDateTime::currentMSecsSinceEpoch() + 5000;
            playPauseButton_->setIcon(playbackControlIcon(2, Qt::black, Qt::black));
            playPauseButton_->setToolTip(QStringLiteral("Pause"));
            playSelectedTrack();
        } else if (nowPlayingArtistLabel_) {
            nowPlayingArtistLabel_->setText(QStringLiteral("Select a track, then press Play."));
        }
    });
    connect(playbackProgressSlider_, &QSlider::sliderReleased, this, [this] {
        if (playbackState_.active) {
            playbackState_.positionMs = playbackProgressSlider_->value();
            playbackPositionSampleMs_ = playbackState_.positionMs;
            playbackPositionClock_.restart();
            playbackSeekPending_ = true;
            playbackSeekTrackUri_ = playbackState_.trackUri;
            playbackSeekTargetMs_ = playbackState_.positionMs;
            playbackSeekPendingUntilMs_ = QDateTime::currentMSecsSinceEpoch() + 4000;
            playbackPositionLabel_->setText(formatPlaybackTime(playbackState_.positionMs));
            spotifyConnection_->seekPlayback(playbackState_.positionMs);
            updateLyricsHighlight();
        }
    });
    connect(playbackVolumeSlider_, &QSlider::valueChanged, this, [this](int volume) {
        if (volume > 0) {
            lastNonZeroVolume_ = volume;
        }
        const bool muted = volume == 0;
        volumeMuteButton_->setIcon(volumeControlIcon(muted));
        volumeMuteButton_->setAccessibleName(muted ? QStringLiteral("Unmute")
                                                    : QStringLiteral("Mute"));
        volumeMuteButton_->setToolTip(muted ? QStringLiteral("Unmute")
                                            : QStringLiteral("Mute"));
        if (!playbackVolumeSlider_->isSliderDown()) {
            QSettings settings;
            settings.setValue(QStringLiteral("playback/volumeSliderPosition"), volume);
        }
        spotifyConnection_->setPlaybackVolume(volumeSliderPositionToPercent(volume));
    });
    connect(playbackVolumeSlider_, &QSlider::sliderReleased, this, [this] {
        QSettings settings;
        settings.setValue(QStringLiteral("playback/volumeSliderPosition"),
                          playbackVolumeSlider_->value());
    });
    connect(volumeMuteButton_, &QToolButton::clicked, this, [this] {
        if (playbackVolumeSlider_->value() == 0) {
            playbackVolumeSlider_->setValue(qBound(1, lastNonZeroVolume_, 100));
        } else {
            lastNonZeroVolume_ = playbackVolumeSlider_->value();
            playbackVolumeSlider_->setValue(0);
        }
    });
    connect(deviceButton_, &QToolButton::clicked, this, [this] {
        spotifyConnection_->refreshAvailableDevices([this] {
            auto *menu = new QMenu(deviceButton_);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            menu->setStyleSheet(R"(
                QMenu {
                    background: #282828;
                    color: #b3b3b3;
                    border: 1px solid #3e3e3e;
                    padding: 5px;
                }
                QMenu::item {
                    background: transparent;
                    color: #b3b3b3;
                    padding: 8px 24px 8px 10px;
                    border-radius: 4px;
                }
                QMenu::item:selected { background: #3e3e3e; color: #ffffff; }
                QMenu::item:disabled { color: #6a6a6a; }
                QMenu::separator { height: 1px; background: #3e3e3e; margin: 5px 6px; }
                QMenu::section { color: #ffffff; font-weight: 700; padding: 7px 8px 5px; }
            )");
            menu->addSection(QStringLiteral("Spotify Connect devices"));

            QList<SpotifyDeviceResult> devices = spotifyConnection_->availableDevices();
            const QString devicesError = spotifyConnection_->availableDevicesError();
            if (!devicesError.isEmpty()) {
                QAction *errorAction = menu->addAction(devicesError);
                errorAction->setEnabled(false);
            }
            const QString localDeviceId = spotifyConnection_->localPlaybackDeviceId();
            const bool localDeviceListed = std::any_of(
                devices.cbegin(), devices.cend(), [&localDeviceId](const SpotifyDeviceResult &device) {
                    return !localDeviceId.isEmpty() && device.id == localDeviceId;
                });
            if (!localDeviceId.isEmpty() && !localDeviceListed) {
                SpotifyDeviceResult localDevice;
                localDevice.id = localDeviceId;
                localDevice.name = QStringLiteral("MusicPlayer");
                localDevice.type = QStringLiteral("computer");
                devices.prepend(localDevice);
            }

            for (const SpotifyDeviceResult &device : devices) {
                if (device.id.isEmpty()) {
                    continue;
                }
                const bool isThisApp = device.id == localDeviceId;
                QString label = device.name.isEmpty() ? device.type : device.name;
                if (label.isEmpty()) {
                    label = QStringLiteral("Spotify device");
                }
                if (isThisApp) {
                    label += QStringLiteral(" (This app)");
                } else if (!device.type.isEmpty()) {
                    label += QStringLiteral(" · %1").arg(device.type);
                }

                QAction *action = menu->addAction(label);
                action->setCheckable(true);
                action->setChecked(device.active || device.id == playbackState_.deviceId);
                action->setEnabled(!device.restricted);
                if (device.restricted) {
                    action->setToolTip(QStringLiteral("Spotify does not allow this device to be controlled."));
                }
                connect(action, &QAction::triggered, this, [this, id = device.id] {
                    spotifyConnection_->transferPlaybackToDevice(id);
                });
            }

            if (devices.isEmpty() && devicesError.isEmpty()) {
                QAction *empty = menu->addAction(QStringLiteral(
                    "Spotify reported no available devices. Open Spotify on another device; some device models may not be listed."));
                empty->setEnabled(false);
            }
            menu->popup(deviceButton_->mapToGlobal(QPoint(0, deviceButton_->height())));
        });
    });

    setCentralWidget(root);

    setStyleSheet(R"(
        QMainWindow { background: #000000; }
        QWidget#root { background: #000000; color: #ffffff; }
        QWidget#titleBar { background: #000000; }
        QWidget#windowDragArea { background: transparent; }
        QFrame#sidebar {
            background: #121212;
            border: none;
            border-radius: 8px;
        }
        QFrame#nowPlayingPanel {
            background: #121212;
            border: none;
            border-radius: 8px;
        }
        QLabel#nowPlayingPanelSource { color: #ffffff; font-size: 16px; font-weight: 700; }
        QLabel#trackPanelArtwork {
            background: #121212;
            color: #b3b3b3;
            border-radius: 6px;
            font-size: 28px;
        }
        QLabel#nowPlayingPanelTrackTitle { color: #ffffff; font-size: 23px; font-weight: 700; }
        QFrame#artistInfoCard { background: transparent; border: none; }
        QFrame#artistInfoTextCard {
            background: #1e1e1e;
            border: none;
            border-bottom-left-radius: 8px;
            border-bottom-right-radius: 8px;
        }
        QLabel#artistBannerHeading {
            background: transparent;
            color: #ffffff;
            padding: 0px;
            font-size: 12px;
            font-weight: 700;
        }
        QLabel#artistInfoArtwork {
            background: #121212;
            color: #b3b3b3;
            border-top-left-radius: 6px;
            border-top-right-radius: 6px;
            border-bottom-left-radius: 0px;
            border-bottom-right-radius: 0px;
            font-size: 28px;
        }
        QLabel#artistInfoName { color: #ffffff; font-size: 18px; font-weight: 700; }
        QLabel#artistInfoListeners { color: #b3b3b3; font-size: 11px; }
        QLabel#artistInfoDetails { color: #b3b3b3; font-size: 12px; }
        QLabel#artistPageName { color: #ffffff; font-size: 34px; font-weight: 750; }
        QLabel#artistPageArtwork {
            background: #282828;
            color: #b3b3b3;
            border-radius: 8px;
            font-size: 34px;
        }
        QLabel#albumPageArtwork {
            background: #282828;
            color: #b3b3b3;
            border-radius: 8px;
            font-size: 34px;
        }
        QSplitter#mainSplitter::handle:horizontal {
            background: #000000;
            width: 7px;
        }
        QSplitter#mainSplitter::handle:horizontal:hover { background: #535353; }
        QFrame#surface {
            background: #181818;
            border: none;
            border-radius: 8px;
        }
        QFrame#playerBar {
            background: #000000;
            border: none;
            border-top: 1px solid #282828;
            border-radius: 0;
        }
        QWidget#lyricsPage { background: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #353b38, stop: 0.55 #202320, stop: 1 #181818); }
        QLabel#lyricsHeading { color: #ffffff; font-size: 28px; font-weight: 700; }
        QLabel#lyricsTrackTitle { color: #b3b3b3; font-size: 14px; }
        QLabel#lyricsStatus, QLabel#lyricsSyncStatus {
            color: #b3b3b3;
            font-size: 13px;
        }
        QListWidget#lyricsList {
            background: transparent;
            border: none;
            outline: none;
            padding: 14px 0;
        }
        QListWidget#lyricsList::item {
            background: transparent;
            border: none;
            padding: 10px 18px;
        }
        QListWidget#lyricsList::item:hover,
        QListWidget#lyricsList::item:selected { background: transparent; }
        QLabel#librarySidebarHeading { font-size: 18px; font-weight: 700; padding: 3px 2px; }
        QLabel#eyebrow { color: #b3b3b3; font-size: 10px; font-weight: 700; }
        QLabel#mutedText { color: #b3b3b3; }
        QLabel#pageHeading { font-size: 27px; font-weight: 650; }
        QLabel#pageDescription { color: #b3b3b3; font-size: 13px; }
        QLabel#statusDot { color: #f5c46b; font-size: 10px; }
        QLabel#cardHeading { font-size: 17px; font-weight: 600; }
        QLabel#coverPlaceholder {
            background: #282828;
            color: #b3b3b3;
            border: none;
            border-radius: 6px;
            font-size: 22px;
        }
        QLabel#trackTitle { font-size: 13px; font-weight: 600; }
        QLabel#timeLabel { color: #ffffff; font-size: 10px; }
        QLabel#playerArtistText { color: #b3b3b3; font-size: 11px; }
        QLabel#playbackHoverTime {
            background: #282828;
            color: #ffffff;
            border: 1px solid #3e3e3e;
            border-radius: 4px;
            padding: 3px 6px;
            font-size: 10px;
        }
        QToolButton#titleBarButton {
            background: transparent;
            color: #b3b3b3;
            border: none;
            border-radius: 0;
            font-size: 25px;
            padding: 0;
        }
        QToolButton#titleBarButton:hover { background: #282828; color: #ffffff; }
        QToolButton#titleBarButton:disabled { color: #535353; }
        QToolButton#homeButton {
            background: transparent;
            color: #b3b3b3;
            border: none;
            border-radius: 18px;
            padding: 0;
        }
        QToolButton#homeButton:hover { background: #282828; color: #ffffff; }
        QToolButton#windowControlButton {
            background: transparent;
            color: #b3b3b3;
            border: none;
            border-radius: 0;
            font-size: 18px;
        }
        QToolButton#windowControlButton:hover { background: #282828; color: #ffffff; }
        QWidget#titleBar QWidget#windowButtons { background: transparent; }
        QToolButton#windowCloseButton {
            background: transparent;
            color: #b3b3b3;
            border: none;
            border-radius: 0;
            font-size: 18px;
        }
        QToolButton#windowCloseButton:hover { background: #e81123; color: #ffffff; }
        QToolButton#windowCloseButton:pressed { background: #c50f1f; }
        QFrame#topSearchBar {
            background: #242424;
            border: 1px solid #242424;
            border-radius: 18px;
        }
        QFrame#topSearchBar:hover { background: #2a2a2a; border-color: #2a2a2a; }
        QLineEdit#globalSearchEdit {
            background: transparent;
            color: #ffffff;
            border: none;
            min-height: 0;
            padding: 0 4px;
        }
        QPushButton#searchActionButton {
            background: transparent;
            border: none;
            border-radius: 15px;
            padding: 2px;
        }
        QPushButton#searchActionButton:hover { background: #3e3e3e; }
        QPushButton#searchActionButton:disabled { background: transparent; }
        QToolButton#navButton {
            border: none;
            border-radius: 7px;
            padding: 8px 10px;
            text-align: left;
        }
        QToolButton#navButton:hover { background: #1a1a1a; }
        QToolButton#navButton:checked { background: #1a1a1a; color: #ffffff; }
        QToolButton#transportIconButton {
            background: transparent;
            border: none;
            padding: 0;
        }
        QToolButton#transportIconButton:hover,
        QToolButton#transportIconButton:pressed { background: transparent; }
        QToolButton#playButton {
            background: #ffffff;
            border: none;
            border-radius: 19px;
            padding: 0;
        }
        QToolButton#playButton:hover { background: #e6e6e6; }
        QToolButton#playButton:disabled { background: #b3b3b3; }
        QToolButton#repeatButton {
            background: transparent;
            border: none;
            padding: 0;
        }
        QToolButton#repeatButton:hover,
        QToolButton#repeatButton:checked { background: transparent; }
        QToolButton#volumeMuteButton {
            background: transparent;
            border: none;
            padding: 0;
        }
        QToolButton#volumeMuteButton:hover { background: #282828; border-radius: 5px; }
        QToolButton#volumeMuteButton:disabled { background: transparent; }
        QToolButton#lyricsButton {
            color: #b3b3b3;
            background: transparent;
            border: none;
            border-radius: 5px;
            padding: 0;
        }
        QToolButton#lyricsButton:hover { background: #282828; color: #ffffff; }
        QToolButton#lyricsButton:checked,
        QToolButton#lyricsButton:checked:hover { background: transparent; color: #1ed760; }
        QToolButton#deviceButton { background: transparent; border: none; padding: 0; }
        QToolButton#deviceButton:hover { background: #282828; border-radius: 5px; }
        QToolButton#deviceButton:disabled { background: transparent; }
        QToolButton#nowPlayingPanelButton {
            color: #b3b3b3;
            background: transparent;
            border: none;
            border-radius: 5px;
        }
        QToolButton#nowPlayingPanelButton:hover,
        QToolButton#nowPlayingPanelButton:checked { background: #282828; color: #1ed760; }
        QPushButton#primaryButton {
            background: #1ed760;
            color: #000000;
            border: none;
            border-radius: 7px;
            padding: 8px 12px;
            font-weight: 600;
        }
        QPushButton#primaryButton:hover { background: #3be477; }
        QPushButton#primaryButton:pressed { background: #1fbe56; }
        QPushButton#primaryButton:disabled {
            background: #535353;
            color: #b3b3b3;
        }
        QPushButton {
            background: #282828;
            color: #ffffff;
            border: none;
            padding: 7px 10px;
            border-radius: 6px;
        }
        QPushButton:hover { background: #3e3e3e; }
        QPushButton:disabled { background: #282828; color: #6a6a6a; }
        QListWidget#playlistList { background: transparent; border: none; outline: none; }
        QListWidget#playlistList::item { padding: 7px 8px; border-radius: 6px; color: #b3b3b3; }
        QListWidget#playlistList::item:hover { background: #1a1a1a; }
        QListWidget#playlistList::item:selected { background: #282828; color: #ffffff; }
        QListWidget#searchArtistResults,
        QListWidget#artistReleases { background: transparent; border: none; outline: none; }
        QListWidget#searchArtistResults::item {
            color: #b3b3b3;
            padding: 3px;
            border-radius: 6px;
        }
        QListWidget#searchArtistResults::item:hover,
        QListWidget#searchArtistResults::item:selected { background: #282828; color: #ffffff; }
        QListWidget#artistReleases::item { padding: 1px; border-radius: 6px; }
        QListWidget#artistReleases::item:hover,
        QListWidget#artistReleases::item:selected { background: #282828; }
        QLineEdit, QSpinBox {
            background: #282828;
            color: #ffffff;
            border: 1px solid #3e3e3e;
            min-height: 34px;
            padding: 5px 10px;
            border-radius: 6px;
        }
        QLineEdit#spotifyClientId {
            min-height: 48px;
            padding: 0px 10px;
        }
        QSpinBox#spotifyCallbackPort, QSpinBox#artworkCacheSizeMb {
            min-height: 48px;
            padding: 0px 10px;
        }
        QSpinBox#spotifyCallbackPort QLineEdit,
        QSpinBox#artworkCacheSizeMb QLineEdit {
            min-height: 0px;
            padding: 0px;
            margin: 0px;
            border: none;
            background: transparent;
        }
        QLabel#accountAvatar {
            color: #b3b3b3;
            background: #282828;
            border-radius: 14px;
            font-weight: 600;
        }
        QLineEdit:focus, QSpinBox:focus { border-color: #b3b3b3; }
        QTableView {
            background: #181818;
            color: #ffffff;
            border: none;
            selection-background-color: #282828;
            selection-color: #ffffff;
            outline: none;
        }
        QTableView::item { padding: 2px 4px; border: none; }
        QTableView::item:hover { background: #292929; }
        QScrollBar:vertical {
            background: transparent;
            width: 8px;
            margin: 0;
            border: none;
        }
        QScrollBar::handle:vertical {
            background: transparent;
            min-height: 28px;
            border-radius: 4px;
        }
        QScrollBar[sectionHovered="true"]::handle:vertical { background: #535353; }
        QScrollBar[sectionHovered="true"]::handle:vertical:hover { background: #b3b3b3; }
        QScrollBar[sectionHovered="true"]::handle:vertical:pressed { background: #1ed760; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0;
            border: none;
            background: transparent;
        }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
            background: transparent;
        }
        QScrollBar:horizontal {
            background: transparent;
            height: 8px;
            margin: 0;
            border: none;
        }
        QScrollBar::handle:horizontal {
            background: transparent;
            min-width: 28px;
            border-radius: 4px;
        }
        QScrollBar[sectionHovered="true"]::handle:horizontal { background: #535353; }
        QScrollBar[sectionHovered="true"]::handle:horizontal:hover { background: #b3b3b3; }
        QScrollBar[sectionHovered="true"]::handle:horizontal:pressed { background: #1ed760; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0;
            border: none;
            background: transparent;
        }
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
            background: transparent;
        }
        QAbstractScrollArea::corner { background: transparent; }
        QHeaderView::section {
            background: #181818;
            color: #b3b3b3;
            border: none;
            border-bottom: 1px solid #282828;
            padding: 7px 4px;
        }
        QSlider#playbackProgressSlider, QSlider#playbackVolumeSlider { background: transparent; }
        QSlider#playbackProgressSlider::groove:horizontal,
        QSlider#playbackVolumeSlider::groove:horizontal {
            height: 4px;
            background: #4d4d4d;
            border-radius: 2px;
        }
        QSlider#playbackProgressSlider::handle:horizontal,
        QSlider#playbackVolumeSlider::handle:horizontal {
            width: 10px;
            height: 10px;
            margin: -4px 0;
            border: none;
            border-radius: 5px;
            background: transparent;
        }
        QSlider#playbackProgressSlider::sub-page:horizontal,
        QSlider#playbackVolumeSlider::sub-page:horizontal {
            background: transparent;
            border: none;
        }
        QSlider#playbackProgressSlider::add-page:horizontal,
        QSlider#playbackVolumeSlider::add-page:horizontal {
            background: transparent;
            border: none;
        }
    )");

    const QByteArray savedWindowGeometry = appSettings.value(
        QStringLiteral("window/geometry")).toByteArray();
    if (!savedWindowGeometry.isEmpty()) {
        restoreGeometry(savedWindowGeometry);
    }
    const bool nowPlayingPanelVisible = appSettings.value(
        QStringLiteral("window/nowPlayingPanelVisible"), true).toBool();
    {
        const QSignalBlocker signalBlocker(nowPlayingPanelButton_);
        nowPlayingPanelButton_->setChecked(nowPlayingPanelVisible);
    }
    nowPlayingPanel_->setVisible(nowPlayingPanelVisible);
    const QByteArray savedSplitterState = appSettings.value(
        QStringLiteral("window/mainSplitter")).toByteArray();
    if (!savedSplitterState.isEmpty()) {
        bodySplitter_->restoreState(savedSplitterState);
    }

    restoreSavedPlayback();
    const QString restoredClientId = spotifyConnection_->restoreSession();
    if (!restoredClientId.isEmpty()) {
        spotifyClientIdEdit_->setText(restoredClientId);
        QSettings settings;
        settings.setValue("spotify/clientId", restoredClientId);
        startupWindowPending_ = true;
        QTimer::singleShot(30000, this, [this] { showStartupWindow(); });
    } else {
        show();
    }
    reauthorizeSpotifyButton_->setEnabled(!spotifyClientIdEdit_->text().trimmed().isEmpty());
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && maximizeButton_) {
        maximizeButton_->setText(isMaximized() ? QStringLiteral("❐")
                                               : QStringLiteral("□"));
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    captureCurrentNavigationPosition();
    QSettings settings;
    const int pageIndex = pageStack_ ? pageStack_->currentIndex() : homePageIndex;
    settings.setValue(QStringLiteral("navigation/lastPage"), pageIndex);

    QString playlistId;
    QString playlistName;
    if (pageIndex == libraryPageIndex) {
        playlistId = activeLibraryPlaylistId_;
        playlistName = activeLibraryPlaylistName_;
        if (!spotifyConnection_ || !spotifyConnection_->isConnected()) {
            playlistId = settings.value(QStringLiteral("navigation/playlistId")).toString();
            playlistName = settings.value(QStringLiteral("navigation/playlistName"),
                                          QStringLiteral("Liked Songs")).toString();
        }
    }
    settings.setValue(QStringLiteral("navigation/playlistId"), playlistId);
    settings.setValue(QStringLiteral("navigation/playlistName"), playlistName);
    settings.setValue(QStringLiteral("navigation/searchQuery"),
                      pageIndex == searchPageIndex && searchEdit_
                          ? searchEdit_->text().trimmed() : QString{});
    settings.setValue(QStringLiteral("navigation/artistId"),
                      pageIndex == artistPageIndex ? currentArtistPageId_ : QString{});
    settings.setValue(QStringLiteral("navigation/artistName"),
                      pageIndex == artistPageIndex ? currentArtistPageName_ : QString{});
    settings.setValue(QStringLiteral("navigation/albumId"),
                      pageIndex == albumPageIndex ? currentAlbumPageId_ : QString{});
    settings.setValue(QStringLiteral("navigation/albumName"),
                      pageIndex == albumPageIndex ? currentAlbumPageName_ : QString{});

    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    if (bodySplitter_) {
        settings.setValue(QStringLiteral("window/mainSplitter"), bodySplitter_->saveState());
    }
    if (nowPlayingPanelButton_) {
        settings.setValue(QStringLiteral("window/nowPlayingPanelVisible"),
                          nowPlayingPanelButton_->isChecked());
    }
    if (playlistFilterEdit_) {
        settings.setValue(QStringLiteral("window/libraryFilter"), playlistFilterEdit_->text());
    }
    if (libraryTracksTable_) {
        settings.setValue(QStringLiteral("window/libraryScroll"),
                          libraryTracksTable_->verticalScrollBar()->value());
    }
    if (searchResultsTable_) {
        settings.setValue(QStringLiteral("window/searchScroll"),
                          searchResultsTable_->verticalScrollBar()->value());
    }
    if (playlistsList_) {
        settings.setValue(QStringLiteral("window/librarySidebarScroll"),
                          playlistsList_->verticalScrollBar()->value());
    }
    settings.setValue(QStringLiteral("window/artistPageScroll"), artistPageScrollPosition_);
    settings.setValue(QStringLiteral("window/artistReleaseScroll"),
                      artistReleaseListScrollPosition_);
    settings.setValue(QStringLiteral("window/albumPageScroll"), albumPageScrollPosition_);

    settings.setValue(QStringLiteral("playback/active"),
                      playbackState_.active && !playbackState_.trackUri.isEmpty());
    settings.setValue(QStringLiteral("playback/trackUri"), playbackState_.trackUri);
    settings.setValue(QStringLiteral("playback/trackName"), playbackState_.trackName);
    settings.setValue(QStringLiteral("playback/artists"), playbackState_.artists);
    settings.setValue(QStringLiteral("playback/artistId"), playbackState_.artistId);
    settings.setValue(QStringLiteral("playback/album"), playbackState_.album);
    settings.setValue(QStringLiteral("playback/albumId"), playbackState_.albumId);
    settings.setValue(QStringLiteral("playback/artworkUrl"), playbackState_.artworkUrl);
    settings.setValue(QStringLiteral("playback/durationMs"), playbackState_.durationMs);
    settings.setValue(QStringLiteral("playback/positionMs"), playbackState_.positionMs);
    settings.setValue(QStringLiteral("playback/sourceName"), playbackSourceName_);
    settings.setValue(QStringLiteral("playback/sourcePlaylistId"), playbackSourcePlaylistId_);
    settings.setValue(QStringLiteral("playback/sourceAlbumId"), playbackSourceAlbumId_);
    settings.sync();
    QMainWindow::closeEvent(event);
}

void MainWindow::updateSpotifyConnectionUi(const QString &message, bool connected, bool busy)
{
    if (!connected && likedSongsQueueActive_) {
        stopLikedSongsPlayback();
    }
    if (startupWindowPending_ && !connected && !busy) {
        showStartupWindow();
    }

    if (connected) {
        spotifyStatusLabel_->hide();
        const QString displayName = spotifyConnection_->currentUserDisplayName();
        connectionDescription_->setText(displayName.isEmpty()
            ? message + QStringLiteral(". Spotify profile details are still loading.")
            : message + QStringLiteral(". Your Spotify profile loaded successfully."));
        if (!displayName.isEmpty()) {
            spotifyProfileNameLabel_->setText(displayName);
        }
        const QString profileImageUrl = spotifyConnection_->currentUserImageUrl();
        spotifyHeaderAvatarLabel_->setToolTip(displayName.isEmpty()
            ? QStringLiteral("Connected to Spotify")
            : QStringLiteral("Connected as %1").arg(displayName));
        if (profileImageUrl != displayedHeaderAvatarUrl_) {
            displayedHeaderAvatarUrl_ = profileImageUrl;
            ++headerAvatarRequestId_;
            const quint64 requestId = headerAvatarRequestId_;
            spotifyHeaderAvatarLabel_->setPixmap({});
            spotifyHeaderAvatarLabel_->setText(displayName.isEmpty()
                ? QStringLiteral("♪") : displayName.left(1).toUpper());
            if (!profileImageUrl.isEmpty()) {
                QPointer<QLabel> headerAvatarGuard(spotifyHeaderAvatarLabel_);
                spotifyConnection_->requestImage(profileImageUrl,
                    [this, headerAvatarGuard, requestId](const QImage &image) {
                    if (!headerAvatarGuard || image.isNull()
                        || requestId != headerAvatarRequestId_) {
                        return;
                    }
                    headerAvatarGuard->setText({});
                    headerAvatarGuard->setPixmap(circularAvatarPixmap(
                        image, headerAvatarGuard->size()));
                });
            }
        }
        if (!profileImageUrl.isEmpty()) {
            spotifyHeaderAvatarLabel_->setToolTip(displayName.isEmpty()
                ? QStringLiteral("Spotify account")
                : QStringLiteral("Spotify account: %1").arg(displayName));
        }
        if (profileImageUrl != displayedProfileArtworkUrl_) {
            displayedProfileArtworkUrl_ = profileImageUrl;
            ++profileArtworkRequestId_;
            const quint64 requestId = profileArtworkRequestId_;
            spotifyProfileAvatarLabel_->setPixmap({});
            spotifyProfileAvatarLabel_->setText(QStringLiteral("♪"));
            if (!profileImageUrl.isEmpty()) {
                QPointer<QLabel> avatarGuard(spotifyProfileAvatarLabel_);
                spotifyConnection_->requestImage(profileImageUrl,
                    [this, avatarGuard, requestId](const QImage &image) {
                    if (!avatarGuard || image.isNull() || requestId != profileArtworkRequestId_) {
                        return;
                    }
                    avatarGuard->setText({});
                    avatarGuard->setPixmap(QPixmap::fromImage(image).scaled(
                        avatarGuard->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
                });
            }
        }
    } else if (busy) {
        spotifyStatusLabel_->setText(message);
        spotifyStatusLabel_->show();
        spotifyHeaderAvatarLabel_->setPixmap({});
        spotifyHeaderAvatarLabel_->setText(QStringLiteral("♪"));
        spotifyHeaderAvatarLabel_->setToolTip(message);
        connectionDescription_->setText("Finish authorizing MusicPlayer in the Spotify browser window.");
    } else {
        spotifyStatusLabel_->setText(message);
        spotifyStatusLabel_->show();
        spotifyHeaderAvatarLabel_->setPixmap({});
        spotifyHeaderAvatarLabel_->setText(QStringLiteral("♪"));
        spotifyHeaderAvatarLabel_->setToolTip(message);
        connectionDescription_->setText(
            "Sign in securely with Spotify to link your account and load your profile.");
    }

    if (!connected) {
        displayedHeaderAvatarUrl_.clear();
        ++headerAvatarRequestId_;
        displayedProfileArtworkUrl_.clear();
        ++profileArtworkRequestId_;
        spotifyProfileAvatarLabel_->setPixmap({});
        spotifyProfileAvatarLabel_->setText(QStringLiteral("♪"));
        spotifyProfileNameLabel_->setText(QStringLiteral("Spotify profile"));
        searchResultsModel_->clearArtworkCache();
        libraryTracksModel_->clearArtworkCache();
    }

    connectSpotifyButton_->setEnabled(!busy);
    if (busy) {
        connectSpotifyButton_->setText("Connecting…");
    } else if (connected) {
        connectSpotifyButton_->setText("Disconnect Spotify");
    } else {
        connectSpotifyButton_->setText("Connect Spotify");
    }

    searchEdit_->setEnabled(connected);
    searchButton_->setEnabled(connected && !searchBusy_);
    playlistFilterEdit_->setEnabled(connected);
    searchResultsTable_->setEnabled(connected);
    playlistsList_->setEnabled(connected);
    libraryTracksTable_->setEnabled(connected);
    libraryRefreshButton_->setEnabled(connected);
    reauthorizeSpotifyButton_->setEnabled(!busy && !spotifyClientIdEdit_->text().trimmed().isEmpty());
    if (connected && !libraryLoadedForSession_) {
        if (pageStack_->currentIndex() == libraryPageIndex) {
            QSettings settings;
            activeLibraryPlaylistId_ = settings.value(QStringLiteral("navigation/playlistId"))
                                           .toString();
            activeLibraryPlaylistName_ = settings.value(QStringLiteral("navigation/playlistName"),
                                                        QStringLiteral("Liked Songs")).toString();
            if (activeLibraryPlaylistName_.isEmpty()) {
                activeLibraryPlaylistName_ = QStringLiteral("Liked Songs");
            }
            librarySummaryText_ = QStringLiteral("Loading your saved music…");
            updateLibraryHeader();
        }
        libraryLoadedForSession_ = true;
        refreshPlaylistSidebar();
        const bool loadLibraryOnConnect = pageStack_->currentIndex() == libraryPageIndex;
        if (loadLibraryOnConnect && !activeLibraryPlaylistId_.isEmpty()) {
            loadLibraryTracks(0, false);
        }
        const quint64 libraryRequestId = libraryRequestId_;
        const QString currentTrackUri = playbackState_.active
            ? playbackState_.trackUri : QString{};
        spotifyConnection_->checkSavedTracksChanged(
            currentTrackUri, [this, loadLibraryOnConnect, libraryRequestId](
                                 bool changed, const QString &) {
                if (!loadLibraryOnConnect || !spotifyConnection_->isConnected()
                    || pageStack_->currentIndex() != libraryPageIndex
                    || libraryRequestId_ != libraryRequestId
                    || !activeLibraryPlaylistId_.isEmpty()) {
                    return;
                }
                loadLibraryTracks(0, false, changed, false);
            });
        if (pageStack_->currentIndex() == searchPageIndex
            && !searchEdit_->text().trimmed().isEmpty()) {
            performSearch();
        }
    }
    if (connected && pageStack_->currentIndex() == artistPageIndex
        && !currentArtistPageId_.isEmpty() && !artistProfileLoaded_
        && !artistReleaseRequestActive_) {
        loadArtistPage();
    }
    if (connected && pageStack_->currentIndex() == albumPageIndex
        && !currentAlbumPageId_.isEmpty() && !albumTrackRequestActive_
        && albumTracksModel_->loadedTrackCount() == 0) {
        loadAlbumPage();
    }
    if (!connected) {
        ++searchRequestId_;
        searchBusy_ = false;
        searchStatusLabel_->setText("Connect Spotify to search the catalog.");

        libraryLoadedForSession_ = false;
        ++artistPageRequestId_;
        artistProfileLoaded_ = false;
        artistReleaseRequestActive_ = false;
        ++albumPageRequestId_;
        albumTrackRequestActive_ = false;
        ++libraryRequestId_;
        ++playlistRequestId_;
        libraryBusy_ = false;
        activeLibraryPageOffset_ = -1;
        pendingLibraryPageOffsets_.clear();
        cachedLibraryPageOffsets_.clear();
        forceNetworkLibraryPageOffsets_.clear();
        activeLibraryPlaylistId_.clear();
        activeLibraryPlaylistName_ = QStringLiteral("Liked Songs");
        activeLibraryCreator_ = QStringLiteral("Your saved tracks");
        activeLibraryArtworkUrl_.clear();
        librarySummaryText_ = busy ? QStringLiteral("Connecting to Spotify…")
                                   : QStringLiteral("Connect Spotify to load your music.");
        updateLibraryHeader();
        playlistStatusLabel_->setText(busy ? QStringLiteral("Connecting to Spotify…")
                                           : QStringLiteral("Connect Spotify to load your library."));
        playlistStatusLabel_->setVisible(!busy);
        playlistsList_->clear();
        addLikedSongsSidebarItem();
        playlistFilterEdit_->setEnabled(false);
        libraryTracksModel_->clearRows();
        requestedLibraryArtworkUrl_.clear();
        ++libraryArtworkRequestId_;
        if (auto *table = dynamic_cast<TrackTableView *>(libraryTracksTable_)) {
            table->setLibraryArtwork({});
        }
        libraryTracksTable_->setEnabled(false);
        libraryTotalTracks_ = 0;
        libraryRefreshButton_->setEnabled(false);
        if (!busy) {
            playbackState_ = {};
            updatePlaybackUi(playbackState_);
        }
    } else {
        updatePlaybackUi(playbackState_);
    }
}

void MainWindow::refreshPlaylistSidebar(bool forceNetwork)
{
    ++playlistRequestId_;
    const quint64 requestId = playlistRequestId_;
    playlistsList_->clear();
    addLikedSongsSidebarItem();
    if (!spotifyConnection_->isConnected()) {
        playlistStatusLabel_->setText("Connect Spotify to load your library.");
        playlistStatusLabel_->setVisible(true);
        return;
    }

    playlistStatusLabel_->setText("Loading your library…");
    playlistStatusLabel_->setVisible(true);
    spotifyConnection_->fetchCurrentUserLibraryItems(
        [this, requestId](const QList<SpotifyLibraryItem> &items, const QString &error) {
            if (requestId != playlistRequestId_ || !spotifyConnection_->isConnected()) {
                return;
            }
            playlistsList_->clear();
            addLikedSongsSidebarItem();
            for (const SpotifyLibraryItem &libraryItem : items) {
                addLibrarySidebarItem(libraryItem);
            }
            if (!activeLibraryPlaylistId_.isEmpty()) {
                for (int row = 0; row < playlistsList_->count(); ++row) {
                    QListWidgetItem *item = playlistsList_->item(row);
                    if (item->data(libraryItemTypeRole).toInt()
                            == static_cast<int>(SpotifyLibraryItemType::Playlist)
                        && item->data(Qt::UserRole).toString() == activeLibraryPlaylistId_) {
                        playlistsList_->setCurrentItem(item);
                        break;
                    }
                }
            } else {
                playlistsList_->setCurrentItem(likedSongsItem_);
            }
            filterPlaylists(playlistFilterEdit_->text());
            if (restorePlaylistSidebarScrollPending_) {
                restorePlaylistSidebarScrollPending_ = false;
                const int scrollPosition = savedPlaylistSidebarScrollPosition_;
                QTimer::singleShot(0, this, [this, scrollPosition] {
                    playlistsList_->verticalScrollBar()->setValue(scrollPosition);
                });
            }
            updateActiveLibraryMetadata();
            if (!error.isEmpty()) {
                playlistStatusLabel_->setText(items.isEmpty()
                    ? QStringLiteral("Could not load your library.")
                    : QStringLiteral("Some library items could not be loaded."));
                playlistStatusLabel_->setToolTip(error);
                playlistStatusLabel_->setVisible(true);
            } else {
                playlistStatusLabel_->setToolTip({});
                playlistStatusLabel_->setText(items.isEmpty()
                    ? QStringLiteral("No saved albums, episodes, shows, or playlists.")
                    : QString{});
                playlistStatusLabel_->setVisible(items.isEmpty());
            }
        }, forceNetwork);
}

void MainWindow::addLikedSongsSidebarItem()
{
    if (!playlistsList_) {
        return;
    }
    auto *item = new QListWidgetItem(QStringLiteral("Liked Songs"), playlistsList_);
    likedSongsItem_ = item;
    item->setData(Qt::UserRole, QString{});
    item->setData(libraryItemTypeRole, static_cast<int>(SpotifyLibraryItemType::LikedSongs));
    item->setData(libraryItemCategoryRole, QStringLiteral("Playlist"));
    item->setData(libraryItemCreatorRole, QStringLiteral("Your saved tracks"));
    item->setIcon(likedSongsIcon());
    item->setToolTip(QStringLiteral("Liked Songs · Your saved tracks"));
    item->setSizeHint(QSize(0, 60));
}

void MainWindow::addLibrarySidebarItem(const SpotifyLibraryItem &libraryItem)
{
    auto *item = new QListWidgetItem(libraryItem.name, playlistsList_);
    item->setData(Qt::UserRole, libraryItem.id);
    item->setData(libraryItemTypeRole, static_cast<int>(libraryItem.type));
    item->setData(libraryItemCreatorRole, libraryItem.creator);
    item->setData(libraryItemCategoryRole, libraryItem.category);
    item->setData(libraryItemUriRole, libraryItem.spotifyUri);
    item->setData(libraryItemUrlRole, libraryItem.spotifyUrl);
    item->setData(libraryItemArtworkUrlRole, libraryItem.imageUrl);
    item->setToolTip(libraryItem.creator.isEmpty()
        ? libraryItem.name
        : QStringLiteral("%1\n%2 · %3")
              .arg(libraryItem.name, libraryItem.category, libraryItem.creator));
    item->setSizeHint(QSize(0, 60));

    if (!libraryItem.imageUrl.isEmpty()) {
        const QString itemId = libraryItem.id;
        const int itemType = static_cast<int>(libraryItem.type);
        const quint64 requestId = playlistRequestId_;
        spotifyConnection_->requestImage(libraryItem.imageUrl,
            [this, requestId, itemId, itemType](const QImage &image) {
            if (image.isNull() || requestId != playlistRequestId_) {
                return;
            }
            for (int row = 0; row < playlistsList_->count(); ++row) {
                QListWidgetItem *candidate = playlistsList_->item(row);
                if (candidate->data(Qt::UserRole).toString() == itemId
                    && candidate->data(libraryItemTypeRole).toInt() == itemType) {
                    candidate->setIcon(QIcon(QPixmap::fromImage(image).scaled(
                        QSize(42, 42), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                    break;
                }
            }
        });
    }
}

void MainWindow::performSearch()
{
    closeLyricsView();
    const QString query = searchEdit_->text().trimmed();
    recordNavigation({searchPageIndex, {}, {}, query});
    pageStack_->setCurrentIndex(searchPageIndex);
    searchResultsModel_->clearRows();
    searchArtistResultsList_->clear();
    searchArtistResultsList_->hide();
    searchArtistResultsHeading_->hide();
    if (query.isEmpty()) {
        ++searchRequestId_;
        searchBusy_ = false;
        searchButton_->setEnabled(spotifyConnection_->isConnected());
        searchStatusLabel_->setText("Enter a track, artist, or album name.");
        return;
    }

    const quint64 requestId = ++searchRequestId_;
    searchBusy_ = true;
    searchButton_->setEnabled(false);
    searchStatusLabel_->setText("Searching Spotify…");
    spotifyConnection_->searchCatalog(query,
        [this, requestId](const QList<SpotifyTrackResult> &tracks,
                          const QList<SpotifyArtistProfile> &artists,
                          const QString &error) {
            if (requestId != searchRequestId_) {
                return;
            }

            searchBusy_ = false;
            searchButton_->setEnabled(spotifyConnection_->isConnected());
            if (!error.isEmpty()) {
                searchStatusLabel_->setText(error);
                return;
            }

            for (const SpotifyArtistProfile &artist : artists) {
                auto *item = new QListWidgetItem(artist.name, searchArtistResultsList_);
                item->setData(Qt::UserRole, artist.id);
                item->setData(searchArtistImageUrlRole, artist.imageUrl);
                item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
                item->setToolTip(artist.name);
                if (!artist.imageUrl.isEmpty()) {
                    spotifyConnection_->requestImage(artist.imageUrl,
                        [this, requestId, artistId = artist.id](const QImage &image) {
                        if (requestId != searchRequestId_ || image.isNull()) return;
                        for (int row = 0; row < searchArtistResultsList_->count(); ++row) {
                            QListWidgetItem *candidate = searchArtistResultsList_->item(row);
                            if (candidate->data(Qt::UserRole).toString() == artistId) {
                                candidate->setIcon(QIcon(QPixmap::fromImage(image).scaled(
                                    QSize(64, 64), Qt::KeepAspectRatioByExpanding,
                                    Qt::SmoothTransformation)));
                                break;
                            }
                        }
                    });
                }
            }
            const bool hasArtists = !artists.isEmpty();
            searchArtistResultsHeading_->setVisible(hasArtists);
            searchArtistResultsList_->setVisible(hasArtists);

            searchResultsModel_->setSearchTracks(tracks);
            updateTrackTablePlaybackState(searchResultsModel_);
            lastPlaybackSelectionPage_ = -1;
            updatePlaybackUi(playbackState_);
            if (restoreSearchScrollPending_) {
                restoreSearchScrollPending_ = false;
                const int scrollPosition = savedSearchScrollPosition_;
                QTimer::singleShot(0, this, [this, scrollPosition] {
                    searchResultsTable_->verticalScrollBar()->setValue(scrollPosition);
                });
            }
            if (tracks.isEmpty() && artists.isEmpty()) {
                searchStatusLabel_->setText(QStringLiteral("No artists or tracks found."));
            } else {
                searchStatusLabel_->setText(
                    QStringLiteral("%1 artists · %2 tracks found. Double-click a track to play it.")
                        .arg(artists.size()).arg(tracks.size()));
            }
        });
}

void MainWindow::navigateToPage(int pageIndex)
{
    if (pageIndex == libraryPageIndex) {
        showSavedTracks();
        return;
    }
    if (pageIndex < homePageIndex || pageIndex > settingsPageIndex) {
        return;
    }
    closeLyricsView();
    NavigationEntry entry;
    entry.pageIndex = pageIndex;
    if (pageIndex == searchPageIndex) {
        entry.searchQuery = searchEdit_->text().trimmed();
    }
    recordNavigation(entry);
    pageStack_->setCurrentIndex(pageIndex);
}

void MainWindow::navigateToEntry(const NavigationEntry &entry)
{
    closeLyricsView();
    restoringNavigation_ = true;
    restoreNavigationScroll(entry);
    if (entry.pageIndex == artistPageIndex) {
        showArtistPage(entry.artistId, entry.artistName);
        saveLastNavigation(entry);
        restoringNavigation_ = false;
        updateHistoryButtons();
        return;
    }
    if (entry.pageIndex == albumPageIndex) {
        showAlbumPage(entry.albumId, entry.albumName);
        saveLastNavigation(entry);
        restoringNavigation_ = false;
        updateHistoryButtons();
        return;
    }
    pageStack_->setCurrentIndex(entry.pageIndex);
    saveLastNavigation(entry);
    if (entry.pageIndex == libraryPageIndex) {
        activeLibraryPlaylistId_ = entry.playlistId;
        activeLibraryPlaylistName_ = entry.playlistName.isEmpty()
            ? QStringLiteral("Liked Songs") : entry.playlistName;
        librarySummaryText_ = QStringLiteral("Loading your saved music…");
        updateLibraryHeader();
        playlistsList_->clearSelection();
        bool selected = false;
        for (int index = 0; index < playlistsList_->count(); ++index) {
            QListWidgetItem *item = playlistsList_->item(index);
            const auto type = static_cast<SpotifyLibraryItemType>(
                item->data(libraryItemTypeRole).toInt());
            if (entry.playlistId.isEmpty() && type == SpotifyLibraryItemType::LikedSongs) {
                playlistsList_->setCurrentItem(item);
                selected = true;
                break;
            }
            if (!entry.playlistId.isEmpty() && type == SpotifyLibraryItemType::Playlist
                && item->data(Qt::UserRole).toString() == entry.playlistId) {
                playlistsList_->setCurrentItem(item);
                selected = true;
                break;
            }
        }
        if (!selected) playlistsList_->clearSelection();
        updateActiveLibraryMetadata();
        loadLibraryTracks(0, false);
    } else if (entry.pageIndex == searchPageIndex) {
        searchEdit_->setText(entry.searchQuery);
        performSearch();
    }
    restoringNavigation_ = false;
    updateHistoryButtons();
}

void MainWindow::captureCurrentNavigationPosition()
{
    if (navigationHistoryIndex_ < 0 || navigationHistoryIndex_ >= navigationHistory_.size()) {
        return;
    }
    NavigationEntry &entry = navigationHistory_[navigationHistoryIndex_];
    switch (entry.pageIndex) {
    case searchPageIndex:
        if (searchResultsTable_) {
            entry.primaryScrollPosition = searchResultsTable_->verticalScrollBar()->value();
        }
        break;
    case libraryPageIndex:
        if (libraryTracksTable_) {
            entry.primaryScrollPosition = libraryTracksTable_->verticalScrollBar()->value();
        }
        break;
    case artistPageIndex:
        if (artistPageScroll_) {
            entry.primaryScrollPosition = artistPageScroll_->verticalScrollBar()->value();
        }
        if (artistReleasesList_) {
            entry.secondaryScrollPosition = artistReleasesList_->verticalScrollBar()->value();
        }
        break;
    case albumPageIndex:
        if (albumTracksTable_) {
            entry.primaryScrollPosition = albumTracksTable_->verticalScrollBar()->value();
        }
        break;
    default:
        break;
    }
}

void MainWindow::restoreNavigationScroll(const NavigationEntry &entry)
{
    switch (entry.pageIndex) {
    case searchPageIndex:
        savedSearchScrollPosition_ = qMax(0, entry.primaryScrollPosition);
        restoreSearchScrollPending_ = !entry.searchQuery.trimmed().isEmpty();
        break;
    case libraryPageIndex:
        savedLibraryScrollPosition_ = qMax(0, entry.primaryScrollPosition);
        restoreLibraryScrollPending_ = true;
        break;
    case artistPageIndex: {
        artistPageScrollPosition_ = qMax(0, entry.primaryScrollPosition);
        artistReleaseListScrollPosition_ = qMax(0, entry.secondaryScrollPosition);
        restoreArtistScrollPending_ = true;
        break;
    }
    case albumPageIndex:
        albumPageScrollPosition_ = qMax(0, entry.primaryScrollPosition);
        restoreAlbumScrollPending_ = true;
        break;
    default:
        break;
    }
}

void MainWindow::recordNavigation(const NavigationEntry &entry)
{
    if (restoringNavigation_) {
        return;
    }
    captureCurrentNavigationPosition();
    saveLastNavigation(entry);
    if (navigationHistoryIndex_ >= 0
        && navigationHistory_.at(navigationHistoryIndex_).pageIndex == entry.pageIndex
        && navigationHistory_.at(navigationHistoryIndex_).playlistId == entry.playlistId
        && navigationHistory_.at(navigationHistoryIndex_).playlistName == entry.playlistName
        && navigationHistory_.at(navigationHistoryIndex_).searchQuery == entry.searchQuery
        && navigationHistory_.at(navigationHistoryIndex_).artistId == entry.artistId
        && navigationHistory_.at(navigationHistoryIndex_).artistName == entry.artistName
        && navigationHistory_.at(navigationHistoryIndex_).albumId == entry.albumId) {
        return;
    }
    if (navigationHistoryIndex_ + 1 < navigationHistory_.size()) {
        navigationHistory_.resize(navigationHistoryIndex_ + 1);
    }
    navigationHistory_.append(entry);
    navigationHistoryIndex_ = navigationHistory_.size() - 1;
    updateHistoryButtons();
}

void MainWindow::saveLastNavigation(const NavigationEntry &entry)
{
    QSettings settings;
    settings.setValue(QStringLiteral("navigation/lastPage"), entry.pageIndex);
    settings.setValue(QStringLiteral("navigation/playlistId"), entry.playlistId);
    settings.setValue(QStringLiteral("navigation/playlistName"), entry.playlistName);
    settings.setValue(QStringLiteral("navigation/searchQuery"), entry.searchQuery);
    settings.setValue(QStringLiteral("navigation/artistId"), entry.artistId);
    settings.setValue(QStringLiteral("navigation/artistName"), entry.artistName);
    settings.setValue(QStringLiteral("navigation/albumId"), entry.albumId);
    settings.setValue(QStringLiteral("navigation/albumName"), entry.albumName);
}

void MainWindow::goBack()
{
    if (navigationHistoryIndex_ <= 0) {
        return;
    }
    captureCurrentNavigationPosition();
    --navigationHistoryIndex_;
    navigateToEntry(navigationHistory_.at(navigationHistoryIndex_));
}

void MainWindow::goForward()
{
    if (navigationHistoryIndex_ + 1 >= navigationHistory_.size()) {
        return;
    }
    captureCurrentNavigationPosition();
    ++navigationHistoryIndex_;
    navigateToEntry(navigationHistory_.at(navigationHistoryIndex_));
}

void MainWindow::updateHistoryButtons()
{
    if (backButton_) {
        backButton_->setEnabled(navigationHistoryIndex_ > 0);
    }
    if (forwardButton_) {
        forwardButton_->setEnabled(navigationHistoryIndex_ + 1 < navigationHistory_.size());
    }
}

void MainWindow::filterPlaylists(const QString &query)
{
    const QString foldedQuery = query.trimmed().toCaseFolded();
    for (int index = 0; index < playlistsList_->count(); ++index) {
        QListWidgetItem *item = playlistsList_->item(index);
        const auto type = static_cast<SpotifyLibraryItemType>(
            item->data(libraryItemTypeRole).toInt());
        if (type == SpotifyLibraryItemType::LikedSongs) {
            item->setHidden(false);
            continue;
        }
        const QString searchableText = item->text() + QLatin1Char(' ')
            + item->data(libraryItemCreatorRole).toString() + QLatin1Char(' ')
            + item->data(libraryItemCategoryRole).toString();
        item->setHidden(!searchableText.toCaseFolded().contains(foldedQuery));
    }
}

void MainWindow::showSavedTracks()
{
    closeLyricsView();
    recordNavigation({libraryPageIndex, {}, QStringLiteral("Liked Songs"), {}});
    if (!restoringNavigation_) {
        restoreLibraryScrollPending_ = false;
        savedLibraryScrollPosition_ = 0;
    }
    pageStack_->setCurrentIndex(libraryPageIndex);
    activeLibraryPlaylistId_.clear();
    activeLibraryPlaylistName_ = QStringLiteral("Liked Songs");
    activeLibraryCreator_ = QStringLiteral("Your saved tracks");
    activeLibraryArtworkUrl_.clear();
    librarySummaryText_ = QStringLiteral("Loading your saved tracks…");
    updateLibraryHeader();
    if (likedSongsItem_) playlistsList_->setCurrentItem(likedSongsItem_);
    loadLibraryTracks(0, false);
}

void MainWindow::showPlaylist(const QString &playlistId, const QString &playlistName)
{
    if (playlistId.isEmpty()) {
        return;
    }
    closeLyricsView();
    recordNavigation({libraryPageIndex, playlistId, playlistName, {}});
    if (!restoringNavigation_) {
        restoreLibraryScrollPending_ = false;
        savedLibraryScrollPosition_ = 0;
    }
    pageStack_->setCurrentIndex(libraryPageIndex);
    activeLibraryPlaylistId_ = playlistId;
    activeLibraryPlaylistName_ = playlistName;
    activeLibraryCreator_.clear();
    activeLibraryArtworkUrl_.clear();
    librarySummaryText_ = QStringLiteral("Loading playlist tracks…");
    for (int row = 0; row < playlistsList_->count(); ++row) {
        QListWidgetItem *item = playlistsList_->item(row);
        if (item->data(libraryItemTypeRole).toInt()
                == static_cast<int>(SpotifyLibraryItemType::Playlist)
            && item->data(Qt::UserRole).toString() == playlistId) {
            playlistsList_->setCurrentItem(item);
            break;
        }
    }
    updateActiveLibraryMetadata();
    loadLibraryTracks(0, false);
}

void MainWindow::loadLibraryTracks(int offset, bool append, bool forceNetwork,
                                   bool scanAllPages)
{
    if (!spotifyConnection_->isConnected()) {
        librarySummaryText_ = QStringLiteral("Connect Spotify to load your music.");
        updateLibraryHeader();
        return;
    }

    if (!append) {
        ++libraryRequestId_;
        activeLibraryPageOffset_ = -1;
        pendingLibraryPageOffsets_.clear();
        cachedLibraryPageOffsets_.clear();
        forceNetworkLibraryPageOffsets_.clear();
        libraryTotalTracks_ = 0;
        libraryTotalDurationMs_ = 0;
        libraryPageDurations_.clear();
        librarySummaryScanActive_ = scanAllPages;
        libraryForceRefreshActive_ = scanAllPages && forceNetwork;
        libraryTracksModel_->clearRows();
        updateLibraryArtwork();
        libraryBusy_ = false;
    }
    requestLibraryRange(offset, forceNetwork);
}

void MainWindow::requestLibraryRange(int firstRow, bool forceNetwork)
{
    if (!spotifyConnection_->isConnected()) {
        return;
    }

    constexpr int spotifyPageSize = 50;
    const int prefetchBatchSize = librarySummaryScanActive_ ? 500 : spotifyPageSize;
    firstRow = qMax(0, firstRow);
    const int batchStart = (firstRow / prefetchBatchSize) * prefetchBatchSize;
    const int batchEnd = libraryTotalTracks_ > 0
        ? qMin(batchStart + prefetchBatchSize, libraryTotalTracks_)
        : batchStart + prefetchBatchSize;

    bool alreadyTargetingBatch = activeLibraryPageOffset_ >= batchStart
        && activeLibraryPageOffset_ < batchEnd;
    for (int queuedOffset : pendingLibraryPageOffsets_) {
        alreadyTargetingBatch = alreadyTargetingBatch
            || (queuedOffset >= batchStart && queuedOffset < batchEnd);
    }
    if (!alreadyTargetingBatch) {
        pendingLibraryPageOffsets_.clear();
        forceNetworkLibraryPageOffsets_.clear();
    }

    if (forceNetwork || libraryForceRefreshActive_) {
        for (int offset = batchStart; offset < batchEnd; offset += spotifyPageSize) {
            forceNetworkLibraryPageOffsets_.insert(offset);
        }
    }
    for (int offset = batchStart; offset < batchEnd; offset += spotifyPageSize) {
        if (cachedLibraryPageOffsets_.contains(offset)
            || activeLibraryPageOffset_ == offset
            || pendingLibraryPageOffsets_.contains(offset)) {
            continue;
        }
        pendingLibraryPageOffsets_.append(offset);
    }
    processNextLibraryPage();
}

void MainWindow::processNextLibraryPage()
{
    if (activeLibraryPageOffset_ >= 0) {
        return;
    }

    while (!pendingLibraryPageOffsets_.isEmpty()
           && (cachedLibraryPageOffsets_.contains(pendingLibraryPageOffsets_.first())
               || (libraryTotalTracks_ > 0
                   && pendingLibraryPageOffsets_.first() >= libraryTotalTracks_))) {
        pendingLibraryPageOffsets_.removeFirst();
    }
    if (pendingLibraryPageOffsets_.isEmpty()) {
        libraryBusy_ = false;
        libraryRefreshButton_->setEnabled(spotifyConnection_->isConnected());
        if (librarySummaryScanActive_ && libraryTotalTracks_ > 0) {
            constexpr int spotifyPageSize = 50;
            int nextMissingOffset = -1;
            for (int offset = 0; offset < libraryTotalTracks_; offset += spotifyPageSize) {
                if (!cachedLibraryPageOffsets_.contains(offset)) {
                    nextMissingOffset = offset;
                    break;
                }
            }
            if (nextMissingOffset >= 0) {
                requestLibraryRange(nextMissingOffset, libraryForceRefreshActive_);
                return;
            }
        }
        librarySummaryScanActive_ = false;
        libraryForceRefreshActive_ = false;
        updateLibraryStatus();
        tryFocusPlaybackTrack();
        return;
    }

    const int offset = pendingLibraryPageOffsets_.takeFirst();
    const QString playlistId = activeLibraryPlaylistId_;
    const quint64 requestId = libraryRequestId_;
    const bool forceNetwork = forceNetworkLibraryPageOffsets_.remove(offset);
    activeLibraryPageOffset_ = offset;
    libraryBusy_ = true;
    libraryRefreshButton_->setEnabled(spotifyConnection_->isConnected());
    updateLibraryStatus();

    const auto handlePage = [this, requestId, offset](
                                const QList<SpotifyTrackResult> &tracks, int,
                                int total, const QString &error) {
        if (requestId != libraryRequestId_) {
            return;
        }

        const bool restoreLibraryViewport = !restoreLibraryScrollPending_
            && pendingLibraryFocusTrackUri_.isEmpty()
            && pageStack_->currentIndex() == libraryPageIndex;
        const int previousScrollPosition = libraryTracksTable_->verticalScrollBar()->value();
        QString scrollAnchorUri;
        int scrollAnchorRow = -1;
        int scrollAnchorTop = 0;
        if (restoreLibraryViewport) {
            int viewportY = 0;
            while (viewportY < libraryTracksTable_->viewport()->height()) {
                const int visibleRow = libraryTracksTable_->rowAt(viewportY);
                if (visibleRow < 0) {
                    break;
                }
                const QRect visibleRect = libraryTracksTable_->visualRect(
                    libraryTracksModel_->index(visibleRow, trackTitleColumn));
                const SpotifyTrackResult *anchorTrack =
                    libraryTracksModel_->trackAt(visibleRow);
                if (anchorTrack) {
                    scrollAnchorUri = anchorTrack->spotifyUri;
                    scrollAnchorRow = visibleRow;
                    scrollAnchorTop = visibleRect.top();
                    break;
                }
                viewportY = qMax(viewportY + 1, visibleRect.bottom() + 1);
            }
        }

        activeLibraryPageOffset_ = -1;
        if (!error.isEmpty()) {
            if (tracks.isEmpty()) {
                pendingLibraryPageOffsets_.clear();
                libraryBusy_ = false;
                librarySummaryScanActive_ = false;
                libraryForceRefreshActive_ = false;
                if (pendingLibraryFocusPlaylistId_ == activeLibraryPlaylistId_) {
                    pendingLibraryFocusTrackUri_.clear();
                    pendingLibraryFocusPlaylistId_.clear();
                }
                libraryRefreshButton_->setEnabled(spotifyConnection_->isConnected());
                librarySummaryText_ = error;
                updateLibraryHeader();
                return;
            }
        }

        cachedLibraryPageOffsets_.insert(offset);
        libraryTotalTracks_ = qMax(0, total);
        for (auto it = libraryPageDurations_.begin(); it != libraryPageDurations_.end();) {
            if (it.key() >= libraryTotalTracks_) {
                libraryTotalDurationMs_ -= it.value();
                it = libraryPageDurations_.erase(it);
            } else {
                ++it;
            }
        }
        if (!libraryPageDurations_.contains(offset)) {
            qint64 pageDurationMs = 0;
            for (const SpotifyTrackResult &track : tracks) {
                pageDurationMs += qMax(0, track.durationMs);
            }
            libraryPageDurations_.insert(offset, pageDurationMs);
            libraryTotalDurationMs_ += pageDurationMs;
        }
        libraryTracksModel_->setTotalRows(libraryTotalTracks_);
        libraryTracksModel_->setTracks(offset, tracks);
        tryFocusPlaybackTrack();
        updateLibraryArtwork();
        updateTrackTablePlaybackState(libraryTracksModel_);
        lastPlaybackSelectionPage_ = -1;
        updatePlaybackUi(playbackState_);
        if (restoreLibraryScrollPending_) {
            restoreLibraryScrollPending_ = false;
            const int scrollPosition = savedLibraryScrollPosition_;
            QTimer::singleShot(0, this, [this, scrollPosition] {
                libraryTracksTable_->verticalScrollBar()->setValue(scrollPosition);
            });
        }
        if (restoreLibraryViewport) {
            QTimer::singleShot(0, this,
                [this, scrollAnchorUri, scrollAnchorRow, scrollAnchorTop,
                 previousScrollPosition] {
                QTimer::singleShot(0, this,
                    [this, scrollAnchorUri, scrollAnchorRow, scrollAnchorTop,
                     previousScrollPosition] {
                    if (pageStack_->currentIndex() != libraryPageIndex
                        || restoreLibraryScrollPending_
                        || !pendingLibraryFocusTrackUri_.isEmpty()) {
                        return;
                    }
                    QScrollBar *scrollBar = libraryTracksTable_->verticalScrollBar();
                    if (!scrollAnchorUri.isEmpty()) {
                        const int row = libraryTracksModel_->rowForUri(scrollAnchorUri,
                                                                        scrollAnchorRow);
                        if (row >= 0) {
                            const QModelIndex anchorIndex = libraryTracksModel_->index(
                                row, trackTitleColumn);
                            libraryTracksTable_->scrollTo(anchorIndex,
                                                          QAbstractItemView::PositionAtTop);
                            scrollBar->setValue(qBound(scrollBar->minimum(),
                                                       scrollBar->value() - scrollAnchorTop,
                                                       scrollBar->maximum()));
                            return;
                        }
                    }
                    scrollBar->setValue(qBound(scrollBar->minimum(), previousScrollPosition,
                                               scrollBar->maximum()));
                });
            });
        }
        for (auto it = cachedLibraryPageOffsets_.begin(); it != cachedLibraryPageOffsets_.end();) {
            if (*it >= libraryTotalTracks_) {
                it = cachedLibraryPageOffsets_.erase(it);
            } else {
                ++it;
            }
        }
        for (int index = pendingLibraryPageOffsets_.size() - 1; index >= 0; --index) {
            if (pendingLibraryPageOffsets_.at(index) >= libraryTotalTracks_) {
                pendingLibraryPageOffsets_.removeAt(index);
            }
        }
        updateLibraryStatus();
        if (!error.isEmpty()) {
            librarySummaryText_ = QStringLiteral("Showing saved tracks; refresh failed.");
            updateLibraryHeader();
        }
        processNextLibraryPage();
    };

    if (playlistId.isEmpty()) {
        spotifyConnection_->fetchSavedTracks(offset, handlePage, forceNetwork);
    } else {
        spotifyConnection_->fetchPlaylistTracks(playlistId, offset, handlePage, forceNetwork);
    }
}

void MainWindow::updateLibraryStatus()
{
    if (!spotifyConnection_->isConnected()) {
        librarySummaryText_ = QStringLiteral("Connect Spotify to load your music.");
        updateLibraryHeader();
        return;
    }
    if (libraryTotalTracks_ == 0 && !libraryBusy_
        && pendingLibraryPageOffsets_.isEmpty()) {
        librarySummaryText_ = activeLibraryPlaylistId_.isEmpty()
            ? QStringLiteral("No saved tracks yet.")
            : QStringLiteral("This playlist is empty.");
        updateLibraryHeader();
        return;
    }

    const QString trackCount = QStringLiteral("%1 %2")
        .arg(libraryTotalTracks_)
        .arg(libraryTotalTracks_ == 1 ? QStringLiteral("track") : QStringLiteral("tracks"));
    if (librarySummaryScanActive_ || libraryBusy_ || !pendingLibraryPageOffsets_.isEmpty()) {
        librarySummaryText_ = trackCount + QStringLiteral(" · Calculating total time…");
        updateLibraryHeader();
        return;
    }

    const int expectedPageCount = (libraryTotalTracks_ + 49) / 50;
    if (libraryPageDurations_.size() < expectedPageCount) {
        librarySummaryText_ = trackCount + QStringLiteral(" · Total time fills in as you browse");
        updateLibraryHeader();
        return;
    }

    const qint64 totalMinutes = libraryTotalDurationMs_ / (60 * 1000);
    const qint64 hours = totalMinutes / 60;
    const qint64 minutes = totalMinutes % 60;
    librarySummaryText_ = trackCount + QStringLiteral(" · %1 hr %2 min")
                              .arg(hours).arg(minutes, 2, 10, QLatin1Char('0'));
    updateLibraryHeader();
}

void MainWindow::updateLibraryHeader()
{
    if (libraryTracksModel_) {
        libraryTracksModel_->setLibraryHeader(activeLibraryPlaylistName_, activeLibraryCreator_,
                                              librarySummaryText_);
    }
}

void MainWindow::updateActiveLibraryMetadata()
{
    if (activeLibraryPlaylistId_.isEmpty()) {
        activeLibraryCreator_ = QStringLiteral("Your saved tracks");
        activeLibraryArtworkUrl_.clear();
        updateLibraryHeader();
        updateLibraryArtwork();
        return;
    }

    activeLibraryCreator_.clear();
    activeLibraryArtworkUrl_.clear();
    for (int row = 0; row < playlistsList_->count(); ++row) {
        const QListWidgetItem *item = playlistsList_->item(row);
        if (item->data(libraryItemTypeRole).toInt()
                == static_cast<int>(SpotifyLibraryItemType::Playlist)
            && item->data(Qt::UserRole).toString() == activeLibraryPlaylistId_) {
            activeLibraryCreator_ = item->data(libraryItemCreatorRole).toString();
            activeLibraryArtworkUrl_ = item->data(libraryItemArtworkUrlRole).toString();
            break;
        }
    }
    updateLibraryHeader();
    updateLibraryArtwork();
}

void MainWindow::updateLibraryArtwork()
{
    auto *table = dynamic_cast<TrackTableView *>(libraryTracksTable_);
    if (!table) {
        return;
    }

    if (activeLibraryPlaylistId_.isEmpty()) {
        const QString likedSongsArtworkKey = QStringLiteral("liked-songs-icon");
        if (requestedLibraryArtworkUrl_ != likedSongsArtworkKey) {
            requestedLibraryArtworkUrl_ = likedSongsArtworkKey;
            ++libraryArtworkRequestId_;
            table->setLibraryArtwork(likedSongsIcon().pixmap(QSize(192, 192)).toImage());
        }
        return;
    }

    QString artworkUrl = activeLibraryArtworkUrl_;
    if (artworkUrl.isEmpty() && libraryTracksModel_) {
        const SpotifyTrackResult *firstTrack =
            libraryTracksModel_->trackAt(libraryTracksModel_->firstTrackRow());
        if (firstTrack) {
            artworkUrl = firstTrack->albumImageUrl;
        }
    }
    if (artworkUrl.isEmpty()) {
        // Keep the last artwork visible while an empty or inaccessible playlist has no
        // track row from which to discover a replacement image.
        requestedLibraryArtworkUrl_.clear();
        ++libraryArtworkRequestId_;
        return;
    }
    if (artworkUrl == requestedLibraryArtworkUrl_) {
        return;
    }

    requestedLibraryArtworkUrl_ = artworkUrl;
    const quint64 requestId = ++libraryArtworkRequestId_;
    if (!spotifyConnection_) {
        table->setLibraryArtwork({});
        return;
    }

    QPointer<TrackTableView> tableGuard(table);
    spotifyConnection_->requestImage(artworkUrl,
        [this, tableGuard, artworkUrl, requestId](const QImage &image) {
            if (!tableGuard || requestId != libraryArtworkRequestId_
                || requestedLibraryArtworkUrl_ != artworkUrl) {
                return;
            }
            tableGuard->setLibraryArtwork(image);
        });
}

void MainWindow::sortTrackTable(QTableView *table, TrackTableModel *model, int column)
{
    if (!table || !model || column < trackTitleColumn || column > trackDurationColumn) {
        return;
    }

    const Qt::SortOrder order = model->sortColumn() == column
        ? (model->sortOrder() == Qt::AscendingOrder ? Qt::DescendingOrder
                                                     : Qt::AscendingOrder)
        : (column == trackAddedAtColumn ? Qt::DescendingOrder : Qt::AscendingOrder);
    QString selectedUri;
    if (table->currentIndex().isValid()) {
        const SpotifyTrackResult *selected = model->trackAt(table->currentIndex().row());
        if (selected) selectedUri = selected->spotifyUri;
    }

    model->sort(column, order);
    table->horizontalHeader()->setSortIndicator(column, order);
    if (!selectedUri.isEmpty()) {
        const int selectedRow = model->rowForUri(selectedUri);
        if (selectedRow >= 0) {
            table->setCurrentIndex(model->index(selectedRow, trackTitleColumn));
            table->selectRow(selectedRow);
        }
    }
}

void MainWindow::playTrackFromTable(QTableView *table, TrackTableModel *model, int row)
{
    if (!table || !model || row < 0 || row >= model->rowCount()) {
        return;
    }
    if (table == libraryTracksTable_
        && row - model->firstTrackRow() >= model->totalTrackCount()) {
        return;
    }

    table->setCurrentIndex(model->index(row, trackTitleColumn));
    table->selectRow(row);
    const SpotifyTrackResult *track = model->trackAt(row);
    const QString uri = track ? track->spotifyUri : QString{};
    if (uri.isEmpty()) {
        if (table == libraryTracksTable_ && row >= model->firstTrackRow()) {
            requestLibraryRange(row - model->firstTrackRow());
        }
        return;
    }

    if (restoredPlaybackPending_ && uri != playbackState_.trackUri) {
        restoredPlaybackPending_ = false;
    }

    if (table == libraryTracksTable_) {
        playbackSourceName_ = activeLibraryPlaylistName_.isEmpty()
            ? QStringLiteral("Liked Songs") : activeLibraryPlaylistName_;
        playbackSourcePlaylistId_ = activeLibraryPlaylistId_;
        playbackSourceAlbumId_.clear();
    } else if (table == searchResultsTable_) {
        playbackSourceName_ = QStringLiteral("Search results");
        playbackSourcePlaylistId_.clear();
        playbackSourceAlbumId_.clear();
    } else if (table == albumTracksTable_) {
        playbackSourceName_ = currentAlbumProfile_.name.isEmpty()
            ? currentAlbumPageName_ : currentAlbumProfile_.name;
        playbackSourcePlaylistId_.clear();
        playbackSourceAlbumId_ = currentAlbumPageId_;
    }
    if (nowPlayingPanelSourceLabel_) {
        nowPlayingPanelSourceLabel_->setText(playbackSourceName_);
    }

    if (restoredPlaybackPending_ && uri == playbackState_.trackUri) {
        playPauseButton_->click();
        return;
    }

    if (playbackState_.active && playbackState_.trackUri == uri) {
        if (playbackState_.paused) {
            spotifyConnection_->resumePlayback();
        } else {
            spotifyConnection_->pausePlayback();
        }
        return;
    }
    playSelectedTrack();
}

void MainWindow::showTrackContextMenu(QTableView *table, TrackTableModel *model,
                                      const QPoint &position)
{
    if (!table || !model) {
        return;
    }
    const QModelIndex index = table->indexAt(position);
    const SpotifyTrackResult *track = index.isValid() ? model->trackAt(index.row()) : nullptr;
    if (!track) {
        return;
    }
    const QString albumId = track->albumId;
    const QString artistId = track->artistId;
    table->selectRow(index.row());
    QMenu menu(table);
    QAction *albumAction = menu.addAction(QStringLiteral("View album profile"));
    QAction *artistAction = menu.addAction(QStringLiteral("View artist profile"));
    albumAction->setEnabled(!albumId.isEmpty());
    artistAction->setEnabled(!artistId.isEmpty());
    QAction *selected = menu.exec(table->viewport()->mapToGlobal(position));
    if (selected == albumAction) {
        showAlbumPage(albumId, track->album);
    } else if (selected == artistAction) {
        openArtistById(artistId, track->artists.section(QStringLiteral(", "), 0, 0));
    }
}

void MainWindow::openArtistById(const QString &artistId, const QString &artistName)
{
    if (artistId.isEmpty()) return;
    showArtistPage(artistId, artistName);
}

void MainWindow::showArtistPage(const QString &artistId, const QString &artistName)
{
    if (artistId.isEmpty()) return;
    closeLyricsView();
    NavigationEntry entry;
    entry.pageIndex = artistPageIndex;
    entry.artistId = artistId;
    entry.artistName = artistName;
    recordNavigation(entry);

    currentArtistPageId_ = artistId;
    currentArtistPageName_ = artistName;
    if (!restoringNavigation_) {
        artistPageScrollPosition_ = 0;
        artistReleaseListScrollPosition_ = 0;
        restoreArtistScrollPending_ = false;
    }
    pageStack_->setCurrentIndex(artistPageIndex);
    ++artistPageRequestId_;
    artistProfileLoaded_ = false;
    artistReleaseRequestActive_ = false;
    artistReleaseOffset_ = 0;
    artistReleaseTotal_ = 0;
    artistReleaseIds_.clear();
    artistReleasesList_->clear();
    artistLoadMoreButton_->hide();
    artistPageArtworkLabel_->clearImage();
    artistPageNameLabel_->setText(artistName.isEmpty() ? QStringLiteral("Loading artist…")
                                                       : artistName);
    artistPageInfoLabel_->setText(QStringLiteral("Loading artist details…"));
    artistPageStatusLabel_->setText(spotifyConnection_->isConnected()
        ? QStringLiteral("Loading releases…")
        : QStringLiteral("Connect Spotify to load releases."));
    if (spotifyConnection_->isConnected()) {
        loadArtistPage();
    }
}

void MainWindow::loadArtistPage()
{
    if (currentArtistPageId_.isEmpty() || !spotifyConnection_->isConnected()) return;
    const QString artistId = currentArtistPageId_;
    const quint64 requestId = artistPageRequestId_;
    spotifyConnection_->fetchArtistProfile(
        artistId, [this, artistId, requestId](const SpotifyArtistProfile &artist,
                                              const QString &error) {
        if (requestId != artistPageRequestId_ || artistId != currentArtistPageId_) return;
        artistProfileLoaded_ = true;
        if (!error.isEmpty()) {
            artistPageInfoLabel_->setText(error);
            if (artistPageNameLabel_->text().isEmpty()) {
                artistPageNameLabel_->setText(QStringLiteral("Artist"));
            }
            return;
        }
        if (!artist.name.isEmpty()) {
            currentArtistPageName_ = artist.name;
            artistPageNameLabel_->setText(artist.name);
        }
        QStringList details;
        if (!artist.genres.isEmpty()) details.append(artist.genres.join(QStringLiteral(" · ")));
        if (artist.followers >= 0) {
            details.append(QStringLiteral("%1 followers")
                               .arg(QLocale().toString(artist.followers)));
        }
        artistPageInfoLabel_->setText(details.isEmpty()
            ? QStringLiteral("Artist on Spotify") : details.join(QStringLiteral(" · ")));
        if (!artist.imageUrl.isEmpty()) {
            QPointer<PanelImageLabel> artworkGuard(artistPageArtworkLabel_);
            spotifyConnection_->requestImage(artist.imageUrl,
                [this, artistId, requestId, artworkGuard](const QImage &image) {
                if (!artworkGuard || image.isNull() || requestId != artistPageRequestId_
                    || artistId != currentArtistPageId_) return;
                artworkGuard->setImage(image);
            });
        }
    });

    if (artistReleaseRequestActive_) return;
    artistReleaseRequestActive_ = true;
    spotifyConnection_->fetchArtistAlbums(
        artistId, 0, [this, artistId, requestId](const QList<SpotifyAlbumProfile> &albums,
                                                int nextOffset, int total,
                                                bool fromCache,
                                                const QString &error) {
        if (requestId != artistPageRequestId_ || artistId != currentArtistPageId_) return;
        artistReleaseRequestActive_ = false;
        if (!error.isEmpty()) {
            artistPageStatusLabel_->setText(error);
            artistLoadMoreButton_->setText(QStringLiteral("Retry loading releases"));
            artistLoadMoreButton_->setEnabled(true);
            artistLoadMoreButton_->setVisible(true);
            return;
        }
        artistReleaseOffset_ = nextOffset;
        artistReleaseTotal_ = total;
        appendArtistReleases(albums);
        if (artistReleaseTotal_ == 0) {
            artistPageStatusLabel_->setText(QStringLiteral("No releases were returned."));
        } else {
            artistPageStatusLabel_->setText(QStringLiteral("%1 of %2 releases loaded…")
                                                .arg(artistReleaseIds_.size())
                                                .arg(artistReleaseTotal_));
        }
        const bool moreAvailable = artistReleaseOffset_ < artistReleaseTotal_;
        artistLoadMoreButton_->setVisible(moreAvailable);
        artistLoadMoreButton_->setEnabled(false);
        artistLoadMoreButton_->setText(QStringLiteral("Loading all releases…"));
        if (moreAvailable) {
            QTimer::singleShot(fromCache ? 0 : 750, this, [this, requestId, artistId] {
                if (requestId == artistPageRequestId_ && artistId == currentArtistPageId_) {
                    loadMoreArtistReleases();
                }
            });
        } else {
            artistPageStatusLabel_->setText(QStringLiteral("%1 releases")
                                                .arg(artistReleaseIds_.size()));
        }
    });
}

void MainWindow::loadMoreArtistReleases()
{
    if (artistReleaseRequestActive_ || currentArtistPageId_.isEmpty()
        || !spotifyConnection_->isConnected()
        || artistReleaseOffset_ >= artistReleaseTotal_) return;
    artistReleaseRequestActive_ = true;
    artistLoadMoreButton_->setEnabled(false);
    artistLoadMoreButton_->setText(QStringLiteral("Loading all releases…"));
    artistPageStatusLabel_->setText(QStringLiteral("%1 of %2 releases loaded…")
                                        .arg(artistReleaseIds_.size())
                                        .arg(artistReleaseTotal_));
    const QString artistId = currentArtistPageId_;
    const int offset = artistReleaseOffset_;
    const quint64 requestId = artistPageRequestId_;
    spotifyConnection_->fetchArtistAlbums(
        artistId, offset, [this, artistId, requestId](const QList<SpotifyAlbumProfile> &albums,
                                                     int nextOffset, int total,
                                                     bool fromCache,
                                                     const QString &error) {
        if (requestId != artistPageRequestId_ || artistId != currentArtistPageId_) return;
        artistReleaseRequestActive_ = false;
        if (!error.isEmpty()) {
            artistPageStatusLabel_->setText(error);
            artistLoadMoreButton_->setText(QStringLiteral("Retry loading releases"));
            artistLoadMoreButton_->setEnabled(true);
            artistLoadMoreButton_->setVisible(true);
            return;
        }
        artistReleaseOffset_ = nextOffset;
        artistReleaseTotal_ = total;
        appendArtistReleases(albums);
        artistPageStatusLabel_->setText(QStringLiteral("%1 of %2 releases loaded…")
                                            .arg(artistReleaseIds_.size())
                                            .arg(artistReleaseTotal_));
        const bool moreAvailable = artistReleaseOffset_ < artistReleaseTotal_;
        artistLoadMoreButton_->setVisible(moreAvailable);
        artistLoadMoreButton_->setEnabled(false);
        artistLoadMoreButton_->setText(QStringLiteral("Loading all releases…"));
        if (moreAvailable) {
            QTimer::singleShot(fromCache ? 0 : 750, this, [this, requestId, artistId] {
                if (requestId == artistPageRequestId_ && artistId == currentArtistPageId_) {
                    loadMoreArtistReleases();
                }
            });
        } else {
            artistPageStatusLabel_->setText(QStringLiteral("%1 releases")
                                                .arg(artistReleaseIds_.size()));
        }
    });
}

void MainWindow::appendArtistReleases(const QList<SpotifyAlbumProfile> &albums)
{
    const bool restoringScroll = restoreArtistScrollPending_;
    const int pagePosition = restoringScroll
        ? artistPageScrollPosition_ : artistPageScroll_->verticalScrollBar()->value();
    const int releasePosition = restoringScroll
        ? artistReleaseListScrollPosition_ : artistReleasesList_->verticalScrollBar()->value();

    for (const SpotifyAlbumProfile &album : albums) {
        if (album.id.isEmpty() || artistReleaseIds_.contains(album.id)) continue;
        artistReleaseIds_.insert(album.id);
        int insertAt = 0;
        while (insertAt < artistReleasesList_->count()) {
            const QString existingDate = artistReleasesList_->item(insertAt)
                                             ->data(artistReleaseDateRole).toString();
            if (album.releaseDate > existingDate) break;
            ++insertAt;
        }
        auto *item = new QListWidgetItem(QIcon(), album.name);
        artistReleasesList_->insertItem(insertAt, item);
        item->setData(Qt::UserRole, album.id);
        item->setData(artistReleaseDateRole, album.releaseDate);
        QString category = album.albumType == QStringLiteral("album")
            ? QStringLiteral("Album")
            : album.albumType == QStringLiteral("single")
                ? QStringLiteral("Single / EP")
                : album.albumType == QStringLiteral("compilation")
                    ? QStringLiteral("Compilation") : QStringLiteral("Release");
        QStringList subtitle{category};
        if (!album.releaseDate.isEmpty()) subtitle.append(album.releaseDate.left(4));
        if (album.totalTracks > 0) {
            subtitle.append(QStringLiteral("%1 tracks").arg(album.totalTracks));
        }
        item->setData(libraryItemCategoryRole, subtitle.join(QStringLiteral(" · ")));
        item->setData(libraryItemCreatorRole, album.artists);
        item->setToolTip(album.name);
        item->setSizeHint(QSize(240, 60));
        if (!album.imageUrl.isEmpty()) {
            const QString albumId = album.id;
            const quint64 requestId = artistPageRequestId_;
            spotifyConnection_->requestImage(album.imageUrl,
                [this, albumId, requestId](const QImage &image) {
                if (image.isNull() || requestId != artistPageRequestId_
                    || !artistReleaseIds_.contains(albumId)) return;
                for (int row = 0; row < artistReleasesList_->count(); ++row) {
                    QListWidgetItem *candidate = artistReleasesList_->item(row);
                    if (candidate->data(Qt::UserRole).toString() == albumId) {
                        candidate->setIcon(QIcon(QPixmap::fromImage(image).scaled(
                            QSize(42, 42), Qt::KeepAspectRatioByExpanding,
                            Qt::SmoothTransformation)));
                        break;
                    }
                }
            });
        }
    }
    const int rowsHeight = artistReleasesList_->count() * 62 + 4;
    artistReleasesList_->setMinimumHeight(qMin(rowsHeight, 480));
    artistReleasesList_->setMaximumHeight(qMin(rowsHeight, 480));
    QTimer::singleShot(0, this, [this, pagePosition, releasePosition, restoringScroll] {
        if (restoringScroll && !restoreArtistScrollPending_) return;
        QScrollBar *pageScrollBar = artistPageScroll_->verticalScrollBar();
        QScrollBar *releaseScrollBar = artistReleasesList_->verticalScrollBar();
        pageScrollBar->setValue(pagePosition);
        releaseScrollBar->setValue(releasePosition);
        artistPageScrollPosition_ = pageScrollBar->value();
        artistReleaseListScrollPosition_ = releaseScrollBar->value();
        if (restoringScroll
            && pagePosition <= pageScrollBar->maximum()
            && releasePosition <= releaseScrollBar->maximum()) {
            restoreArtistScrollPending_ = false;
        }
    });
}

void MainWindow::showAlbumPage(const QString &albumId, const QString &albumName)
{
    if (albumId.isEmpty()) return;
    closeLyricsView();
    if (!restoringNavigation_ && pageStack_->currentIndex() == albumPageIndex
        && currentAlbumPageId_ == albumId) {
        return;
    }
    NavigationEntry entry;
    entry.pageIndex = albumPageIndex;
    entry.albumId = albumId;
    entry.albumName = albumName;
    recordNavigation(entry);

    currentAlbumPageId_ = albumId;
    currentAlbumPageName_ = albumName;
    if (!restoringNavigation_) {
        albumPageScrollPosition_ = 0;
        restoreAlbumScrollPending_ = false;
    }
    currentAlbumProfile_ = {};
    albumPageTracks_.clear();
    albumTrackOffset_ = 0;
    albumTrackTotal_ = 0;
    albumTrackRequestActive_ = false;
    albumTrackIds_.clear();
    ++albumPageRequestId_;
    pageStack_->setCurrentIndex(albumPageIndex);
    albumPageNameLabel_->setText(albumName.isEmpty()
        ? QStringLiteral("Loading album…") : albumName);
    albumPageArtistLabel_->clear();
    albumPageArtistLabel_->setClickable(false);
    albumPageInfoLabel_->clear();
    albumPageStatusLabel_->setText(spotifyConnection_->isConnected()
        ? QStringLiteral("Loading album details…")
        : QStringLiteral("Connect Spotify to load this album."));
    albumPageArtworkLabel_->clearImage();
    albumTracksModel_->setSearchTracks({});
    albumTracksTable_->setEnabled(false);
    if (spotifyConnection_->isConnected()) loadAlbumPage();
}

void MainWindow::loadAlbumPage()
{
    if (currentAlbumPageId_.isEmpty() || !spotifyConnection_->isConnected()) return;
    const QString albumId = currentAlbumPageId_;
    const quint64 requestId = albumPageRequestId_;
    spotifyConnection_->fetchAlbumProfile(
        albumId, [this, albumId, requestId](const SpotifyAlbumProfile &album,
                                           const QString &error) {
        if (requestId != albumPageRequestId_ || albumId != currentAlbumPageId_) return;
        if (!error.isEmpty()) {
            albumPageStatusLabel_->setText(error);
            if (currentAlbumPageName_.isEmpty()) {
                albumPageNameLabel_->setText(QStringLiteral("Album"));
            }
        } else {
            currentAlbumProfile_ = album;
            currentAlbumPageName_ = album.name;
            albumPageNameLabel_->setText(album.name);
            QStringList facts;
            albumPageArtistLabel_->setText(album.artists);
            albumPageArtistLabel_->setClickable(!album.artistId.isEmpty());
            if (!album.releaseDate.isEmpty()) facts.append(album.releaseDate);
            if (!album.albumType.isEmpty()) {
                const QString type = album.albumType == QStringLiteral("single")
                    ? QStringLiteral("Single / EP")
                    : album.albumType.left(1).toUpper() + album.albumType.mid(1);
                facts.append(type);
            }
            if (album.totalTracks > 0) {
                facts.append(QStringLiteral("%1 tracks").arg(album.totalTracks));
            }
            albumPageInfoLabel_->setText(facts.join(QStringLiteral(" · ")));
            if (!album.imageUrl.isEmpty()) {
                QPointer<PanelImageLabel> artworkGuard(albumPageArtworkLabel_);
                spotifyConnection_->requestImage(album.imageUrl,
                    [artworkGuard](const QImage &image) {
                    if (artworkGuard && !image.isNull()) artworkGuard->setImage(image);
                });
            }
        }

        albumTracksTable_->setEnabled(true);
        albumTrackRequestActive_ = true;
        spotifyConnection_->fetchAlbumTracks(
            albumId, 0, [this, albumId, requestId](
                const QList<SpotifyTrackResult> &tracks, int nextOffset,
                int total, const QString &tracksError) {
            if (requestId != albumPageRequestId_ || albumId != currentAlbumPageId_) return;
            albumTrackRequestActive_ = false;
            albumTrackOffset_ = nextOffset;
            albumTrackTotal_ = total;
            if (nextOffset <= 0 && total > 0 && tracks.isEmpty()) {
                albumPageStatusLabel_->setText(!tracksError.isEmpty()
                    ? tracksError : QStringLiteral("Spotify returned no tracks for this release."));
                return;
            }
            QList<SpotifyTrackResult> enriched = tracks;
            for (SpotifyTrackResult &track : enriched) {
                track.albumId = albumId;
                track.album = currentAlbumProfile_.name.isEmpty()
                    ? currentAlbumPageName_ : currentAlbumProfile_.name;
                track.albumImageUrl = currentAlbumProfile_.imageUrl;
                if (!track.spotifyUri.isEmpty()) albumTrackIds_.insert(track.spotifyUri);
            }
            albumPageTracks_ = enriched;
            albumTracksModel_->setTotalRows(total);
            albumTracksModel_->setTracks(0, enriched);
            updateTrackTablePlaybackState(albumTracksModel_);
            tryFocusPlaybackTrack();
            lastPlaybackSelectionPage_ = -1;
            updatePlaybackUi(playbackState_);
            albumPageStatusLabel_->setText(!tracksError.isEmpty()
                ? tracksError
                : total == 0 ? QStringLiteral("This release has no tracks.")
                : QStringLiteral("%1 of %2 tracks loaded…")
                      .arg(albumTracksModel_->loadedTrackCount()).arg(total));
            if (restoreAlbumScrollPending_) {
                restoreAlbumScrollPending_ = false;
                const int scrollPosition = albumPageScrollPosition_;
                QTimer::singleShot(0, this, [this, scrollPosition] {
                    albumTracksTable_->verticalScrollBar()->setValue(scrollPosition);
                });
            }
            if (albumTrackOffset_ < albumTrackTotal_) {
                QTimer::singleShot(100, this, [this, requestId, albumId] {
                    if (requestId == albumPageRequestId_ && albumId == currentAlbumPageId_) {
                        loadMoreAlbumTracks();
                    }
                });
            } else if (albumTrackTotal_ > 0) {
                albumPageStatusLabel_->setText(QStringLiteral("%1 tracks")
                                                  .arg(albumTrackTotal_));
            }
        });
    });
}

void MainWindow::loadMoreAlbumTracks()
{
    if (albumTrackRequestActive_ || currentAlbumPageId_.isEmpty()
        || !spotifyConnection_->isConnected() || albumTrackOffset_ >= albumTrackTotal_) return;
    albumTrackRequestActive_ = true;
    const QString albumId = currentAlbumPageId_;
    const int offset = albumTrackOffset_;
    const quint64 requestId = albumPageRequestId_;
    spotifyConnection_->fetchAlbumTracks(
        albumId, offset, [this, albumId, requestId, offset](
            const QList<SpotifyTrackResult> &tracks, int nextOffset,
            int total, const QString &error) {
        if (requestId != albumPageRequestId_ || albumId != currentAlbumPageId_) return;
        albumTrackRequestActive_ = false;
        if (tracks.isEmpty() && !error.isEmpty()) {
            albumPageStatusLabel_->setText(error);
            return;
        }
        if (tracks.isEmpty() && nextOffset <= offset && total > offset) {
            albumPageStatusLabel_->setText(QStringLiteral("Spotify returned no more tracks."));
            return;
        }
        albumTrackOffset_ = nextOffset;
        albumTrackTotal_ = total;
        QList<SpotifyTrackResult> enriched = tracks;
        for (SpotifyTrackResult &track : enriched) {
            track.albumId = albumId;
            track.album = currentAlbumProfile_.name.isEmpty()
                ? currentAlbumPageName_ : currentAlbumProfile_.name;
            track.albumImageUrl = currentAlbumProfile_.imageUrl;
            if (!track.spotifyUri.isEmpty()) albumTrackIds_.insert(track.spotifyUri);
        }
        albumPageTracks_.append(enriched);
        albumTracksModel_->setTracks(offset, enriched);
        albumTracksModel_->setTotalRows(total);
        albumPageStatusLabel_->setText(!error.isEmpty()
            ? error
            : QStringLiteral("%1 of %2 tracks loaded…")
                  .arg(albumTracksModel_->loadedTrackCount()).arg(total));
        updateTrackTablePlaybackState(albumTracksModel_);
        tryFocusPlaybackTrack();
        lastPlaybackSelectionPage_ = -1;
        updatePlaybackUi(playbackState_);
        if (albumTrackOffset_ < albumTrackTotal_) {
            QTimer::singleShot(100, this, [this, requestId, albumId] {
                if (requestId == albumPageRequestId_ && albumId == currentAlbumPageId_) {
                    loadMoreAlbumTracks();
                }
            });
        } else {
            albumPageStatusLabel_->setText(QStringLiteral("%1 tracks")
                                               .arg(albumTrackTotal_));
        }
    });
}

void MainWindow::updateTrackTablesPlaybackState()
{
    updateTrackTablePlaybackState(searchResultsModel_);
    updateTrackTablePlaybackState(libraryTracksModel_);
    updateTrackTablePlaybackState(albumTracksModel_);
}

void MainWindow::updateTrackTablePlaybackState(TrackTableModel *model)
{
    if (!model) {
        return;
    }
    model->setPlaybackState(playbackState_);
    QTableView *table = model == searchResultsModel_ ? searchResultsTable_
        : model == libraryTracksModel_ ? libraryTracksTable_
        : model == albumTracksModel_ ? albumTracksTable_ : nullptr;
    if (!table) {
        return;
    }
    table->viewport()->update();
}

TrackTableModel *MainWindow::modelForTable(const QTableView *table) const
{
    if (table == searchResultsTable_) return searchResultsModel_;
    if (table == libraryTracksTable_) return libraryTracksModel_;
    if (table == albumTracksTable_) return albumTracksModel_;
    return nullptr;
}

QString MainWindow::selectedTrackUri() const
{
    const QTableView *table = nullptr;
    if (pageStack_->currentIndex() == 1) {
        table = searchResultsTable_;
    } else if (pageStack_->currentIndex() == 2) {
        table = libraryTracksTable_;
    } else if (pageStack_->currentIndex() == albumPageIndex) {
        table = albumTracksTable_;
    }
    if (!table || !table->currentIndex().isValid()) {
        return {};
    }

    const TrackTableModel *model = modelForTable(table);
    const SpotifyTrackResult *track = model ? model->trackAt(table->currentIndex().row()) : nullptr;
    return track ? track->spotifyUri : QString{};
}

void MainWindow::playSelectedTrack()
{
    const QString uri = selectedTrackUri();
    if (uri.isEmpty()) {
        if (nowPlayingArtistLabel_) {
            nowPlayingArtistLabel_->setText(QStringLiteral("This item does not have a playable Spotify track URI."));
        }
        return;
    }
    if (restoredPlaybackPending_ && uri != playbackState_.trackUri) {
        restoredPlaybackPending_ = false;
    }
    if (pageStack_->currentIndex() == libraryPageIndex) {
        playbackSourceName_ = activeLibraryPlaylistName_.isEmpty()
            ? QStringLiteral("Liked Songs") : activeLibraryPlaylistName_;
        playbackSourcePlaylistId_ = activeLibraryPlaylistId_;
        playbackSourceAlbumId_.clear();
    } else if (pageStack_->currentIndex() == searchPageIndex) {
        playbackSourceName_ = QStringLiteral("Search results");
        playbackSourcePlaylistId_.clear();
        playbackSourceAlbumId_.clear();
    } else if (pageStack_->currentIndex() == albumPageIndex) {
        playbackSourceName_ = currentAlbumProfile_.name.isEmpty()
            ? currentAlbumPageName_ : currentAlbumProfile_.name;
        playbackSourcePlaylistId_.clear();
        playbackSourceAlbumId_ = currentAlbumPageId_;
    }
    if (nowPlayingPanelSourceLabel_) {
        nowPlayingPanelSourceLabel_->setText(playbackSourceName_);
    }
    lastRequestedTrackUri_ = uri;

    if (pageStack_->currentIndex() == libraryPageIndex && !activeLibraryPlaylistId_.isEmpty()) {
        stopLikedSongsPlayback();
        const QString contextUri = QStringLiteral("spotify:playlist:%1").arg(activeLibraryPlaylistId_);
        spotifyConnection_->playTracks({}, contextUri, uri);
        return;
    }

    if (pageStack_->currentIndex() == albumPageIndex && !currentAlbumPageId_.isEmpty()) {
        stopLikedSongsPlayback();
        const QString contextUri = QStringLiteral("spotify:album:%1").arg(currentAlbumPageId_);
        spotifyConnection_->playTracks({}, contextUri, uri);
        return;
    }

    if (pageStack_->currentIndex() == libraryPageIndex && activeLibraryPlaylistId_.isEmpty()) {
        startLikedSongsPlaybackAtRow(libraryTracksTable_->currentIndex().row());
        return;
    }

    stopLikedSongsPlayback();

    QTableView *table = pageStack_->currentIndex() == searchPageIndex ? searchResultsTable_
        : pageStack_->currentIndex() == albumPageIndex ? albumTracksTable_
        : libraryTracksTable_;
    TrackTableModel *model = modelForTable(table);
    QStringList queue;
    const int firstRow = table->currentIndex().row();
    for (int row = firstRow; model && row < model->rowCount() && queue.size() < 100; ++row) {
        const SpotifyTrackResult *track = model->trackAt(row);
        const QString trackUri = track ? track->spotifyUri : QString{};
        if (trackUri.isEmpty()) {
            break;
        }
        if (trackUri.startsWith(QStringLiteral("spotify:track:"))) {
            queue.append(trackUri);
        }
    }
    spotifyConnection_->playTracks(queue);
}

void MainWindow::startLikedSongsPlaybackAtRow(int row)
{
    if (!spotifyConnection_ || !libraryTracksModel_ || row < 0) {
        return;
    }
    const SpotifyTrackResult *selectedTrack = libraryTracksModel_->trackAt(row);
    if (!selectedTrack || !selectedTrack->spotifyUri.startsWith(QStringLiteral("spotify:track:"))) {
        return;
    }

    stopLikedSongsPlayback();
    likedSongsQueueActive_ = true;
    likedSongsQueueAwaitingStart_ = true;
    likedSongsQueuePlayCommandPending_ = true;
    likedSongsQueueCurrentUri_ = selectedTrack->spotifyUri;
    likedSongsQueueCurrentIndex_ = 0;
    likedSongsQueueTotalTracks_ = libraryTracksModel_->totalTrackCount();

    const int firstTrackRow = libraryTracksModel_->firstTrackRow();
    int lastLoadedTrackOffset = row - firstTrackRow;
    for (int trackRow = row; trackRow < libraryTracksModel_->rowCount(); ++trackRow) {
        const SpotifyTrackResult *track = libraryTracksModel_->trackAt(trackRow);
        if (!track) {
            break;
        }
        if (track->spotifyUri.startsWith(QStringLiteral("spotify:track:"))) {
            likedSongsQueueUris_.append(track->spotifyUri);
            lastLoadedTrackOffset = trackRow - firstTrackRow;
        }
    }

    const bool allTracksLoaded = libraryTracksModel_->loadedTrackCount()
        >= libraryTracksModel_->totalTrackCount();
    if (allTracksLoaded) {
        likedSongsQueueNextFetchOffset_ = likedSongsQueueTotalTracks_;
    } else {
        constexpr int spotifyPageSize = 50;
        likedSongsQueueNextFetchOffset_ = qMin(
            likedSongsQueueTotalTracks_,
            ((lastLoadedTrackOffset / spotifyPageSize) + 1) * spotifyPageSize);
    }

    const quint64 generation = likedSongsQueueGeneration_;
    const QString trackUri = likedSongsQueueCurrentUri_;
    spotifyConnection_->playTracks({trackUri}, {}, {}, -1,
        [this, generation](bool success) {
            if (!likedSongsQueueActive_ || generation != likedSongsQueueGeneration_) {
                return;
            }
            likedSongsQueuePlayCommandPending_ = false;
            if (!success) {
                stopLikedSongsPlayback();
                return;
            }
            updateLikedSongsPlaybackQueue(playbackState_);
        });
}

void MainWindow::stopLikedSongsPlayback()
{
    ++likedSongsQueueGeneration_;
    likedSongsQueueActive_ = false;
    likedSongsQueuePlayCommandPending_ = false;
    likedSongsQueueFetchInFlight_ = false;
    likedSongsQueueRequestInFlight_ = false;
    likedSongsQueueAwaitingStart_ = false;
    likedSongsQueueUris_.clear();
    likedSongsQueueCurrentUri_.clear();
    likedSongsQueueQueuedUri_.clear();
    likedSongsQueueCurrentIndex_ = -1;
    likedSongsQueueQueuedIndex_ = -1;
    likedSongsQueueNextFetchOffset_ = -1;
    likedSongsQueueTotalTracks_ = 0;
}

void MainWindow::updateLikedSongsPlaybackQueue(const SpotifyPlaybackState &state)
{
    if (!likedSongsQueueActive_) {
        return;
    }
    if (state.externalDevice) {
        if (!likedSongsQueueAwaitingStart_) {
            stopLikedSongsPlayback();
        }
        return;
    }
    if (!state.active || state.trackUri.isEmpty()) {
        return;
    }
    if (likedSongsQueuePlayCommandPending_) {
        return;
    }
    if (likedSongsQueueAwaitingStart_) {
        if (state.trackUri != likedSongsQueueCurrentUri_) {
            return;
        }
        likedSongsQueueAwaitingStart_ = false;
    }

    int matchingIndex = -1;
    if (state.trackUri == likedSongsQueueQueuedUri_ && likedSongsQueueQueuedIndex_ >= 0) {
        matchingIndex = likedSongsQueueQueuedIndex_;
    } else if (state.trackUri == likedSongsQueueCurrentUri_) {
        matchingIndex = likedSongsQueueCurrentIndex_;
    } else {
        for (int index = likedSongsQueueCurrentIndex_ + 1;
             index < likedSongsQueueUris_.size(); ++index) {
            if (likedSongsQueueUris_.at(index) == state.trackUri) {
                matchingIndex = index;
                break;
            }
        }
    }
    if (matchingIndex < 0) {
        stopLikedSongsPlayback();
        return;
    }

    likedSongsQueueCurrentIndex_ = matchingIndex;
    likedSongsQueueCurrentUri_ = state.trackUri;
    if (likedSongsQueueQueuedUri_ == state.trackUri) {
        likedSongsQueueQueuedUri_.clear();
        likedSongsQueueQueuedIndex_ = -1;
    }
    queueNextLikedSongsTrack();
}

void MainWindow::queueNextLikedSongsTrack()
{
    if (!likedSongsQueueActive_ || likedSongsQueueAwaitingStart_
        || likedSongsQueuePlayCommandPending_ || likedSongsQueueRequestInFlight_
        || likedSongsQueueFetchInFlight_ || likedSongsQueueCurrentIndex_ < 0
        || playbackState_.trackUri != likedSongsQueueCurrentUri_
        || playbackState_.repeatState == QStringLiteral("track")) {
        return;
    }

    const int nextIndex = likedSongsQueueCurrentIndex_ + 1;
    if (nextIndex >= likedSongsQueueUris_.size()) {
        if (likedSongsQueueNextFetchOffset_ >= 0
            && likedSongsQueueNextFetchOffset_ < likedSongsQueueTotalTracks_) {
            const int offset = likedSongsQueueNextFetchOffset_;
            const quint64 generation = likedSongsQueueGeneration_;
            likedSongsQueueFetchInFlight_ = true;
            spotifyConnection_->fetchSavedTracks(offset,
                [this, generation, offset](const QList<SpotifyTrackResult> &tracks, int nextOffset,
                                           int total, const QString &error) {
                    if (!likedSongsQueueActive_ || generation != likedSongsQueueGeneration_) {
                        return;
                    }
                    likedSongsQueueFetchInFlight_ = false;
                    if (total >= 0) {
                        likedSongsQueueTotalTracks_ = total;
                    }
                    for (const SpotifyTrackResult &track : tracks) {
                        if (track.spotifyUri.startsWith(QStringLiteral("spotify:track:"))) {
                            likedSongsQueueUris_.append(track.spotifyUri);
                        }
                    }
                    if (nextOffset > offset) {
                        likedSongsQueueNextFetchOffset_ = nextOffset;
                    } else {
                        likedSongsQueueNextFetchOffset_ = likedSongsQueueTotalTracks_;
                    }
                    if (!error.isEmpty() && tracks.isEmpty()) {
                        likedSongsQueueNextFetchOffset_ = likedSongsQueueTotalTracks_;
                        return;
                    }
                    queueNextLikedSongsTrack();
                });
        } else if (playbackState_.repeatState == QStringLiteral("context")
                   && !likedSongsQueueUris_.isEmpty()) {
            const quint64 generation = likedSongsQueueGeneration_;
            const int currentIndex = likedSongsQueueCurrentIndex_;
            const QString currentUri = likedSongsQueueCurrentUri_;
            const QString nextUri = likedSongsQueueUris_.first();
            likedSongsQueueRequestInFlight_ = true;
            spotifyConnection_->queuePlaybackTrack(nextUri,
                [this, generation, currentIndex, currentUri, nextUri](bool success) {
                    if (!likedSongsQueueActive_ || generation != likedSongsQueueGeneration_) {
                        return;
                    }
                    likedSongsQueueRequestInFlight_ = false;
                    if (!success) {
                        stopLikedSongsPlayback();
                        return;
                    }
                    if (likedSongsQueueCurrentIndex_ == currentIndex
                        && likedSongsQueueCurrentUri_ == currentUri) {
                        likedSongsQueueQueuedIndex_ = 0;
                        likedSongsQueueQueuedUri_ = nextUri;
                    } else {
                        queueNextLikedSongsTrack();
                    }
                });
        }
        return;
    }

    if (likedSongsQueueQueuedIndex_ == nextIndex
        && likedSongsQueueQueuedUri_ == likedSongsQueueUris_.at(nextIndex)) {
        return;
    }

    const quint64 generation = likedSongsQueueGeneration_;
    const int currentIndex = likedSongsQueueCurrentIndex_;
    const QString currentUri = likedSongsQueueCurrentUri_;
    const QString nextUri = likedSongsQueueUris_.at(nextIndex);
    likedSongsQueueRequestInFlight_ = true;
    spotifyConnection_->queuePlaybackTrack(nextUri,
        [this, generation, currentIndex, currentUri, nextIndex, nextUri](bool success) {
            if (!likedSongsQueueActive_ || generation != likedSongsQueueGeneration_) {
                return;
            }
            likedSongsQueueRequestInFlight_ = false;
            if (!success) {
                stopLikedSongsPlayback();
                return;
            }
            if (likedSongsQueueCurrentIndex_ == currentIndex
                && likedSongsQueueCurrentUri_ == currentUri) {
                likedSongsQueueQueuedIndex_ = nextIndex;
                likedSongsQueueQueuedUri_ = nextUri;
            } else {
                queueNextLikedSongsTrack();
            }
        });
}

void MainWindow::skipTrackInCurrentList(int direction)
{
    if (direction != -1 && direction != 1) {
        return;
    }
    if (!spotifyConnection_) {
        return;
    }

    // Remote Spotify devices keep their own queue, so leave their transport
    // commands on Spotify's Connect path.
    if (playbackState_.externalDevice) {
        direction < 0 ? spotifyConnection_->previousTrack() : spotifyConnection_->nextTrack();
        return;
    }

    QTableView *table = nullptr;
    if (pageStack_->currentIndex() == searchPageIndex) {
        table = searchResultsTable_;
    } else if (pageStack_->currentIndex() == libraryPageIndex) {
        table = libraryTracksTable_;
    } else if (pageStack_->currentIndex() == albumPageIndex) {
        table = albumTracksTable_;
    }
    TrackTableModel *model = modelForTable(table);
    if (!table || !model || !playbackState_.active || playbackState_.trackUri.isEmpty()) {
        direction < 0 ? spotifyConnection_->previousTrack() : spotifyConnection_->nextTrack();
        return;
    }

    const int page = pageStack_->currentIndex();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    int currentRow = -1;
    if (pendingTransportPage_ == page && pendingTransportTrackRow_ >= 0
        && pendingTransportUntilMs_ > nowMs) {
        const SpotifyTrackResult *pendingTrack = model->trackAt(pendingTransportTrackRow_);
        currentRow = pendingTrack && pendingTrack->spotifyUri == pendingTransportTrackUri_
            ? pendingTransportTrackRow_
            : model->rowForUri(pendingTransportTrackUri_, pendingTransportTrackRow_);
    }
    if (currentRow < 0) {
        int preferredRow = -1;
        if (table->currentIndex().isValid()) {
            const SpotifyTrackResult *selectedTrack = model->trackAt(table->currentIndex().row());
            if (selectedTrack && selectedTrack->spotifyUri == playbackState_.trackUri) {
                preferredRow = table->currentIndex().row();
            }
        }
        currentRow = model->rowForUri(playbackState_.trackUri, preferredRow);
    }

    const int targetRow = currentRow + direction;
    const int firstTrackRow = model->firstTrackRow();
    const int endTrackRow = firstTrackRow + model->totalTrackCount();
    const SpotifyTrackResult *targetTrack = targetRow >= firstTrackRow && targetRow < endTrackRow
        ? model->trackAt(targetRow) : nullptr;
    const QString targetUri = targetTrack ? targetTrack->spotifyUri : QString{};
    if (targetUri.isEmpty() || !targetUri.startsWith(QStringLiteral("spotify:track:"))) {
        direction < 0 ? spotifyConnection_->previousTrack() : spotifyConnection_->nextTrack();
        return;
    }

    pendingTransportTrackUri_ = targetUri;
    pendingTransportTrackRow_ = targetRow;
    pendingTransportPage_ = page;
    pendingTransportUntilMs_ = nowMs + 10000;
    lastRequestedTrackUri_ = targetUri;

    QString contextUri;
    if (page == libraryPageIndex && !activeLibraryPlaylistId_.isEmpty()) {
        contextUri = QStringLiteral("spotify:playlist:%1").arg(activeLibraryPlaylistId_);
    } else if (page == albumPageIndex && !currentAlbumPageId_.isEmpty()) {
        contextUri = QStringLiteral("spotify:album:%1").arg(currentAlbumPageId_);
    }
    if (!contextUri.isEmpty()) {
        // Reposition within the source context. This keeps Spotify's full
        // playlist/album queue intact while making each button click follow
        // the row order currently shown in the table.
        spotifyConnection_->playTracks({}, contextUri, targetUri);
        return;
    }

    if (page == libraryPageIndex && activeLibraryPlaylistId_.isEmpty()) {
        startLikedSongsPlaybackAtRow(targetRow);
        return;
    }

    // Saved tracks and search results have no playable context URI, so send a
    // bounded queue beginning at the adjacent visible row.
    QStringList queue;
    for (int row = targetRow; row < endTrackRow && queue.size() < 100; ++row) {
        const SpotifyTrackResult *track = model->trackAt(row);
        if (!track || !track->spotifyUri.startsWith(QStringLiteral("spotify:track:"))) {
            break;
        }
        queue.append(track->spotifyUri);
    }
    if (queue.isEmpty()) {
        pendingTransportTrackUri_.clear();
        pendingTransportTrackRow_ = -1;
        pendingTransportPage_ = -1;
        pendingTransportUntilMs_ = 0;
        direction < 0 ? spotifyConnection_->previousTrack() : spotifyConnection_->nextTrack();
        return;
    }
    spotifyConnection_->playTracks(queue);
}

void MainWindow::focusPlaybackSource()
{
    closeLyricsView();
    const QString trackUri = playbackState_.trackUri;
    if (!playbackState_.active || trackUri.isEmpty()) {
        return;
    }

    if (!playbackSourceAlbumId_.isEmpty()) {
        pendingAlbumFocusTrackUri_ = trackUri;
        pendingLibraryFocusTrackUri_.clear();
        pendingLibraryFocusPlaylistId_.clear();
        if (pageStack_->currentIndex() == albumPageIndex
            && currentAlbumPageId_ == playbackSourceAlbumId_) {
            tryFocusPlaybackTrack();
        } else {
            showAlbumPage(playbackSourceAlbumId_, playbackSourceName_);
        }
        return;
    }

    if (!playbackSourcePlaylistId_.isEmpty()) {
        pendingLibraryFocusTrackUri_ = trackUri;
        pendingLibraryFocusPlaylistId_ = playbackSourcePlaylistId_;
        if (pageStack_->currentIndex() == libraryPageIndex
            && activeLibraryPlaylistId_ == playbackSourcePlaylistId_) {
            tryFocusPlaybackTrack();
        } else {
            showPlaylist(playbackSourcePlaylistId_, playbackSourceName_);
        }
        return;
    }

    if (playbackSourceName_ == QStringLiteral("Liked Songs")) {
        pendingLibraryFocusTrackUri_ = trackUri;
        pendingLibraryFocusPlaylistId_.clear();
        if (pageStack_->currentIndex() == libraryPageIndex
            && activeLibraryPlaylistId_.isEmpty()) {
            tryFocusPlaybackTrack();
        } else {
            showSavedTracks();
        }
        return;
    }

    pendingLibraryFocusTrackUri_.clear();
    pendingLibraryFocusPlaylistId_.clear();
    pendingAlbumFocusTrackUri_.clear();
    if (playbackSourceName_ == QStringLiteral("Search results")) {
        navigateToPage(searchPageIndex);
        const int row = searchResultsModel_->rowForUri(trackUri);
        if (row >= 0) {
            const QModelIndex index = searchResultsModel_->index(row, trackTitleColumn);
            searchResultsTable_->setCurrentIndex(index);
            searchResultsTable_->selectRow(row);
            searchResultsTable_->scrollTo(index, QAbstractItemView::PositionAtCenter);
        }
    }
}

void MainWindow::tryFocusPlaybackTrack()
{
    if (!pendingAlbumFocusTrackUri_.isEmpty()) {
        if (pageStack_->currentIndex() != albumPageIndex
            || currentAlbumPageId_ != playbackSourceAlbumId_) {
            return;
        }
        const int row = albumTracksModel_->rowForUri(pendingAlbumFocusTrackUri_);
        if (row >= 0) {
            const QModelIndex index = albumTracksModel_->index(row, trackTitleColumn);
            albumTracksTable_->setCurrentIndex(index);
            albumTracksTable_->selectRow(row);
            albumTracksTable_->scrollTo(index, QAbstractItemView::PositionAtCenter);
            pendingAlbumFocusTrackUri_.clear();
        }
        return;
    }
    if (pendingLibraryFocusTrackUri_.isEmpty()
        || pageStack_->currentIndex() != libraryPageIndex
        || activeLibraryPlaylistId_ != pendingLibraryFocusPlaylistId_) {
        return;
    }

    const int row = libraryTracksModel_->rowForUri(pendingLibraryFocusTrackUri_);
    if (row >= 0) {
        const QModelIndex index = libraryTracksModel_->index(row, trackTitleColumn);
        libraryTracksTable_->setCurrentIndex(index);
        libraryTracksTable_->selectRow(row);
        libraryTracksTable_->scrollTo(index, QAbstractItemView::PositionAtCenter);
        if (!libraryTracksModel_->sortPending()) {
            pendingLibraryFocusTrackUri_.clear();
            pendingLibraryFocusPlaylistId_.clear();
        }
        return;
    }

    const bool loadingComplete = !libraryBusy_ && !librarySummaryScanActive_
        && pendingLibraryPageOffsets_.isEmpty()
        && libraryTracksModel_->loadedTrackCount() >= libraryTracksModel_->totalTrackCount();
    if (loadingComplete) {
        pendingLibraryFocusTrackUri_.clear();
        pendingLibraryFocusPlaylistId_.clear();
    }
}

void MainWindow::updateNowPlayingPanel(const SpotifyPlaybackState &state)
{
    if (!nowPlayingPanelSourceLabel_ || !nowPlayingPanelTrackArtworkLabel_
        || !nowPlayingPanelTrackTitleLabel_ || !nowPlayingPanelArtistArtworkLabel_
        || !nowPlayingPanelArtistNameLabel_ || !nowPlayingPanelArtistListenersLabel_
        || !nowPlayingPanelArtistInfoLabel_) {
        return;
    }

    const QString fallbackSource = state.externalDevice
        ? QStringLiteral("Spotify Connect") : QStringLiteral("Spotify");
    nowPlayingPanelSourceLabel_->setText(state.active
        ? (playbackSourceName_.isEmpty() ? fallbackSource : playbackSourceName_)
        : QStringLiteral("—"));
    const bool sourceIsNavigable = state.active && !state.trackUri.isEmpty()
        && (!playbackSourcePlaylistId_.isEmpty()
            || !playbackSourceAlbumId_.isEmpty()
            || playbackSourceName_ == QStringLiteral("Liked Songs")
            || playbackSourceName_ == QStringLiteral("Search results"));
    nowPlayingPanelSourceLabel_->setClickable(sourceIsNavigable);
    nowPlayingPanelTrackTitleLabel_->setText(state.active
        ? (state.trackName.isEmpty() ? QStringLiteral("Playing from Spotify") : state.trackName)
        : QStringLiteral("Nothing playing"));
    nowPlayingPanelTrackTitleLabel_->setClickable(state.active && !state.albumId.isEmpty());

    const QString artworkUrl = state.active ? state.artworkUrl : QString{};
    if (displayedPanelArtworkUrl_ != artworkUrl) {
        displayedPanelArtworkUrl_ = artworkUrl;
        ++panelArtworkRequestId_;
        const quint64 requestId = panelArtworkRequestId_;
        nowPlayingPanelTrackArtworkLabel_->clearImage();
        if (!artworkUrl.isEmpty()) {
            QPointer<PanelImageLabel> artworkGuard(nowPlayingPanelTrackArtworkLabel_);
            spotifyConnection_->requestImage(artworkUrl,
                [this, artworkGuard, requestId](const QImage &image) {
                if (!artworkGuard || image.isNull() || requestId != panelArtworkRequestId_) {
                    return;
                }
                artworkGuard->setImage(image);
            });
        }
    }

    QString artistId = state.active ? state.artistId : QString{};
    if (artistId.isEmpty() && state.active && !state.trackUri.isEmpty()) {
        for (TrackTableModel *model : {libraryTracksModel_, searchResultsModel_,
                                       albumTracksModel_}) {
            const int row = model ? model->rowForUri(state.trackUri) : -1;
            const SpotifyTrackResult *track = row >= 0 ? model->trackAt(row) : nullptr;
            if (track && !track->artistId.isEmpty()) {
                artistId = track->artistId;
                break;
            }
        }
    }

    const QString primaryArtist = state.artists.section(QLatin1Char(','), 0, 0).trimmed();
    nowPlayingPanelArtistNameLabel_->setClickable(state.active && !artistId.isEmpty());
    const bool artistProfileRequestNeeded = !artistId.isEmpty()
        && spotifyConnection_->isConnected()
        && requestedNowPlayingArtistProfileId_ != artistId;
    if (artistId != displayedNowPlayingArtistId_ || artistProfileRequestNeeded) {
        displayedNowPlayingArtistId_ = artistId;
        requestedNowPlayingArtistProfileId_.clear();
        ++nowPlayingArtistRequestId_;
        const quint64 requestId = nowPlayingArtistRequestId_;
        nowPlayingPanelArtistArtworkLabel_->clearImage();
        nowPlayingPanelArtistNameLabel_->setText(primaryArtist.isEmpty()
            ? QStringLiteral("Artist") : primaryArtist);
        nowPlayingPanelArtistListenersLabel_->setText(
            QStringLiteral("Monthly listeners unavailable"));
        nowPlayingPanelArtistInfoLabel_->setText(artistId.isEmpty()
            ? (state.active ? QStringLiteral("Artist bio unavailable.")
                            : QStringLiteral("Artist details appear while music plays."))
            : QStringLiteral("Loading artist details…"));

        if (!artistId.isEmpty() && spotifyConnection_->isConnected()) {
            requestedNowPlayingArtistProfileId_ = artistId;
            QPointer<QLabel> nameGuard(nowPlayingPanelArtistNameLabel_);
            QPointer<QLabel> listenersGuard(nowPlayingPanelArtistListenersLabel_);
            QPointer<QLabel> infoGuard(nowPlayingPanelArtistInfoLabel_);
            QPointer<PanelImageLabel> imageGuard(nowPlayingPanelArtistArtworkLabel_);
            spotifyConnection_->fetchArtistProfile(
                artistId, [this, artistId, requestId, nameGuard, listenersGuard,
                           infoGuard, imageGuard]
                (const SpotifyArtistProfile &profile, const QString &error) {
                if (requestId != nowPlayingArtistRequestId_
                    || artistId != displayedNowPlayingArtistId_) {
                    return;
                }
                if (!error.isEmpty()) {
                    if (infoGuard) {
                        infoGuard->setText(QStringLiteral("Artist details could not be loaded."));
                    }
                    return;
                }
                if (nameGuard && !profile.name.isEmpty()) nameGuard->setText(profile.name);
                QString listenersText = QStringLiteral("Monthly listeners unavailable");
                if (profile.followers >= 0) {
                    listenersText += QStringLiteral(" · %1 followers")
                        .arg(QLocale().toString(profile.followers));
                }
                if (listenersGuard) {
                    listenersGuard->setText(listenersText);
                }

                QString details = QStringLiteral("Artist bio isn't available.");
                if (!profile.genres.isEmpty()) {
                    details.prepend(QStringLiteral("Genres: %1\n\n")
                                        .arg(profile.genres.join(QStringLiteral(" · "))));
                }
                if (infoGuard) infoGuard->setText(details);
                if (!profile.imageUrl.isEmpty() && imageGuard) {
                    spotifyConnection_->requestImage(
                        profile.imageUrl, [this, artistId, requestId, imageGuard](const QImage &image) {
                        if (!imageGuard || image.isNull()
                            || requestId != nowPlayingArtistRequestId_
                            || artistId != displayedNowPlayingArtistId_) {
                            return;
                        }
                        imageGuard->setImage(image);
                    });
                }
            });
        }
    } else if (artistId.isEmpty()) {
        nowPlayingPanelArtistNameLabel_->setText(primaryArtist.isEmpty()
            ? QStringLiteral("Artist") : primaryArtist);
        nowPlayingPanelArtistListenersLabel_->setText(
            QStringLiteral("Monthly listeners unavailable"));
    }
}

void MainWindow::updatePlaybackUi(const SpotifyPlaybackState &state, bool fromSpotify)
{
    SpotifyPlaybackState incomingState = state;
    if (fromSpotify && restoredPlaybackPending_ && !state.active
        && playbackState_.active && !playbackState_.trackUri.isEmpty()) {
        incomingState = playbackState_;
        incomingState.deviceReady = state.deviceReady;
        incomingState.audioActivated = state.audioActivated;
        incomingState.deviceId = state.deviceId;
        incomingState.deviceName = state.deviceName;
        incomingState.deviceType = state.deviceType;
        incomingState.deviceVolumePercent = state.deviceVolumePercent;
        incomingState.supportsVolume = state.supportsVolume;
    } else if (fromSpotify && state.active) {
        restoredPlaybackPending_ = false;
    }

    const QString previousTrackUri = playbackState_.trackUri;
    if (incomingState.active && !incomingState.trackUri.isEmpty()
        && (incomingState.trackUri == lastRequestedTrackUri_
            || incomingState.trackUri != previousTrackUri)) {
        lastRequestedTrackUri_.clear();
    }
    SpotifyPlaybackState updatedState = incomingState;
    if (playbackSeekPending_) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (!state.active || state.trackUri != playbackSeekTrackUri_
            || nowMs >= playbackSeekPendingUntilMs_) {
            playbackSeekPending_ = false;
        } else if (qAbs(state.positionMs - playbackSeekTargetMs_) <= 1500) {
            playbackSeekPending_ = false;
        } else {
            updatedState.positionMs = playbackState_.positionMs;
        }
    }
    // Playback metadata from the SDK can omit album/artist identifiers.
    // Reuse the matching loaded track so the now-playing links stay usable.
    if (updatedState.active && !updatedState.trackUri.isEmpty()
        && (updatedState.albumId.isEmpty() || updatedState.artistId.isEmpty())) {
        for (TrackTableModel *model : {libraryTracksModel_, searchResultsModel_,
                                       albumTracksModel_}) {
            const int row = model ? model->rowForPlayback(updatedState) : -1;
            const SpotifyTrackResult *track = row >= 0 ? model->trackAt(row) : nullptr;
            if (!track) {
                continue;
            }
            if (updatedState.albumId.isEmpty()) updatedState.albumId = track->albumId;
            if (updatedState.artistId.isEmpty()) updatedState.artistId = track->artistId;
            if (updatedState.album.isEmpty()) updatedState.album = track->album;
            if (!updatedState.albumId.isEmpty() && !updatedState.artistId.isEmpty()) break;
        }
    }
    playbackState_ = updatedState;
    playbackPositionSampleMs_ = updatedState.positionMs;
    playbackPositionClock_.restart();
    if (updatedState.active && updatedState.externalDevice
        && updatedState.trackUri != previousTrackUri) {
        playbackSourceName_ = QStringLiteral("Spotify Connect");
        playbackSourcePlaylistId_.clear();
        playbackSourceAlbumId_.clear();
    } else if (updatedState.active && playbackSourceName_.isEmpty()) {
        playbackSourceName_ = updatedState.externalDevice
            ? QStringLiteral("Spotify Connect") : QStringLiteral("Spotify");
    }
    updateLikedSongsPlaybackQueue(updatedState);
    updateNowPlayingPanel(updatedState);
    updateLyricsForPlayback(updatedState);
    if (nowPlayingArtworkLabel_
        && displayedPlaybackArtworkUrl_ != updatedState.artworkUrl) {
        displayedPlaybackArtworkUrl_ = updatedState.artworkUrl;
        ++playbackArtworkRequestId_;
        const quint64 requestId = playbackArtworkRequestId_;
        nowPlayingArtworkLabel_->setPixmap(QPixmap{});
        nowPlayingArtworkLabel_->setText(QStringLiteral("♪"));
        if (auto *lyricsPage = findChild<QWidget *>(QStringLiteral("lyricsPage"))) {
            lyricsPage->setStyleSheet(QString{});
        }
        if (!updatedState.artworkUrl.isEmpty()) {
            QPointer<QLabel> artworkGuard(nowPlayingArtworkLabel_);
            spotifyConnection_->requestImage(updatedState.artworkUrl,
                [this, artworkGuard, requestId](const QImage &image) {
                if (!artworkGuard || image.isNull() || requestId != playbackArtworkRequestId_) {
                    return;
                }
                artworkGuard->setText(QString{});
                artworkGuard->setPixmap(QPixmap::fromImage(image).scaled(
                    artworkGuard->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
                if (auto *lyricsPage = findChild<QWidget *>(QStringLiteral("lyricsPage"))) {
                    const QImage colorSamples = image.scaled(
                        24, 24, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    QColor vividArtworkColor(QStringLiteral("#6958c8"));
                    int strongestColorScore = -1;
                    for (int y = 0; y < colorSamples.height(); ++y) {
                        for (int x = 0; x < colorSamples.width(); ++x) {
                            const QColor candidate = colorSamples.pixelColor(x, y);
                            const int brightness = candidate.value();
                            if (brightness < 45 || brightness > 245) {
                                continue;
                            }
                            const int score = candidate.hsvSaturation() * brightness;
                            if (score > strongestColorScore) {
                                strongestColorScore = score;
                                vividArtworkColor = candidate;
                            }
                        }
                    }
                    const int hue = vividArtworkColor.hsvHue() < 0
                        ? 268 : vividArtworkColor.hsvHue();
                    const QColor topTint = QColor::fromHsv(
                        hue, qMax(205, vividArtworkColor.hsvSaturation()),
                        qBound(125, vividArtworkColor.value(), 205));
                    const QColor middleTint = QColor::fromHsv(
                        hue, qMax(180, vividArtworkColor.hsvSaturation()),
                        qMax(72, topTint.value() * 62 / 100));
                    lyricsPage->setStyleSheet(QStringLiteral(
                        "QWidget#lyricsPage { background: qlineargradient("
                        "x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 %1, "
                        "stop: 0.48 %2, stop: 1 #181818); }")
                        .arg(topTint.name(), middleTint.name()));
                }
            });
        }
    }
    updateTrackTablesPlaybackState();
    if (startupWindowPending_ && updatedState.deviceReady && updatedState.audioActivated) {
        showStartupWindow();
    }
    const bool connected = spotifyConnection_ && spotifyConnection_->isConnected();

    const int selectionPage = pageStack_->currentIndex();
    const qint64 selectionTimeMs = QDateTime::currentMSecsSinceEpoch();
    if (pendingTransportUntilMs_ <= selectionTimeMs) {
        pendingTransportTrackUri_.clear();
        pendingTransportTrackRow_ = -1;
        pendingTransportPage_ = -1;
        pendingTransportUntilMs_ = 0;
    }
    const bool pendingTransportMatched = updatedState.active
        && updatedState.trackUri == pendingTransportTrackUri_
        && pendingTransportPage_ == selectionPage
        && pendingTransportTrackRow_ >= 0;
    const bool trackSelectionMissing =
        (selectionPage == searchPageIndex
         && !searchResultsTable_->selectionModel()->hasSelection())
        || (selectionPage == libraryPageIndex
            && !libraryTracksTable_->selectionModel()->hasSelection())
        || (selectionPage == albumPageIndex
            && !albumTracksTable_->selectionModel()->hasSelection());
    if (updatedState.active && !updatedState.trackUri.isEmpty()
        && (updatedState.trackUri != lastPlaybackSelectionTrackUri_
            || selectionPage != lastPlaybackSelectionPage_
            || pendingTransportMatched || trackSelectionMissing)) {
        QTableView *table = selectionPage == 1 ? searchResultsTable_
            : selectionPage == 2 ? libraryTracksTable_
            : selectionPage == albumPageIndex ? albumTracksTable_ : nullptr;
        TrackTableModel *model = modelForTable(table);
        if (table && model) {
            int preferredRow = -1;
            if (table->currentIndex().isValid()) {
                const SpotifyTrackResult *selectedTrack = model->trackAt(table->currentIndex().row());
                if (selectedTrack && selectedTrack->spotifyUri == previousTrackUri) {
                    preferredRow = table->currentIndex().row();
                }
            }
            int row = -1;
            if (pendingTransportMatched) {
                const SpotifyTrackResult *pendingTrack = model->trackAt(pendingTransportTrackRow_);
                row = pendingTrack && pendingTrack->spotifyUri == updatedState.trackUri
                    ? pendingTransportTrackRow_
                    : model->rowForPlayback(updatedState, pendingTransportTrackRow_);
            } else {
                row = model->rowForPlayback(updatedState, preferredRow);
            }
            if (row >= 0) {
                table->setCurrentIndex(model->index(row, trackTitleColumn));
                table->selectRow(row);
                lastPlaybackSelectionTrackUri_ = updatedState.trackUri;
                lastPlaybackSelectionPage_ = selectionPage;
                if (pendingTransportMatched) {
                    pendingTransportTrackUri_.clear();
                    pendingTransportTrackRow_ = -1;
                    pendingTransportPage_ = -1;
                    pendingTransportUntilMs_ = 0;
                }
            } else if (updatedState.trackUri != previousTrackUri) {
                table->clearSelection();
            }
        } else {
            lastPlaybackSelectionTrackUri_ = updatedState.trackUri;
            lastPlaybackSelectionPage_ = selectionPage;
        }
    }

    if (updatedState.active) {
        nowPlayingTitleLabel_->setText(updatedState.trackName.isEmpty()
                                           ? QStringLiteral("Playing from Spotify")
                                           : updatedState.trackName);
        nowPlayingTitleLabel_->setClickable(!updatedState.albumId.isEmpty());
        nowPlayingArtistLabel_->setText(updatedState.artists.isEmpty()
                                            ? (updatedState.album.isEmpty()
                                                   ? updatedState.deviceName : updatedState.album)
                                            : updatedState.artists);
        nowPlayingArtistLabel_->setClickable(!displayedNowPlayingArtistId_.isEmpty());
    } else if (updatedState.externalDevice && !updatedState.deviceName.isEmpty()) {
        nowPlayingTitleLabel_->setText(QStringLiteral("Spotify is active on another device"));
        nowPlayingTitleLabel_->setClickable(false);
        nowPlayingArtistLabel_->setText(updatedState.deviceName);
        nowPlayingArtistLabel_->setClickable(false);
    } else {
        nowPlayingTitleLabel_->setText(QStringLiteral("No track selected"));
        nowPlayingTitleLabel_->setClickable(false);
        nowPlayingArtistLabel_->setClickable(false);
        if (!connected) {
            nowPlayingArtistLabel_->setText(QStringLiteral("Connect Spotify to start listening"));
        } else if (!updatedState.deviceReady) {
            nowPlayingArtistLabel_->setText(QStringLiteral("Select a track and press Play"));
        } else if (!updatedState.audioActivated) {
            nowPlayingArtistLabel_->setText(QStringLiteral("Starting the headless playback browser…"));
        } else {
            nowPlayingArtistLabel_->setText(QStringLiteral("Choose a track from Search or Your Library"));
        }
    }

    const bool hasTargetDevice = !updatedState.deviceId.isEmpty()
        && !updatedState.deviceRestricted;
    const bool localDeviceReady = updatedState.deviceReady && updatedState.audioActivated;
    const bool canControl = connected && (hasTargetDevice || localDeviceReady);
    playPauseButton_->setEnabled(connected);
    previousTrackButton_->setEnabled(canControl && updatedState.active);
    nextTrackButton_->setEnabled(canControl && updatedState.active);
    repeatButton_->setEnabled(canControl && updatedState.active);
    playbackProgressSlider_->setEnabled(canControl && updatedState.active
                                         && updatedState.durationMs > 0);
    playbackVolumeSlider_->setEnabled(canControl
        && (!updatedState.externalDevice || updatedState.supportsVolume));
    volumeMuteButton_->setEnabled(playbackVolumeSlider_->isEnabled());
    deviceButton_->setEnabled(connected);
    deviceButton_->setVisible(updatedState.externalDevice && updatedState.active
                              && !updatedState.paused);
    deviceButton_->setToolTip(updatedState.deviceName.isEmpty()
                                  ? QStringLiteral("Choose a Spotify Connect device")
                                  : QStringLiteral("Playing on %1. Click to switch devices.")
                                        .arg(updatedState.deviceName));
    if (updatedState.deviceVolumePercent >= 0 && !playbackVolumeSlider_->isSliderDown()) {
        const int sliderVolume = volumePercentToSliderPosition(updatedState.deviceVolumePercent);
        const QSignalBlocker signalBlocker(playbackVolumeSlider_);
        playbackVolumeSlider_->setValue(sliderVolume);
        if (updatedState.deviceVolumePercent > 0) {
            lastNonZeroVolume_ = sliderVolume;
        }
        const bool muted = updatedState.deviceVolumePercent == 0;
        volumeMuteButton_->setIcon(volumeControlIcon(muted));
        volumeMuteButton_->setAccessibleName(muted ? QStringLiteral("Unmute")
                                                    : QStringLiteral("Mute"));
        volumeMuteButton_->setToolTip(muted ? QStringLiteral("Unmute")
                                            : QStringLiteral("Mute"));
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool playbackIsRunning = updatedState.active && !updatedState.paused;
    const bool observedPaused = !playbackIsRunning;
    if (playbackUiCommandPending_) {
        const bool targetTrackMatched = playbackUiRequestedTrackUri_.isEmpty()
            || updatedState.trackUri == playbackUiRequestedTrackUri_;
        if ((observedPaused == playbackUiRequestedPaused_ && targetTrackMatched)
            || nowMs >= playbackUiCommandUntilMs_) {
            playbackUiCommandPending_ = false;
        }
    }
    const bool showPausedIcon = playbackUiCommandPending_
        ? playbackUiRequestedPaused_ : observedPaused;
    playPauseButton_->setIcon(playbackControlIcon(showPausedIcon ? 1 : 2,
                                                   Qt::black, Qt::black));
    playPauseButton_->setToolTip(showPausedIcon ? QStringLiteral("Play")
                                                : QStringLiteral("Pause"));

    QString repeatMode = updatedState.repeatState;
    if (!playbackRepeatModeOverride_.isEmpty()) {
        if (updatedState.repeatState == playbackRepeatModeOverride_
            || nowMs >= playbackRepeatModeOverrideUntilMs_) {
            playbackRepeatModeOverride_.clear();
        } else {
            repeatMode = playbackRepeatModeOverride_;
        }
    }
    repeatButton_->setIcon(repeatModeIcon(repeatMode));
    repeatButton_->setChecked(repeatMode != QStringLiteral("off"));
    repeatButton_->setToolTip(repeatMode == QStringLiteral("off")
        ? QStringLiteral("Repeat off")
        : repeatMode == QStringLiteral("track")
            ? QStringLiteral("Repeat track") : QStringLiteral("Repeat context"));

    playbackProgressSlider_->setRange(0, qMax(0, updatedState.durationMs));
    if (!playbackProgressSlider_->isSliderDown()) {
        playbackProgressSlider_->setValue(qBound(0, updatedState.positionMs,
                                                qMax(0, updatedState.durationMs)));
        playbackPositionLabel_->setText(formatPlaybackTime(updatedState.positionMs));
    }
    playbackDurationLabel_->setText(formatPlaybackTime(updatedState.durationMs));

    if (updatedState.active && !updatedState.paused) {
        if (!playbackProgressTimer_->isActive()) {
            playbackProgressTimer_->start();
        }
    } else {
        playbackProgressTimer_->stop();
    }
}

void MainWindow::restoreSavedPlayback()
{
    QSettings settings;
    const QString trackUri = settings.value(QStringLiteral("playback/trackUri")).toString();
    if (!settings.value(QStringLiteral("playback/active"), false).toBool()
        || trackUri.isEmpty()) {
        return;
    }

    SpotifyPlaybackState restored;
    restored.active = true;
    restored.paused = true;
    restored.trackUri = trackUri;
    restored.trackName = settings.value(QStringLiteral("playback/trackName")).toString();
    restored.artists = settings.value(QStringLiteral("playback/artists")).toString();
    restored.artistId = settings.value(QStringLiteral("playback/artistId")).toString();
    restored.album = settings.value(QStringLiteral("playback/album")).toString();
    restored.albumId = settings.value(QStringLiteral("playback/albumId")).toString();
    restored.artworkUrl = settings.value(QStringLiteral("playback/artworkUrl")).toString();
    restored.durationMs = qMax(0, settings.value(QStringLiteral("playback/durationMs")).toInt());
    restored.positionMs = qBound(0,
        settings.value(QStringLiteral("playback/positionMs")).toInt(),
        qMax(0, restored.durationMs));
    restoredPlaybackPositionMs_ = restored.positionMs;
    playbackSourceName_ = settings.value(QStringLiteral("playback/sourceName")).toString();
    playbackSourcePlaylistId_ = settings.value(
        QStringLiteral("playback/sourcePlaylistId")).toString();
    playbackSourceAlbumId_ = settings.value(
        QStringLiteral("playback/sourceAlbumId")).toString();
    restoredPlaybackPending_ = true;
    updatePlaybackUi(restored);
}

void MainWindow::showStartupWindow()
{
    if (!startupWindowPending_) {
        return;
    }
    startupWindowPending_ = false;
    show();
    raise();
    activateWindow();
}

void MainWindow::closeLyricsView()
{
    if (lyricsButton_ && lyricsButton_->isChecked()) {
        lyricsButton_->setChecked(false);
    }
}

void MainWindow::updateLyricsForPlayback(const SpotifyPlaybackState &state)
{
    if (!lyricsViewOpen_) {
        return;
    }

    if (!state.active || (state.trackUri.isEmpty() && state.trackName.isEmpty())) {
        ++lyricsRequestId_;
        lyricsRequestInFlight_ = false;
        lyricsLoaded_ = false;
        lyricsScrollAnimation_->stop();
        lyricsColorAnimation_->stop();
        lyricsTrackUri_.clear();
        currentLyricLineIndex_ = -1;
        lyricLineStartTimesMs_.clear();
        lyricLineStartColors_.clear();
        lyricLineTargetColors_.clear();
        lyricsList_->clear();
        lyricsTrackLabel_->setText(QStringLiteral("Nothing playing"));
        lyricsStatusLabel_->setText(QStringLiteral("Play a track to find its lyrics."));
        lyricsSyncStatusLabel_->clear();
        lyricsContentStack_->setCurrentIndex(0);
        return;
    }

    const QString trackIdentity = !state.trackUri.isEmpty()
        ? state.trackUri
        : state.trackName + QLatin1Char('\n') + state.artists;
    if (trackIdentity == lyricsTrackUri_ && (lyricsRequestInFlight_ || lyricsLoaded_)) {
        updateLyricsHighlight();
        return;
    }

    lyricsTrackUri_ = trackIdentity;
    const quint64 requestId = ++lyricsRequestId_;
    lyricsRequestInFlight_ = true;
    lyricsLoaded_ = false;
    lyricsScrollAnimation_->stop();
    lyricsColorAnimation_->stop();
    currentLyricLineIndex_ = -1;
    lyricLineStartTimesMs_.clear();
    lyricLineStartColors_.clear();
    lyricLineTargetColors_.clear();
    lyricsList_->clear();
    lyricsTrackLabel_->setText(state.artists.isEmpty()
        ? state.trackName
        : QStringLiteral("%1 — %2").arg(state.trackName, state.artists));
    lyricsStatusLabel_->setText(QStringLiteral("Looking up lyrics…"));
    lyricsSyncStatusLabel_->clear();
    lyricsContentStack_->setCurrentIndex(0);

    if (!spotifyConnection_) {
        lyricsRequestInFlight_ = false;
        lyricsLoaded_ = true;
        lyricsStatusLabel_->setText(QStringLiteral("The lyrics service is unavailable."));
        return;
    }

    spotifyConnection_->fetchTrackLyrics(
        state.trackUri, state.trackName, state.artists, state.album, state.durationMs,
        [this, requestId, trackIdentity](const TrackLyrics &lyrics, const QString &error) {
            if (requestId != lyricsRequestId_ || trackIdentity != lyricsTrackUri_) {
                return;
            }
            lyricsRequestInFlight_ = false;
            lyricsLoaded_ = true;
            displayLyrics(lyrics, error);
        });
}

void MainWindow::displayLyrics(const TrackLyrics &lyrics, const QString &errorMessage)
{
    lyricsColorAnimation_->stop();
    lyricsList_->clear();
    lyricLineStartTimesMs_.clear();
    lyricLineStartColors_.clear();
    lyricLineTargetColors_.clear();
    currentLyricLineIndex_ = -1;
    lyricsSyncStatusLabel_->clear();

    if (!errorMessage.isEmpty()) {
        lyricsStatusLabel_->setText(errorMessage);
        lyricsContentStack_->setCurrentIndex(0);
        return;
    }
    if (lyrics.instrumental) {
        lyricsStatusLabel_->setText(QStringLiteral("Instrumental track — no lyrics."));
        lyricsContentStack_->setCurrentIndex(0);
        return;
    }

    QVector<QPair<int, QString>> lines;
    bool hasTimedLyrics = false;
    if (!lyrics.syncedLyrics.trimmed().isEmpty()) {
        static const QRegularExpression timestampPattern(
            QStringLiteral(R"(\[(\d+):(\d{2})(?:\.(\d{1,3}))?\])"));
        const QStringList sourceLines = lyrics.syncedLyrics.split(QLatin1Char('\n'));
        for (QString sourceLine : sourceLines) {
            sourceLine = sourceLine.trimmed();
            if (sourceLine.isEmpty()) {
                continue;
            }

            QVector<int> timestamps;
            auto matches = timestampPattern.globalMatch(sourceLine);
            while (matches.hasNext()) {
                const QRegularExpressionMatch match = matches.next();
                const int minutes = match.captured(1).toInt();
                const int seconds = match.captured(2).toInt();
                const QString fraction = match.captured(3).leftJustified(3, QLatin1Char('0'));
                const int milliseconds = fraction.isEmpty() ? 0 : fraction.left(3).toInt();
                timestamps.append(((minutes * 60) + seconds) * 1000 + milliseconds);
            }

            const QString lyricText = sourceLine.replace(timestampPattern, QString{}).trimmed();
            if (lyricText.isEmpty() || (lyricText.startsWith(QLatin1Char('['))
                                        && lyricText.endsWith(QLatin1Char(']')))) {
                continue;
            }
            if (timestamps.isEmpty()) {
                lines.append(qMakePair(-1, lyricText));
            } else {
                hasTimedLyrics = true;
                for (const int timestamp : timestamps) {
                    lines.append(qMakePair(timestamp, lyricText));
                }
            }
        }
    }

    if (lines.isEmpty() && !lyrics.plainLyrics.trimmed().isEmpty()) {
        const QStringList plainLines = lyrics.plainLyrics.split(QLatin1Char('\n'));
        for (const QString &line : plainLines) {
            const QString trimmedLine = line.trimmed();
            if (!trimmedLine.isEmpty()) {
                lines.append(qMakePair(-1, trimmedLine));
            }
        }
    }

    if (lines.isEmpty()) {
        lyricsStatusLabel_->setText(QStringLiteral("No lyrics are available for this track."));
        lyricsContentStack_->setCurrentIndex(0);
        return;
    }

    lyricsStatusLabel_->clear();
    if (!hasTimedLyrics) {
        lyricsSyncStatusLabel_->setText(QStringLiteral("These lyrics are not synced to playback."));
    }
    for (const auto &line : lines) {
        auto *item = new QListWidgetItem(line.second, lyricsList_);
        item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        item->setForeground(QBrush(QColor(QStringLiteral("#b3b3b3"))));
        QFont font = item->font();
        font.setPointSize(20);
        font.setWeight(QFont::DemiBold);
        item->setFont(font);
        item->setSizeHint(QSize(0, 68));
        lyricLineStartTimesMs_.append(line.first);
    }
    updateLyricsColors(false);
    lyricsContentStack_->setCurrentIndex(1);
    updateLyricsHighlight();
}

void MainWindow::updateLyricsColors(bool animate)
{
    if (!lyricsList_ || lyricsList_->count() == 0
        || lyricsList_->count() != lyricLineStartTimesMs_.size()) {
        return;
    }

    const QColor sungColor(QStringLiteral("#b3b3b3"));
    const QColor upcomingColor(Qt::white);
    int activeStartTimeMs = -1;
    for (const int startTimeMs : lyricLineStartTimesMs_) {
        if (startTimeMs >= 0 && startTimeMs <= playbackState_.positionMs) {
            activeStartTimeMs = qMax(activeStartTimeMs, startTimeMs);
        }
    }
    QVector<QColor> targets;
    targets.reserve(lyricLineStartTimesMs_.size());
    for (const int startTimeMs : lyricLineStartTimesMs_) {
        if (startTimeMs < 0) {
            // Plain or untimed lines cannot be classified by playback position.
            targets.append(sungColor);
        } else {
            targets.append(activeStartTimeMs >= 0 && startTimeMs < activeStartTimeMs
                               ? sungColor : upcomingColor);
        }
    }

    const bool colorsInitialized = lyricLineTargetColors_.size() == targets.size()
        && lyricLineStartColors_.size() == targets.size();
    if (!animate || !colorsInitialized) {
        lyricsColorAnimation_->stop();
        lyricLineStartColors_ = targets;
        lyricLineTargetColors_ = targets;
        for (int index = 0; index < targets.size(); ++index) {
            lyricsList_->item(index)->setForeground(QBrush(targets.at(index)));
        }
        return;
    }

    bool changed = false;
    for (int index = 0; index < targets.size(); ++index) {
        if (targets.at(index) != lyricLineTargetColors_.at(index)) {
            changed = true;
            break;
        }
    }
    if (!changed) {
        return;
    }

    lyricsColorAnimation_->stop();
    lyricLineStartColors_.clear();
    lyricLineStartColors_.reserve(targets.size());
    for (int index = 0; index < targets.size(); ++index) {
        lyricLineStartColors_.append(lyricsList_->item(index)->foreground().color());
    }
    lyricLineTargetColors_ = targets;
    lyricsColorAnimation_->setStartValue(0.0);
    lyricsColorAnimation_->setEndValue(1.0);
    lyricsColorAnimation_->start();
}

void MainWindow::refreshPlaybackPosition()
{
    if (!playbackState_.active || playbackState_.paused) {
        return;
    }

    const qint64 elapsedMs = playbackPositionClock_.isValid()
        ? playbackPositionClock_.elapsed() : 0;
    const qint64 estimatedPositionMs = playbackPositionSampleMs_ + elapsedMs;
    playbackState_.positionMs = qMin(playbackState_.durationMs,
        static_cast<int>(qMin<qint64>(estimatedPositionMs,
                                     std::numeric_limits<int>::max())));
}

void MainWindow::updateLyricsHighlight()
{
    if (!lyricsViewOpen_ || !lyricsList_ || lyricsList_->count() == 0
        || lyricLineStartTimesMs_.isEmpty()) {
        return;
    }
    refreshPlaybackPosition();
    updateLyricsColors(true);

    int activeLine = -1;
    int latestStartTimeMs = -1;
    for (int index = 0; index < lyricLineStartTimesMs_.size(); ++index) {
        const int startTimeMs = lyricLineStartTimesMs_.at(index);
        if (startTimeMs >= 0 && startTimeMs <= playbackState_.positionMs
            && startTimeMs >= latestStartTimeMs) {
            activeLine = index;
            latestStartTimeMs = startTimeMs;
        }
    }
    if (activeLine == currentLyricLineIndex_) {
        return;
    }

    const auto setLineStyle = [this](int index, bool active) {
        QListWidgetItem *item = lyricsList_->item(index);
        if (!item) {
            return;
        }
        QFont font = item->font();
        font.setPointSize(active ? 24 : 20);
        font.setWeight(active ? QFont::Bold : QFont::DemiBold);
        item->setFont(font);
    };

    if (currentLyricLineIndex_ >= 0 && currentLyricLineIndex_ < lyricsList_->count()) {
        setLineStyle(currentLyricLineIndex_, false);
    }
    currentLyricLineIndex_ = activeLine;
    if (activeLine >= 0) {
        setLineStyle(activeLine, true);
        QScrollBar *scrollBar = lyricsList_->verticalScrollBar();
        const int currentScrollPosition = scrollBar->value();
        lyricsList_->scrollToItem(lyricsList_->item(activeLine),
                                  QAbstractItemView::PositionAtCenter);
        const int targetScrollPosition = scrollBar->value();
        scrollBar->setValue(currentScrollPosition);
        if (targetScrollPosition != currentScrollPosition) {
            lyricsScrollAnimation_->stop();
            lyricsScrollAnimation_->setStartValue(currentScrollPosition);
            lyricsScrollAnimation_->setEndValue(targetScrollPosition);
            lyricsScrollAnimation_->start();
        }
    } else if (currentLyricLineIndex_ < 0) {
        if (lyricsList_->verticalScrollBar()->value() != 0) {
            lyricsScrollAnimation_->stop();
            lyricsScrollAnimation_->setStartValue(lyricsList_->verticalScrollBar()->value());
            lyricsScrollAnimation_->setEndValue(0);
            lyricsScrollAnimation_->start();
        }
    }
}

void MainWindow::updatePlaybackProgress()
{
    if (!playbackState_.active || playbackState_.paused) {
        playbackProgressTimer_->stop();
        updateLyricsHighlight();
        return;
    }
    refreshPlaybackPosition();
    if (!playbackProgressSlider_->isSliderDown()) {
        playbackProgressSlider_->setValue(playbackState_.positionMs);
        playbackPositionLabel_->setText(formatPlaybackTime(playbackState_.positionMs));
    }
    if (playbackState_.durationMs > 0 && playbackState_.positionMs >= playbackState_.durationMs) {
        playbackProgressTimer_->stop();
    }
    updateLyricsHighlight();
}
