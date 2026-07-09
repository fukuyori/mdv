#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCommandLineParser>
#include <QDataStream>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QEasingCurve>
#include <QFileSystemWatcher>
#include <QFont>
#include <QFontDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStringConverter>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include <algorithm>
#include <climits>
#include <functional>

#include "md4c-html.h"

#ifndef MDV_VERSION
#define MDV_VERSION "0.3.1"
#endif

// ---------------------------------------------------------------------------
// Free helpers shared by DocumentTab and independent of any window/tab state.
// ---------------------------------------------------------------------------

static QString markdownToHtml(const QString &markdown)
{
    const QByteArray utf8 = markdown.toUtf8();
    QByteArray html;
    html.reserve(utf8.size() * 2);

    md_html(
        utf8.constData(),
        MD_SIZE(utf8.size()),
        [](const MD_CHAR *text, MD_SIZE size, void *userdata) {
            static_cast<QByteArray *>(userdata)->append(text, qsizetype(size));
        },
        &html,
        MD_DIALECT_GITHUB,
        0);

    return QString::fromUtf8(html);
}

struct Heading {
    int level = 0;
    QString title;
};

static Heading parseHeading(const QString &line)
{
    int index = 0;
    while (index < line.size() && line.at(index).isSpace()) {
        ++index;
    }

    int level = 0;
    while (index + level < line.size() && line.at(index + level) == QLatin1Char('#') && level < 6) {
        ++level;
    }

    if (level == 0 || index + level >= line.size() || !line.at(index + level).isSpace()) {
        return {};
    }

    QString title = line.mid(index + level).trimmed();
    while (title.endsWith(QLatin1Char('#'))) {
        title.chop(1);
    }

    title = title.trimmed();
    if (title.isEmpty()) {
        title = QStringLiteral("(untitled)");
    }

    return {level, title};
}

struct MdBlock {
    QString text;
    int position = 0; // char offset of the block's first line in the source
};

// Blocks are groups of lines separated by blank lines, except that fenced
// code stays together; this is the unit of translation and of pairing in
// the bilingual view.
static QList<MdBlock> splitMarkdownBlocks(const QString &markdown)
{
    QList<MdBlock> blocks;
    QStringList current;
    int currentStart = 0;
    int lineStart = 0;
    bool inFence = false;

    const QStringList lines = markdown.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (!inFence && trimmed.isEmpty()) {
            if (!current.isEmpty()) {
                blocks.append({current.join(QLatin1Char('\n')), currentStart});
                current.clear();
            }
        } else {
            if (current.isEmpty()) {
                currentStart = lineStart;
            }
            current.append(line);
            if (trimmed.startsWith(QLatin1String("```")) || trimmed.startsWith(QLatin1String("~~~"))) {
                inFence = !inFence;
            }
        }
        lineStart += line.size() + 1;
    }
    if (!current.isEmpty()) {
        blocks.append({current.join(QLatin1Char('\n')), currentStart});
    }

    return blocks;
}

// Code blocks and symbol-only blocks (horizontal rules etc.) pass through
// untranslated.
static bool blockIsTranslatable(const QString &block)
{
    const QString trimmed = block.trimmed();
    if (trimmed.startsWith(QLatin1String("```")) || trimmed.startsWith(QLatin1String("~~~"))) {
        return false;
    }
    for (const QChar c : trimmed) {
        if (c.isLetter()) {
            return true;
        }
    }
    return false;
}

// Sort key for translation order: blocks from the viewport top downward
// come first (in reading order), blocks above the viewport are weighted
// to run later.
static int translationPriority(int blockPosition, int viewportTopPosition)
{
    return blockPosition >= viewportTopPosition
        ? blockPosition - viewportTopPosition
        : (viewportTopPosition - blockPosition) * 3;
}

static QString cssQuotedFontFamily(const QString &family)
{
    QString escaped = family;
    escaped.replace("\\", "\\\\");
    escaped.replace("'", "\\'");
    return "'" + escaped + "'";
}

static QString defaultOllamaEndpoint()
{
    return QStringLiteral("http://127.0.0.1:11434");
}

static QString cleanedEndpoint(const QString &endpoint)
{
    QString cleaned = endpoint.trimmed();
    while (cleaned.endsWith(QLatin1Char('/'))) {
        cleaned.chop(1);
    }
    return cleaned.isEmpty() ? defaultOllamaEndpoint() : cleaned;
}

static QString defaultEditorFontFamily()
{
    QFont font("Menlo");
    font.setStyleHint(QFont::Monospace);
    return font.family();
}

static QString defaultPreviewFontFamily()
{
    return QApplication::font().family();
}

// The working directory is the natural starting point when launched from
// a terminal; Finder-launched apps run with cwd "/", where home is nicer.
static QString defaultDialogDir()
{
    const QString cwd = QDir::currentPath();
    return (cwd.isEmpty() || cwd == QLatin1String("/")) ? QDir::homePath() : cwd;
}

// qwebchannel.js is inlined instead of referenced via qrc:// so the
// file://-based page does not depend on cross-scheme access rules.
static QString webChannelScript()
{
    static const QString script = [] {
        QFile file(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
        if (!file.open(QIODevice::ReadOnly)) {
            return QString();
        }
        return QString::fromUtf8(file.readAll());
    }();
    return script;
}

static QString previewScript()
{
    return QStringLiteral(
        "var __mdvBridge = null;"
        "var __mdvProgTs = 0;"
        "new QWebChannel(qt.webChannelTransport, function(channel) {"
        "  __mdvBridge = channel.objects.mdv;"
        "});"
        "function __mdvHeadings() {"
        "  return document.querySelectorAll('#content h1,#content h2,#content h3,"
        "#content h4,#content h5,#content h6');"
        "}"
        "function __mdvAnchorYs(hs) {"
        "  var ys = [0];"
        "  for (var i = 0; i < hs.length; i++)"
        "    ys.push(hs[i].getBoundingClientRect().top + window.scrollY);"
        "  ys.push(document.documentElement.scrollHeight);"
        "  return ys;"
        "}"
        "function __mdvSetContent(html) {"
        "  var c = document.getElementById('content');"
        "  c.innerHTML = html;"
        "  var used = {};"
        "  var hs = c.querySelectorAll('h1,h2,h3,h4,h5,h6');"
        "  for (var i = 0; i < hs.length; i++) {"
        "    if (hs[i].id) continue;"
        "    var slug = hs[i].textContent.trim().toLowerCase()"
        "      .replace(/[^0-9a-z\\u00a0-\\uffff\\- ]/g, '')"
        "      .replace(/ +/g, '-') || 'section';"
        "    var id = slug, n = 1;"
        "    while (used[id]) { id = slug + '-' + (n++); }"
        "    used[id] = 1;"
        "    hs[i].id = id;"
        "  }"
        "}"
        "function __mdvSync(count, segment, t, fraction) {"
        "  var hs = __mdvHeadings();"
        "  var total = document.documentElement.scrollHeight;"
        "  var y;"
        "  if (count > 0 && hs.length === count) {"
        "    var ys = __mdvAnchorYs(hs);"
        "    y = ys[segment] + t * (ys[segment + 1] - ys[segment]);"
        "  } else {"
        "    y = fraction * total;"
        "  }"
        "  y = Math.max(0, Math.min(y, total - window.innerHeight));"
        "  if (Math.abs(window.scrollY - y) < 1) return;"
        "  __mdvProgTs = Date.now();"
        "  window.scrollTo(0, y);"
        "}"
        "var __mdvRaf = false;"
        "window.addEventListener('scroll', function() {"
        "  if (Date.now() - __mdvProgTs < 150) return;"
        "  if (__mdvRaf) return;"
        "  __mdvRaf = true;"
        "  requestAnimationFrame(function() {"
        "    __mdvRaf = false;"
        "    if (!__mdvBridge) return;"
        "    if (Date.now() - __mdvProgTs < 150) return;"
        "    var hs = __mdvHeadings();"
        "    var ys = __mdvAnchorYs(hs);"
        "    var sy = window.scrollY;"
        "    var total = Math.max(1, document.documentElement.scrollHeight);"
        "    var segment = ys.length - 2, t = 1;"
        "    for (var i = 0; i + 1 < ys.length; i++) {"
        "      if (sy > ys[i + 1]) continue;"
        "      var span = ys[i + 1] - ys[i];"
        "      t = span > 0 ? (sy - ys[i]) / span : 0;"
        "      segment = i;"
        "      break;"
        "    }"
        "    __mdvBridge.previewScrolled(hs.length, segment, t, sy / total);"
        "  });"
        "}, {passive: true});");
}

// JS-to-C++ channel: the preview page reports user scrolls through this
// object so the editor can follow.
class PreviewBridge : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    std::function<void(int, int, double, double)> onScrolled;

public slots:
    void previewScrolled(int headingCount, int segment, double t, double fraction)
    {
        if (onScrolled) {
            onScrolled(headingCount, segment, t, fraction);
        }
    }
};

// Keeps the preview locked to the document rendered via setHtml: clicked
// links open in the system browser instead of navigating the preview away,
// while in-page anchor jumps keep working.
class PreviewPage : public QWebEnginePage {
public:
    using QWebEnginePage::QWebEnginePage;

protected:
    bool acceptNavigationRequest(const QUrl &requestedUrl, NavigationType type, bool isMainFrame) override
    {
        Q_UNUSED(isMainFrame);

        if (type != NavigationTypeLinkClicked) {
            return true;
        }
        if (requestedUrl.hasFragment() && requestedUrl.matches(url(), QUrl::RemoveFragment)) {
            return true;
        }

        const QString scheme = requestedUrl.scheme();
        if (scheme == "http" || scheme == "https" || scheme == "mailto") {
            QDesktopServices::openUrl(requestedUrl);
        }
        return false;
    }
};

// Translates markdown fragments through a local Ollama server, keeping up
// to a few requests in flight so the HTTP round-trip overlaps generation.
// Results are keyed, so out-of-order completion is fine. A single instance
// is shared by every open document tab; jobs from different tabs interleave
// in the same queue and can be cancelled selectively by key via cancelKeys().
class OllamaTranslator : public QObject {
    Q_OBJECT

public:
    explicit OllamaTranslator(QObject *parent = nullptr)
        : QObject(parent)
        , manager_(new QNetworkAccessManager(this))
    {
        // Local models can take minutes per block on CPU-only machines.
        manager_->setTransferTimeout(300000);
    }

    void configure(const QString &endpoint, const QString &model, const QString &targetLanguage)
    {
        endpoint_ = endpoint;
        model_ = model;
        targetLanguage_ = targetLanguage;
    }

    void setMaxInFlight(int count)
    {
        maxInFlight_ = qBound(1, count, 8);
        fillSlots();
    }

    void requestTranslation(const QString &key, const QString &markdown)
    {
        queue_.append({key, markdown});
        fillSlots();
    }

    // Reorders waiting jobs so the ones nearest the reader's viewport run
    // first; keys not listed keep their relative order at the back.
    void prioritize(const QStringList &orderedKeys)
    {
        if (queue_.size() < 2) {
            return;
        }
        QHash<QString, int> rank;
        rank.reserve(orderedKeys.size());
        for (int i = 0; i < orderedKeys.size(); ++i) {
            rank.insert(orderedKeys.at(i), i);
        }
        std::stable_sort(queue_.begin(), queue_.end(), [&rank](const Job &a, const Job &b) {
            return rank.value(a.key, INT_MAX) < rank.value(b.key, INT_MAX);
        });
    }

    void cancelAll()
    {
        queue_.clear();
        cancelling_ = true;
        const QList<QNetworkReply *> active = active_.values();
        for (QNetworkReply *reply : active) {
            reply->abort();
        }
        cancelling_ = false;
    }

    // Cancels only jobs matching the given keys (queued or in flight),
    // leaving other tabs' jobs untouched.
    void cancelKeys(const QSet<QString> &keys)
    {
        if (keys.isEmpty()) {
            return;
        }

        queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
            [&keys](const Job &job) { return keys.contains(job.key); }), queue_.end());

        cancelling_ = true;
        for (auto it = activeKeys_.constBegin(); it != activeKeys_.constEnd(); ++it) {
            if (keys.contains(it.value())) {
                it.key()->abort();
            }
        }
        cancelling_ = false;
    }

signals:
    void translated(const QString &key, const QString &markdown);
    void blockFailed(const QString &key, const QString &error);
    void failed(const QString &error);

private:
    struct Job {
        QString key;
        QString text;
    };

    void fillSlots()
    {
        while (active_.size() < maxInFlight_ && !queue_.isEmpty()) {
            sendOne(queue_.takeFirst());
        }
    }

    void sendOne(const Job &job)
    {
        QNetworkRequest request(QUrl(endpoint_ + "/api/chat"));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

        const QJsonObject payload{
            {"model", model_},
            {"stream", false},
            {"messages", QJsonArray{
                QJsonObject{{"role", "system"}, {"content", systemPrompt()}},
                QJsonObject{{"role", "user"}, {"content", job.text}},
            }},
            {"options", QJsonObject{{"temperature", 0.2}}},
        };

        QNetworkReply *reply = manager_->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        active_.insert(reply);
        activeKeys_.insert(reply, job.key);
        connect(reply, &QNetworkReply::finished, this, [this, job, reply] {
            active_.remove(reply);
            activeKeys_.remove(reply);
            reply->deleteLater();

            // Deliberate cancellation (mode switch, shutdown): drop silently.
            if (reply->error() == QNetworkReply::OperationCanceledError && cancelling_) {
                return;
            }

            const QByteArray body = reply->readAll();
            if (reply->error() != QNetworkReply::NoError) {
                if (isFatal(reply->error())) {
                    // Server unreachable: nothing else will succeed either.
                    cancelAll();
                    emit failed(reply->errorString());
                    return;
                }
                // Per-block problem (transient 500, timeout, oversized
                // block): skip this block and keep going. Ollama reports the
                // useful message ("model not found" etc.) in the response
                // body rather than the HTTP reason phrase.
                const QString serverError = QJsonDocument::fromJson(body).object().value("error").toString();
                emit blockFailed(job.key, serverError.isEmpty() ? reply->errorString() : serverError);
                fillSlots();
                return;
            }

            QString text = QJsonDocument::fromJson(body).object()
                .value("message").toObject().value("content").toString();
            // Reasoning models may prefix their answer with a think block.
            static const QRegularExpression thinkBlock(
                QStringLiteral("<think>.*</think>"),
                QRegularExpression::DotMatchesEverythingOption);
            text.remove(thinkBlock);
            text = text.trimmed();

            if (text.isEmpty()) {
                emit blockFailed(job.key, QStringLiteral("empty response"));
            } else {
                emit translated(job.key, text);
            }
            fillSlots();
        });
    }

    static bool isFatal(QNetworkReply::NetworkError error)
    {
        return error == QNetworkReply::ConnectionRefusedError
            || error == QNetworkReply::HostNotFoundError
            || error == QNetworkReply::ProtocolUnknownError
            || error == QNetworkReply::ProtocolInvalidOperationError;
    }

    QString systemPrompt() const
    {
        return QStringLiteral(
            "You are a professional translator. Translate the Markdown fragment "
            "given by the user into %1. Preserve all Markdown syntax, inline code, "
            "code blocks, link URLs, image paths, and HTML tags exactly as they are. "
            "Do not translate the contents of code spans or code blocks. "
            "Output only the translated Markdown, with no explanations or preamble.")
            .arg(targetLanguage_);
    }

    QNetworkAccessManager *manager_ = nullptr;
    QSet<QNetworkReply *> active_;
    QHash<QNetworkReply *, QString> activeKeys_;
    bool cancelling_ = false;
    int maxInFlight_ = 2;
    QList<Job> queue_;
    QString endpoint_;
    QString model_;
    QString targetLanguage_;
};

class MainWindow;

// Lets a tab be dragged out of the tab bar to detach it into a new window,
// mirroring browser tab tear-off. QTabBar's own movable-tabs behavior (drag
// to reorder within the bar) is left alone; only a drag that ends clearly
// outside the bar's bounds is treated as a detach request. The actual
// detach is decided/performed by whoever wires up onTabDetachRequested
// (MainWindow), not by this class.
class DetachableTabBar : public QTabBar {
    Q_OBJECT

public:
    using QTabBar::QTabBar;

    std::function<QWidget *(int)> widgetAtIndex;
    std::function<void(QWidget *, const QPoint &)> onTabDetachRequested;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            // Whether this is actually allowed to detach (e.g. a lone tab
            // dragged onto empty space is a no-op) is decided by whoever
            // handles onTabDetachRequested once the drop target is known.
            draggedIndex_ = tabAt(event->pos());
            draggedWidget_ = (draggedIndex_ >= 0 && widgetAtIndex) ? widgetAtIndex(draggedIndex_) : nullptr;
            draggedOutside_ = false;
        }
        QTabBar::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        QTabBar::mouseMoveEvent(event);
        if ((event->buttons() & Qt::LeftButton) && draggedWidget_ != nullptr) {
            // Generous margin so ordinary in-bar reordering, including the
            // slight vertical wobble of a real drag, never triggers a detach.
            const QRect bounds = rect().adjusted(0, -16, 0, 48);
            const bool outside = !bounds.contains(event->pos());
            const QPoint globalPos = event->globalPosition().toPoint();

            if (outside && !draggedOutside_) {
                // Transition frame only: showDragPreview positions and
                // animates the preview itself. Calling moveDragPreview here
                // too would fight that animation with a direct jump.
                showDragPreview(globalPos);
            } else if (!outside && draggedOutside_) {
                hideDragPreview();
            } else if (outside) {
                moveDragPreview(globalPos);
            }
            draggedOutside_ = outside;
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QWidget *widget = draggedWidget_;
        const bool shouldDetach = draggedOutside_ && widget != nullptr;
        const QPoint globalPos = event->globalPosition().toPoint();
        draggedWidget_ = nullptr;
        draggedOutside_ = false;
        hideDragPreview();

        // Let QTabBar finish its own drag/reorder handling first so removing
        // the tab afterward (if we detach) doesn't fight its internal state.
        QTabBar::mouseReleaseEvent(event);

        if (shouldDetach && onTabDetachRequested) {
            onTabDetachRequested(widget, globalPos);
        }
    }

private:
    // A translucent "ghost" of the tab that follows the cursor once the
    // drag has left the bar, popping in with a grow animation to signal
    // that releasing here will tear the tab off (into a new window, or
    // into whatever mdv window it's dropped on).
    //
    // The window's own opacity is set to its final value immediately -
    // never left at 0 - so the preview is guaranteed to be visible even if
    // the animation below can't run for some reason; the animation is a
    // bonus flourish on top of that, not a requirement for visibility.
    void showDragPreview(const QPoint &globalPos)
    {
        if (draggedIndex_ < 0) {
            return;
        }

        const QPixmap pixmap = grab(tabRect(draggedIndex_));
        if (pixmap.isNull()) {
            return;
        }
        dragPreviewSize_ = pixmap.size();

        dragPreview_ = new QWidget(nullptr, Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
        dragPreview_->setAttribute(Qt::WA_ShowWithoutActivating);
        dragPreview_->setAttribute(Qt::WA_TransparentForMouseEvents);
        dragPreview_->setWindowOpacity(0.9);

        auto *layout = new QVBoxLayout(dragPreview_);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *label = new QLabel(dragPreview_);
        label->setPixmap(pixmap);
        label->setScaledContents(true);
        layout->addWidget(label);

        const QRect endRect = previewRectFor(globalPos, dragPreviewSize_);
        const QSize startSize = dragPreviewSize_ * 0.55;
        const QRect startRect = previewRectFor(globalPos, startSize);

        dragPreview_->setGeometry(startRect);
        dragPreview_->show();
        dragPreview_->raise();

        auto *grow = new QPropertyAnimation(dragPreview_, "geometry", dragPreview_);
        grow->setDuration(160);
        grow->setEasingCurve(QEasingCurve::OutBack);
        grow->setStartValue(startRect);
        grow->setEndValue(endRect);
        grow->start(QAbstractAnimation::DeleteWhenStopped);
    }

    static QRect previewRectFor(const QPoint &globalPos, const QSize &size)
    {
        return QRect(globalPos - QPoint(size.width() / 2, size.height() / 2), size);
    }

    void moveDragPreview(const QPoint &globalPos)
    {
        if (dragPreview_ == nullptr) {
            return;
        }
        dragPreview_->setGeometry(previewRectFor(globalPos, dragPreviewSize_));
    }

    void hideDragPreview()
    {
        if (dragPreview_ != nullptr) {
            // Hide synchronously (not just deleteLater) so a hit test done
            // right after release, e.g. "what window is under the cursor",
            // doesn't see the ghost preview itself.
            dragPreview_->hide();
            dragPreview_->deleteLater();
            dragPreview_ = nullptr;
        }
    }

    QPointer<QWidget> draggedWidget_;
    int draggedIndex_ = -1;
    bool draggedOutside_ = false;
    QPointer<QWidget> dragPreview_;
    QSize dragPreviewSize_;
};

// QTabWidget::setTabBar() is protected, so a subclass is needed to install
// the DetachableTabBar above.
class MdvTabWidget : public QTabWidget {
    Q_OBJECT

public:
    explicit MdvTabWidget(QWidget *parent = nullptr)
        : QTabWidget(parent)
    {
        setTabBar(new DetachableTabBar(this));
    }

    DetachableTabBar *detachableTabBar() const
    {
        return qobject_cast<DetachableTabBar *>(tabBar());
    }
};

// One open Markdown document: its own editor, outline, and live preview
// (each with its own QWebChannel/PreviewBridge pair), plus the translation
// state for that document's content. Owned by MainWindow's QTabWidget, one
// per tab. Shared, window-wide settings (theme, fonts, language, Ollama
// config, dialogs) are read from `window_` rather than duplicated here.
class DocumentTab : public QWidget {
    Q_OBJECT

public:
    explicit DocumentTab(MainWindow *window, QWidget *parent = nullptr);

    QPlainTextEdit *editor() const { return editor_; }
    QWebEngineView *preview() const { return preview_; }
    QTreeWidget *outline() const { return outline_; }

    QString filePath() const { return currentFile_; }
    bool isModified() const { return documentModified_; }
    QString previewMode() const { return previewMode_; }

    // True for a never-touched tab (the startup welcome tab, or a fresh
    // "New" tab): no backing file and no edits, so it's safe to replace
    // outright instead of leaving it around as an extra tab.
    bool isPristineUntitled() const { return currentFile_.isEmpty() && !documentModified_; }

    void setUntitledLabel(const QString &label) { untitledLabel_ = label; }

    QString displayName() const
    {
        return currentFile_.isEmpty() ? untitledLabel_ : QFileInfo(currentFile_).fileName();
    }

    QString tabLabel() const
    {
        return displayName() + (documentModified_ ? QStringLiteral(" *") : QString());
    }

    void setInitialContent(const QString &text)
    {
        editor_->setPlainText(text);
        currentFile_.clear();
        fileEncoding_ = QStringConverter::Utf8;
        encodingLossy_ = false;
        documentModified_ = false;
        pendingExternalChange_ = false;
        watchCurrentFile();
        updateOutline();
        updatePreview();
    }

    bool loadFile(const QString &path);
    bool saveFile();
    bool saveFileAs();
    bool writeFile(const QString &path);
    bool confirmDiscardChanges();
    void checkForExternalChanges();
    void reparentToWindow(MainWindow *newWindow);

    void updateOutline();
    void updatePreview();
    void setPreviewMode(const QString &mode);
    void updateTranslationModeUi();
    void applyFontsAndTheme();
    void applyPaneVisibility();
    void updateUiTexts();

    bool hasPendingTranslation(const QString &key) const { return pendingTranslations_.contains(key); }
    void applyTranslationResult(const QString &key, const QString &text)
    {
        pendingTranslations_.remove(key);
        translationCache_.insert(key, text);
        refreshTranslatedPreview();
    }
    // A failed block is cached as an empty string: it renders as its
    // original text (with a marker in bilingual view) and is not retried
    // until the app restarts or the block is edited.
    void applyTranslationFailure(const QString &key)
    {
        pendingTranslations_.remove(key);
        translationCache_.insert(key, QString());
        refreshTranslatedPreview();
    }
    // Returns true if this tab had translation state that needed resetting.
    bool cancelTranslationsOnFatalError()
    {
        const bool wasTranslating = !pendingTranslations_.isEmpty() || previewMode_ != "original";
        pendingTranslations_.clear();
        if (previewMode_ != "original") {
            previewMode_ = "original";
            updateTranslationModeUi();
            updatePreview();
        }
        return wasTranslating;
    }

    void pasteFromClipboard();
    void jumpToHeading(QTreeWidgetItem *item)
    {
        if (item == nullptr) {
            return;
        }

        const int position = item->data(0, Qt::UserRole).toInt();
        QTextCursor cursor(editor_->document());
        cursor.setPosition(position);
        editor_->setTextCursor(cursor);
        editor_->setFocus();
        editor_->centerCursor();
    }

    void syncPreviewToEditor();
    void syncEditorToPreview(int headingCount, int segment, double t, double fraction);

private:
    bool pasteImageFromClipboard(const QMimeData *mimeData);
    bool pasteImageFileFromClipboard(const QMimeData *mimeData);

    void reloadPreviewTemplate();
    void pushPreviewContent();
    QString buildPreviewTemplate() const;
    QUrl previewBaseUrl() const
    {
        return QUrl::fromLocalFile((currentFile_.isEmpty() ? QDir::currentPath() : QFileInfo(currentFile_).absolutePath()) + "/");
    }

    QString translationCacheKey(const QString &block) const;
    QList<MdBlock> translatableBlocksByPriority() const
    {
        QList<MdBlock> ordered;
        const QList<MdBlock> blocks = splitMarkdownBlocks(editor_->toPlainText());
        for (const MdBlock &block : blocks) {
            if (blockIsTranslatable(block.text)) {
                ordered.append(block);
            }
        }

        const int topPos = editor_->cursorForPosition(QPoint(0, 0)).position();
        std::stable_sort(ordered.begin(), ordered.end(), [topPos](const MdBlock &a, const MdBlock &b) {
            return translationPriority(a.position, topPos) < translationPriority(b.position, topPos);
        });
        return ordered;
    }
    void ensureTranslations();
    void reprioritizeTranslations();
    QString composePreviewHtml() const;
    void refreshTranslatedPreview();
    void cancelOwnTranslations();

    void scrollEditorToPosition(int target)
    {
        QScrollBar *bar = editor_->verticalScrollBar();
        syncingEditorFromPreview_ = true;

        int lo = bar->minimum();
        int hi = bar->maximum();
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            bar->setValue(mid);
            if (editor_->cursorForPosition(QPoint(0, 0)).position() < target) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        bar->setValue(lo);

        syncingEditorFromPreview_ = false;
    }

    QVector<int> editorHeadingPositions() const
    {
        QVector<int> positions;
        bool inFence = false;

        for (QTextBlock block = editor_->document()->firstBlock(); block.isValid(); block = block.next()) {
            const QString trimmed = block.text().trimmed();
            if (trimmed.startsWith(QLatin1String("```")) || trimmed.startsWith(QLatin1String("~~~"))) {
                inFence = !inFence;
                continue;
            }
            if (!inFence && parseHeading(block.text()).level > 0) {
                positions.append(block.position());
            }
        }

        return positions;
    }

    bool isActive() const;

    void watchCurrentFile()
    {
        const QStringList watched = fileWatcher_->files();
        for (const QString &watchedPath : watched) {
            if (watchedPath != currentFile_) {
                fileWatcher_->removePath(watchedPath);
            }
        }
        if (!currentFile_.isEmpty() && !fileWatcher_->files().contains(currentFile_)) {
            fileWatcher_->addPath(currentFile_);
        }
    }
    void onFileChangedOnDisk(const QString &path);

    MainWindow *window_ = nullptr;

    QTreeWidget *outline_ = nullptr;
    QPlainTextEdit *editor_ = nullptr;
    QWebEngineView *preview_ = nullptr;
    QSplitter *splitter_ = nullptr;
    QTimer *previewUpdateTimer_ = nullptr;
    QTimer *translationPriorityTimer_ = nullptr;
    QString pendingPreviewHtml_;
    QUrl loadedPreviewBaseUrl_;
    bool previewLoaded_ = false;

    QString currentFile_;
    QString untitledLabel_;
    QStringConverter::Encoding fileEncoding_ = QStringConverter::Utf8;
    bool encodingLossy_ = false;
    bool documentModified_ = false;
    bool syncingEditorFromPreview_ = false;

    QFileSystemWatcher *fileWatcher_ = nullptr;
    bool pendingExternalChange_ = false;
    bool suppressNextExternalChange_ = false;
    bool externalChangeCheckInProgress_ = false;

    QString previewMode_ = QStringLiteral("original");
    QHash<QString, QString> translationCache_;
    QSet<QString> pendingTranslations_;
    QToolButton *originalModeButton_ = nullptr;
    QToolButton *bilingualModeButton_ = nullptr;
    QToolButton *translatedModeButton_ = nullptr;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // Every open window, in creation order, so a tab dropped near the edge
    // of another mdv window can be merged into it instead of always
    // spawning a brand-new window.
    static QList<MainWindow *> &allWindows()
    {
        static QList<MainWindow *> windows;
        return windows;
    }

    // QApplication::widgetAt() only resolves actual Qt widgets, and the
    // preview pane's QWebEngineView is backed by its own native (Chromium)
    // surface on most platforms - a drop over a preview pane would be
    // missed entirely. Testing each window's own screen geometry directly
    // works regardless of what's rendered inside it.
    static MainWindow *windowAt(const QPoint &globalPos)
    {
        for (MainWindow *candidate : allWindows()) {
            if (candidate->isVisible() && candidate->frameGeometry().contains(globalPos)) {
                return candidate;
            }
        }
        return nullptr;
    }

    explicit MainWindow(bool startWithEditorHidden = false)
    {
        allWindows().append(this);

        loadViewSettings();
        if (startWithEditorHidden) {
            editorVisible_ = false;
        }

        translator_ = new OllamaTranslator(this);
        connect(translator_, &OllamaTranslator::translated, this, [this](const QString &key, const QString &text) {
            for (DocumentTab *tab : allTabs()) {
                if (tab->hasPendingTranslation(key)) {
                    tab->applyTranslationResult(key, text);
                }
            }
        });
        connect(translator_, &OllamaTranslator::blockFailed, this, [this](const QString &key, const QString &error) {
            Q_UNUSED(error);
            for (DocumentTab *tab : allTabs()) {
                if (tab->hasPendingTranslation(key)) {
                    tab->applyTranslationFailure(key);
                }
            }
        });
        connect(translator_, &OllamaTranslator::failed, this, [this](const QString &error) {
            bool anyWasTranslating = false;
            for (DocumentTab *tab : allTabs()) {
                if (tab->cancelTranslationsOnFatalError()) {
                    anyWasTranslating = true;
                }
            }
            if (anyWasTranslating) {
                QMessageBox::warning(this, uiText("translationFailedTitle"),
                    uiText("translationFailedText").arg(error));
            }
        });

        createActions();
        createMenus();

        tabWidget_ = new MdvTabWidget(this);
        tabWidget_->setTabsClosable(true);
        tabWidget_->setMovable(true);
        tabWidget_->setDocumentMode(true);
        connect(tabWidget_, &QTabWidget::currentChanged, this, [this](int index) { onCurrentTabChanged(index); });
        connect(tabWidget_, &QTabWidget::tabCloseRequested, this, [this](int index) { closeTab(index); });
        setCentralWidget(tabWidget_);

        tabWidget_->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tabWidget_->tabBar(), &QTabBar::customContextMenuRequested, this, [this](const QPoint &pos) {
            showTabContextMenu(pos);
        });

        tabWidget_->detachableTabBar()->widgetAtIndex = [this](int index) -> QWidget * {
            return tabWidget_->widget(index);
        };
        tabWidget_->detachableTabBar()->onTabDetachRequested = [this](QWidget *widget, const QPoint &globalPos) {
            if (auto *tab = qobject_cast<DocumentTab *>(widget)) {
                handleTabDropped(tab, globalPos);
            }
        };

        setAcceptDrops(true);

        auto *nextTabShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Tab), this);
        connect(nextTabShortcut, &QShortcut::activated, this, [this] {
            if (tabWidget_->count() > 1) {
                tabWidget_->setCurrentIndex((tabWidget_->currentIndex() + 1) % tabWidget_->count());
            }
        });
        auto *previousTabShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), this);
        connect(previousTabShortcut, &QShortcut::activated, this, [this] {
            if (tabWidget_->count() > 1) {
                tabWidget_->setCurrentIndex((tabWidget_->currentIndex() - 1 + tabWidget_->count()) % tabWidget_->count());
            }
        });

        updateUiTexts();
        loadRecentFiles();
        updateRecentFilesMenu();

        applyViewSettings();
        applyPaneVisibility(false);
        updateWindowTitle();

        versionLabel_ = new QLabel(QStringLiteral("mdv %1").arg(QString::fromUtf8(MDV_VERSION)), this);
        statusBar()->addPermanentWidget(versionLabel_);

        resize(1100, 720);
        showReadyStatus();
    }

    // Called once at startup, after any files requested on the command line
    // or handed over by the OS have been opened as tabs: only shows the
    // blank sample document when nothing else was opened.
    void ensureAtLeastOneTab()
    {
        if (tabWidget_->count() == 0) {
            DocumentTab *tab = newBlankTab();
            tab->setInitialContent(sampleMarkdown());
        }
    }

    ~MainWindow() override
    {
        allWindows().removeAll(this);
    }

    void openFileFromCommandLine(const QString &path)
    {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) {
            QMessageBox::warning(this, uiText("openFailed"), uiText("fileMissing").arg(path));
            return;
        }
        openPathAsTab(info.absoluteFilePath());
    }

    // Files handed over by the OS (open -a, Finder "Open With", Dock drops)
    // arrive while the app may already be editing something.
    void openFileFromOsRequest(const QString &path)
    {
        openFileFromCommandLine(path);
        raise();
        activateWindow();
    }

    QString uiText(const QString &key) const
    {
        const bool ja = currentLanguage_ == "ja";

        if (key == "file") return ja ? "ファイル(&F)" : "&File";
        if (key == "edit") return ja ? "編集(&E)" : "&Edit";
        if (key == "search") return ja ? "検索(&S)" : "&Search";
        if (key == "view") return ja ? "表示(&V)" : "&View";
        if (key == "theme") return ja ? "テーマ(&T)" : "&Theme";
        if (key == "language") return ja ? "言語(&L)" : "&Language";
        if (key == "new") return ja ? "新規作成(&N)" : "&New";
        if (key == "open") return ja ? "開く(&O)..." : "&Open...";
        if (key == "save") return ja ? "保存(&S)" : "&Save";
        if (key == "saveAs") return ja ? "名前を付けて保存(&A)..." : "Save &As...";
        if (key == "closeTab") return ja ? "タブを閉じる(&W)" : "&Close Tab";
        if (key == "openInNewWindow") return ja ? "新しいウインドウで開く" : "Open in New Window";
        if (key == "recent") return ja ? "最近開いたファイル(&R)" : "Open &Recent";
        if (key == "clearRecent") return ja ? "履歴を消去(&C)" : "&Clear Recent Files";
        if (key == "exit") return ja ? "終了(&X)" : "E&xit";
        if (key == "undo") return ja ? "元に戻す(&U)" : "&Undo";
        if (key == "redo") return ja ? "やり直し(&R)" : "&Redo";
        if (key == "cut") return ja ? "切り取り(&T)" : "Cu&t";
        if (key == "copy") return ja ? "コピー(&C)" : "&Copy";
        if (key == "paste") return ja ? "貼り付け(&P)" : "&Paste";
        if (key == "find") return ja ? "検索(&F)..." : "&Find...";
        if (key == "replace") return ja ? "置換(&R)..." : "&Replace...";
        if (key == "findNext") return ja ? "次を検索(&N)" : "Find &Next";
        if (key == "findPrevious") return ja ? "前を検索(&P)" : "Find &Previous";
        if (key == "light") return ja ? "ライト(&L)" : "&Light";
        if (key == "dark") return ja ? "ダーク(&D)" : "&Dark";
        if (key == "sepia") return ja ? "セピア(&S)" : "&Sepia";
        if (key == "zoomIn") return ja ? "拡大(&I)" : "Zoom &In";
        if (key == "zoomOut") return ja ? "縮小(&O)" : "Zoom &Out";
        if (key == "resetZoom") return ja ? "拡大率をリセット(&Z)" : "&Reset Zoom";
        if (key == "editorFont") return ja ? "エディタのフォント(&F)..." : "Editor &Font...";
        if (key == "previewFont") return ja ? "プレビューのフォント(&P)..." : "&Preview Font...";
        if (key == "resetFonts") return ja ? "フォントをリセット(&R)" : "&Reset Fonts";
        if (key == "fontStatus") return ja ? "フォント: エディタ %1 / プレビュー %2" : "Fonts: editor %1 / preview %2";
        if (key == "showEditor") return ja ? "編集ペインを表示(&E)" : "Show &Editor Pane";
        if (key == "editorShown") return ja ? "編集ペインを表示しました" : "Editor pane shown";
        if (key == "editorHidden") return ja ? "編集ペインを非表示にしました" : "Editor pane hidden";
        if (key == "ready") return ja ? "準備完了" : "Ready";
        if (key == "readyWithVersion") return ja ? "準備完了 - mdv %1" : "Ready - mdv %1";
        if (key == "newDocument") return ja ? "新規ドキュメント" : "New document";
        if (key == "openMarkdown") return ja ? "Markdown を開く" : "Open Markdown";
        if (key == "saveMarkdown") return ja ? "Markdown を保存" : "Save Markdown";
        if (key == "openFailed") return ja ? "開けませんでした" : "Open failed";
        if (key == "saveFailed") return ja ? "保存できませんでした" : "Save failed";
        if (key == "fileMissing") return ja ? "ファイルが見つかりません:\n%1" : "The file no longer exists:\n%1";
        if (key == "opened") return ja ? "開きました: %1" : "Opened %1";
        if (key == "saved") return ja ? "保存しました: %1" : "Saved %1";
        if (key == "unsavedTitle") return ja ? "未保存の変更" : "Unsaved changes";
        if (key == "unsavedText") return ja ? "ドキュメントに未保存の変更があります。" : "The document has unsaved changes.";
        if (key == "unsavedExternalText") return ja
            ? "このファイルは他のプログラムによっても変更されています。\n"
              "「保存」を選ぶとその変更を上書きします。「破棄」を選ぶと自分の変更を破棄し、外部の変更をそのまま残します。"
            : "This file was also changed by another program.\n"
              "Save will overwrite that external change; Discard will drop your edits and leave the external version as-is.";
        if (key == "externalChangeTitle") return ja ? "ファイルが変更されました" : "File changed on disk";
        if (key == "externalChangeText") return ja
            ? "%1 は他のプログラムによって変更されました。再読み込みしますか?"
            : "%1 has been changed by another program. Reload it?";
        if (key == "externalChangeUnsavedText") return ja
            ? "%1 は他のプログラムによって変更されました。このタブには未保存の変更があり、再読み込みすると失われます。再読み込みしますか?"
            : "%1 has been changed by another program. This tab has unsaved changes that will be lost if you reload. Reload anyway?";
        if (key == "findReplaceTitle") return ja ? "検索と置換" : "Find and Replace";
        if (key == "findLabel") return ja ? "検索" : "Find";
        if (key == "replaceLabel") return ja ? "置換" : "Replace";
        if (key == "matchCase") return ja ? "大文字/小文字を区別" : "Match case";
        if (key == "replaceAll") return ja ? "すべて置換" : "Replace All";
        if (key == "close") return ja ? "閉じる" : "Close";
        if (key == "enterFind") return ja ? "検索する文字列を入力してください" : "Enter text to find";
        if (key == "searchWrapped") return ja ? "先頭/末尾に戻って検索しました" : "Search wrapped";
        if (key == "notFound") return ja ? "見つかりません: %1" : "Not found: %1";
        if (key == "replacedCount") return ja ? "%1 件置換しました" : "Replaced %1 occurrence(s)";
        if (key == "largeFileTitle") return ja ? "大きなファイル" : "Large file";
        if (key == "largeFileText") return ja
            ? "ファイルサイズが大きいため、開くのに時間がかかる場合があります (%1 MB)。開きますか?"
            : "This file is large (%1 MB) and may take a while to open. Open anyway?";
        if (key == "encodingTitle") return ja ? "文字コードの警告" : "Encoding warning";
        if (key == "encodingText") return ja
            ? "このファイルは UTF-8 として正しく読み込めませんでした(Shift-JIS などの可能性があります)。\n文字化けした状態で開きます。このまま上書き保存すると元の内容が失われる可能性があります。\n開きますか?"
            : "This file could not be fully decoded as UTF-8 (it may use another encoding such as Shift-JIS).\nIt will open with garbled characters, and saving over the original may lose data.\nOpen anyway?";
        if (key == "lossySaveText") return ja
            ? "このドキュメントは文字化けした状態で読み込まれています。保存すると元の内容が失われる可能性があります。保存しますか?"
            : "This document was loaded with decoding errors. Saving may lose the original content. Save anyway?";
        if (key == "pasteImageFailed") return ja ? "画像の貼り付けに失敗しました" : "Paste image failed";
        if (key == "createAssetsFailed") return ja ? "assets ディレクトリを作成できませんでした。" : "Could not create assets directory.";
        if (key == "saveClipboardImageFailed") return ja ? "クリップボード画像を保存できませんでした。" : "Could not save clipboard image.";
        if (key == "copyImageFailed") return ja ? "画像ファイルをコピーできませんでした。" : "Could not copy image file.";
        if (key == "pastedImage") return ja ? "画像を貼り付けました: %1" : "Pasted image %1";
        if (key == "viewStatus") return ja ? "テーマ: %1, 文字サイズ: %2pt" : "Theme: %1, font size: %2pt";
        if (key == "translationMenu") return ja ? "翻訳(&T)" : "&Translation";
        if (key == "trOriginal") return ja ? "原文" : "Original";
        if (key == "trBilingual") return ja ? "対訳" : "Bilingual";
        if (key == "trTranslated") return ja ? "翻訳" : "Translation";
        if (key == "trSettings") return ja ? "翻訳の設定(&S)..." : "Translation &Settings...";
        if (key == "trSettingsTitle") return ja ? "翻訳の設定" : "Translation Settings";
        if (key == "trEndpoint") return ja ? "Ollama エンドポイント:" : "Ollama endpoint:";
        if (key == "trModel") return ja ? "モデル:" : "Model:";
        if (key == "trTarget") return ja ? "翻訳先の言語:" : "Target language:";
        if (key == "trParallel") return ja ? "同時リクエスト数:" : "Parallel requests:";
        if (key == "trPending") return ja ? "(翻訳中...)" : "(translating...)";
        if (key == "trFailedBlock") return ja ? "(翻訳失敗)" : "(translation failed)";
        if (key == "translating") return ja ? "翻訳中... 残り %1" : "Translating... %1 remaining";
        if (key == "translationDone") return ja ? "翻訳が完了しました" : "Translation finished";
        if (key == "translationFailedTitle") return ja ? "翻訳エラー" : "Translation error";
        if (key == "translationFailedText") return ja
            ? "翻訳に失敗しました:\n%1\n\nOllama が起動していること、モデル名が正しいことを確認してください。"
            : "Translation failed:\n%1\n\nMake sure Ollama is running and the model name is correct.";
        if (key == "untitled") return ja ? "無題" : "Untitled";
        if (key == "windowTitle") return ja ? "%1%2 - mdv" : "%1%2 - mdv";
        if (key == "placeholder") {
            return ja
                ? "# Markdown\n\n左側で編集すると、右側にプレビューが表示されます。"
                : "# Markdown\n\nWrite on the left. Preview appears on the right.";
        }

        return key;
    }

    QString markdownFilter() const
    {
        return currentLanguage_ == "ja"
            ? "Markdown ファイル (*.md *.markdown *.mdown);;テキストファイル (*.txt);;すべてのファイル (*)"
            : "Markdown files (*.md *.markdown *.mdown);;Text files (*.txt);;All files (*)";
    }

    QString currentTheme() const { return currentTheme_; }
    int fontSize() const { return fontSize_; }
    bool editorVisible() const { return editorVisible_; }
    QString ollamaEndpoint() const { return ollamaEndpoint_; }
    QString ollamaModel() const { return ollamaModel_; }
    QString translationTarget() const { return translationTarget_; }
    int ollamaParallel() const { return ollamaParallel_; }
    OllamaTranslator *translator() const { return translator_; }
    QString lastDialogDir() const { return lastDialogDir_; }
    void setLastDialogDir(const QString &dir) { lastDialogDir_ = dir; }
    QString saveDialogDir() const { return saveDialogDir_; }
    void setSaveDialogDir(const QString &dir) { saveDialogDir_ = dir; }
    qint64 largeFileWarningBytes() const { return largeFileWarningBytes_; }
    QString editorFontFamily() const { return editorFontFamily_; }
    QString previewFontFamily() const { return previewFontFamily_; }
    int minFontSize() const { return minFontSize_; }

    QString editorFontFamilyCss() const
    {
        return QStringList({
            cssQuotedFontFamily(editorFontFamily_),
            "'SF Mono'",
            "'Menlo'",
            "'Hiragino Sans'",
            "'Hiragino Kaku Gothic ProN'",
            "'Yu Gothic'",
            "'Noto Sans CJK JP'",
            "monospace"
        }).join(", ");
    }

    QString previewFontFamilyCss() const
    {
        return QStringList({
            cssQuotedFontFamily(previewFontFamily_),
            "-apple-system",
            "'BlinkMacSystemFont'",
            "'Hiragino Sans'",
            "'Hiragino Kaku Gothic ProN'",
            "'Yu Gothic'",
            "'Noto Sans CJK JP'",
            "sans-serif"
        }).join(", ");
    }

    DocumentTab *activeTab() const { return qobject_cast<DocumentTab *>(tabWidget_->currentWidget()); }

    QList<DocumentTab *> allTabs() const
    {
        QList<DocumentTab *> list;
        list.reserve(tabWidget_->count());
        for (int i = 0; i < tabWidget_->count(); ++i) {
            if (auto *tab = qobject_cast<DocumentTab *>(tabWidget_->widget(i))) {
                list.append(tab);
            }
        }
        return list;
    }

    bool isKeyPendingInOtherTab(const QString &key, const DocumentTab *exclude) const
    {
        for (DocumentTab *tab : allTabs()) {
            if (tab != exclude && tab->hasPendingTranslation(key)) {
                return true;
            }
        }
        return false;
    }

    void activateTabForDialog(DocumentTab *tab) { tabWidget_->setCurrentWidget(tab); }

    void onTabStateChanged(DocumentTab *tab)
    {
        refreshTabLabel(tab);
        if (tab == activeTab()) {
            updateWindowTitle();
        }
    }

    void onEditorCopyAvailable(DocumentTab *tab, bool available)
    {
        if (tab == activeTab()) {
            cutAction_->setEnabled(available);
            copyAction_->setEnabled(available);
        }
    }

    void onEditorUndoAvailable(DocumentTab *tab, bool available)
    {
        if (tab == activeTab()) {
            undoAction_->setEnabled(available);
        }
    }

    void onEditorRedoAvailable(DocumentTab *tab, bool available)
    {
        if (tab == activeTab()) {
            redoAction_->setEnabled(available);
        }
    }

    void refreshSearchHighlightIfNeeded(DocumentTab *tab)
    {
        if (tab == activeTab() && findDialog_ != nullptr && findDialog_->isVisible() && !findText().isEmpty()) {
            updatePreviewSearchHighlight();
        }
    }

    void addRecentFile(const QString &path)
    {
        const QString absolutePath = QFileInfo(path).absoluteFilePath();
        recentFiles_.removeAll(absolutePath);
        recentFiles_.prepend(absolutePath);

        while (recentFiles_.size() > maxRecentFiles_) {
            recentFiles_.removeLast();
        }

        saveRecentFiles();
        updateRecentFilesMenu();
    }

    void showTranslationSettings()
    {
        QDialog dialog(this);
        dialog.setWindowTitle(uiText("trSettingsTitle"));

        auto *layout = new QGridLayout(&dialog);
        auto *endpointEdit = new QLineEdit(ollamaEndpoint_, &dialog);
        auto *modelCombo = new QComboBox(&dialog);
        modelCombo->setEditable(true);
        modelCombo->setMinimumWidth(220);
        modelCombo->setCurrentText(ollamaModel_);
        auto *targetCombo = new QComboBox(&dialog);
        targetCombo->addItem(QStringLiteral("日本語"), "Japanese");
        targetCombo->addItem(QStringLiteral("English"), "English");
        targetCombo->setCurrentIndex(translationTarget_ == "English" ? 1 : 0);
        auto *parallelSpin = new QSpinBox(&dialog);
        parallelSpin->setRange(1, 8);
        parallelSpin->setValue(ollamaParallel_);

        layout->addWidget(new QLabel(uiText("trEndpoint"), &dialog), 0, 0);
        layout->addWidget(endpointEdit, 0, 1);
        layout->addWidget(new QLabel(uiText("trModel"), &dialog), 1, 0);
        layout->addWidget(modelCombo, 1, 1);
        layout->addWidget(new QLabel(uiText("trTarget"), &dialog), 2, 0);
        layout->addWidget(targetCombo, 2, 1);
        layout->addWidget(new QLabel(uiText("trParallel"), &dialog), 3, 0);
        layout->addWidget(parallelSpin, 3, 1);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons, 4, 0, 1, 2);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        // Offer the models installed on the server; the combo stays editable
        // so a name can be typed when the server is unreachable.
        auto *manager = new QNetworkAccessManager(&dialog);
        const auto fetchModels = [manager, modelCombo, endpointEdit] {
            const QString base = cleanedEndpoint(endpointEdit->text());
            QNetworkReply *reply = manager->get(QNetworkRequest(QUrl(base + "/api/tags")));
            QObject::connect(reply, &QNetworkReply::finished, modelCombo, [reply, modelCombo] {
                reply->deleteLater();
                if (reply->error() != QNetworkReply::NoError) {
                    return;
                }
                const QJsonArray models = QJsonDocument::fromJson(reply->readAll())
                    .object().value("models").toArray();
                const QString current = modelCombo->currentText();
                modelCombo->clear();
                for (const QJsonValue &model : models) {
                    modelCombo->addItem(model.toObject().value("name").toString());
                }
                if (!current.isEmpty()) {
                    modelCombo->setCurrentText(current);
                }
            });
        };
        fetchModels();
        connect(endpointEdit, &QLineEdit::editingFinished, &dialog, fetchModels);

        if (dialog.exec() != QDialog::Accepted) {
            return;
        }

        ollamaEndpoint_ = cleanedEndpoint(endpointEdit->text());
        ollamaModel_ = modelCombo->currentText().trimmed();
        translationTarget_ = targetCombo->currentData().toString();
        ollamaParallel_ = parallelSpin->value();
        saveViewSettings();

        for (DocumentTab *tab : allTabs()) {
            if (tab->previewMode() != "original") {
                tab->updatePreview();
            }
        }
    }

    QString sampleMarkdown() const
    {
        if (currentLanguage_ == "ja") {
            return QStringLiteral(
                "# mdv\n\n"
                "C++ と Qt で作られたシンプルな Markdown ビュワー/エディタです。\n\n"
                "## 機能\n\n"
                "- ライブプレビュー\n"
                "- 見出しアウトライン\n"
                "- Markdown ファイルの読み込みと保存\n"
                "- 軽量な Qt Widgets UI\n\n"
                "## メモ\n\n"
                "### 編集\n\n"
                "左側で入力を始めてください。\n\n"
                "### ナビゲーション\n\n"
                "アウトラインの見出しをクリックすると、その位置へ移動します。");
        }

        return QStringLiteral(
            "# mdv\n\n"
            "Simple Markdown viewer/editor built with C++ and Qt.\n\n"
            "## Features\n\n"
            "- Live preview\n"
            "- Heading outline\n"
            "- Open and save Markdown files\n"
            "- Lightweight Qt Widgets UI\n\n"
            "## Notes\n\n"
            "### Editing\n\n"
            "Start typing on the left.\n\n"
            "### Navigation\n\n"
            "Click a heading in the outline to jump to it.");
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        for (int i = 0; i < tabWidget_->count(); ++i) {
            auto *tab = qobject_cast<DocumentTab *>(tabWidget_->widget(i));
            if (tab == nullptr) {
                continue;
            }
            tabWidget_->setCurrentIndex(i);
            if (!tab->confirmDiscardChanges()) {
                event->ignore();
                return;
            }
        }
        event->accept();
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (!event->mimeData()->hasUrls()) {
            return;
        }
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                event->acceptProposedAction();
                return;
            }
        }
    }

    void dropEvent(QDropEvent *event) override
    {
        bool openedAny = false;
        for (const QUrl &url : event->mimeData()->urls()) {
            if (!url.isLocalFile()) {
                continue;
            }
            const QFileInfo info(url.toLocalFile());
            if (!info.exists() || !info.isFile()) {
                continue;
            }
            openPathAsTab(info.absoluteFilePath());
            openedAny = true;
        }
        if (openedAny) {
            event->acceptProposedAction();
            raise();
            activateWindow();
        }
    }

private:
    void createActions()
    {
        newAction_ = new QAction("&New", this);
        newAction_->setShortcut(QKeySequence::New);
        connect(newAction_, &QAction::triggered, this, [this] { newFile(); });

        openAction_ = new QAction("&Open...", this);
        openAction_->setShortcut(QKeySequence::Open);
        connect(openAction_, &QAction::triggered, this, [this] { openFile(); });

        saveAction_ = new QAction("&Save", this);
        saveAction_->setShortcut(QKeySequence::Save);
        connect(saveAction_, &QAction::triggered, this, [this] {
            if (DocumentTab *tab = activeTab()) tab->saveFile();
        });

        saveAsAction_ = new QAction("Save &As...", this);
        saveAsAction_->setShortcut(QKeySequence::SaveAs);
        connect(saveAsAction_, &QAction::triggered, this, [this] {
            if (DocumentTab *tab = activeTab()) tab->saveFileAs();
        });

        closeTabAction_ = new QAction(this);
        closeTabAction_->setShortcut(QKeySequence::Close);
        connect(closeTabAction_, &QAction::triggered, this, [this] {
            closeTab(tabWidget_->currentIndex());
        });

        clearRecentFilesAction_ = new QAction("&Clear Recent Files", this);
        connect(clearRecentFilesAction_, &QAction::triggered, this, [this] {
            recentFiles_.clear();
            saveRecentFiles();
            updateRecentFilesMenu();
        });

        exitAction_ = new QAction("E&xit", this);
        exitAction_->setShortcut(QKeySequence::Quit);
        connect(exitAction_, &QAction::triggered, this, [this] { close(); });

        undoAction_ = new QAction("&Undo", this);
        undoAction_->setShortcut(QKeySequence::Undo);
        connect(undoAction_, &QAction::triggered, this, [this] {
            if (DocumentTab *tab = activeTab()) tab->editor()->undo();
        });

        redoAction_ = new QAction("&Redo", this);
        redoAction_->setShortcut(QKeySequence::Redo);
        connect(redoAction_, &QAction::triggered, this, [this] {
            if (DocumentTab *tab = activeTab()) tab->editor()->redo();
        });

        cutAction_ = new QAction("Cu&t", this);
        cutAction_->setShortcut(QKeySequence::Cut);
        connect(cutAction_, &QAction::triggered, this, [this] {
            if (DocumentTab *tab = activeTab()) tab->editor()->cut();
        });

        copyAction_ = new QAction("&Copy", this);
        copyAction_->setShortcut(QKeySequence::Copy);
        connect(copyAction_, &QAction::triggered, this, [this] {
            if (DocumentTab *tab = activeTab()) tab->editor()->copy();
        });

        pasteAction_ = new QAction("&Paste", this);
        pasteAction_->setShortcut(QKeySequence::Paste);
        connect(pasteAction_, &QAction::triggered, this, [this] {
            if (DocumentTab *tab = activeTab()) tab->pasteFromClipboard();
        });

        findAction_ = new QAction("&Find...", this);
        findAction_->setShortcut(QKeySequence::Find);
        connect(findAction_, &QAction::triggered, this, [this] { showFindReplaceDialog(false); });

        replaceAction_ = new QAction("&Replace...", this);
        replaceAction_->setShortcut(QKeySequence::Replace);
        connect(replaceAction_, &QAction::triggered, this, [this] { showFindReplaceDialog(true); });

        findNextAction_ = new QAction("Find &Next", this);
        findNextAction_->setShortcut(QKeySequence::FindNext);
        connect(findNextAction_, &QAction::triggered, this, [this] { findNext(); });

        findPreviousAction_ = new QAction("Find &Previous", this);
        findPreviousAction_->setShortcut(QKeySequence::FindPrevious);
        connect(findPreviousAction_, &QAction::triggered, this, [this] { findPrevious(); });

        themeActionGroup_ = new QActionGroup(this);
        themeActionGroup_->setExclusive(true);

        lightThemeAction_ = new QAction("&Light", this);
        lightThemeAction_->setCheckable(true);
        lightThemeAction_->setData("light");
        themeActionGroup_->addAction(lightThemeAction_);

        darkThemeAction_ = new QAction("&Dark", this);
        darkThemeAction_->setCheckable(true);
        darkThemeAction_->setData("dark");
        themeActionGroup_->addAction(darkThemeAction_);

        sepiaThemeAction_ = new QAction("&Sepia", this);
        sepiaThemeAction_->setCheckable(true);
        sepiaThemeAction_->setData("sepia");
        themeActionGroup_->addAction(sepiaThemeAction_);

        connect(themeActionGroup_, &QActionGroup::triggered, this, [this](QAction *action) {
            currentTheme_ = action->data().toString();
            applyViewSettings();
            saveViewSettings();
        });

        increaseFontSizeAction_ = new QAction("Zoom &In", this);
        increaseFontSizeAction_->setShortcut(QKeySequence::ZoomIn);
        connect(increaseFontSizeAction_, &QAction::triggered, this, [this] { changeFontSize(1); });

        decreaseFontSizeAction_ = new QAction("Zoom &Out", this);
        decreaseFontSizeAction_->setShortcut(QKeySequence::ZoomOut);
        connect(decreaseFontSizeAction_, &QAction::triggered, this, [this] { changeFontSize(-1); });

        resetFontSizeAction_ = new QAction("&Reset Zoom", this);
        resetFontSizeAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
        connect(resetFontSizeAction_, &QAction::triggered, this, [this] {
            fontSize_ = defaultFontSize_;
            applyViewSettings();
            saveViewSettings();
        });

        chooseEditorFontAction_ = new QAction(this);
        connect(chooseEditorFontAction_, &QAction::triggered, this, [this] { chooseEditorFont(); });

        choosePreviewFontAction_ = new QAction(this);
        connect(choosePreviewFontAction_, &QAction::triggered, this, [this] { choosePreviewFont(); });

        resetFontsAction_ = new QAction(this);
        connect(resetFontsAction_, &QAction::triggered, this, [this] {
            editorFontFamily_ = defaultEditorFontFamily();
            previewFontFamily_ = defaultPreviewFontFamily();
            applyViewSettings();
            saveViewSettings();
        });

        toggleEditorAction_ = new QAction(this);
        toggleEditorAction_->setCheckable(true);
        connect(toggleEditorAction_, &QAction::triggered, this, [this](bool checked) {
            editorVisible_ = checked;
            applyPaneVisibility(true);
            saveViewSettings();
        });

        translationModeGroup_ = new QActionGroup(this);
        translationModeGroup_->setExclusive(true);

        originalModeAction_ = new QAction(this);
        originalModeAction_->setCheckable(true);
        originalModeAction_->setChecked(true);
        originalModeAction_->setData("original");
        translationModeGroup_->addAction(originalModeAction_);

        bilingualModeAction_ = new QAction(this);
        bilingualModeAction_->setCheckable(true);
        bilingualModeAction_->setData("bilingual");
        translationModeGroup_->addAction(bilingualModeAction_);

        translatedModeAction_ = new QAction(this);
        translatedModeAction_->setCheckable(true);
        translatedModeAction_->setData("translated");
        translationModeGroup_->addAction(translatedModeAction_);

        connect(translationModeGroup_, &QActionGroup::triggered, this, [this](QAction *action) {
            if (DocumentTab *tab = activeTab()) tab->setPreviewMode(action->data().toString());
        });

        translationSettingsAction_ = new QAction(this);
        connect(translationSettingsAction_, &QAction::triggered, this, [this] { showTranslationSettings(); });

        languageActionGroup_ = new QActionGroup(this);
        languageActionGroup_->setExclusive(true);

        englishLanguageAction_ = new QAction("English", this);
        englishLanguageAction_->setCheckable(true);
        englishLanguageAction_->setData("en");
        languageActionGroup_->addAction(englishLanguageAction_);

        japaneseLanguageAction_ = new QAction("日本語", this);
        japaneseLanguageAction_->setCheckable(true);
        japaneseLanguageAction_->setData("ja");
        languageActionGroup_->addAction(japaneseLanguageAction_);

        connect(languageActionGroup_, &QActionGroup::triggered, this, [this](QAction *action) {
            currentLanguage_ = action->data().toString();
            updateUiTexts();
            applyViewSettings();
            saveViewSettings();
        });
    }

    void createMenus()
    {
        fileMenu_ = menuBar()->addMenu(QString());
        fileMenu_->addAction(newAction_);
        fileMenu_->addAction(openAction_);
        fileMenu_->addSeparator();
        fileMenu_->addAction(saveAction_);
        fileMenu_->addAction(saveAsAction_);
        fileMenu_->addSeparator();
        fileMenu_->addAction(closeTabAction_);
        fileMenu_->addSeparator();
        recentFilesMenu_ = fileMenu_->addMenu(QString());
        fileMenu_->addSeparator();
        fileMenu_->addAction(exitAction_);

        editMenu_ = menuBar()->addMenu(QString());
        editMenu_->addAction(undoAction_);
        editMenu_->addAction(redoAction_);
        editMenu_->addSeparator();
        editMenu_->addAction(cutAction_);
        editMenu_->addAction(copyAction_);
        editMenu_->addAction(pasteAction_);

        searchMenu_ = menuBar()->addMenu(QString());
        searchMenu_->addAction(findAction_);
        searchMenu_->addAction(findNextAction_);
        searchMenu_->addAction(findPreviousAction_);
        searchMenu_->addSeparator();
        searchMenu_->addAction(replaceAction_);

        translationMenu_ = menuBar()->addMenu(QString());
        translationMenu_->addAction(originalModeAction_);
        translationMenu_->addAction(bilingualModeAction_);
        translationMenu_->addAction(translatedModeAction_);
        translationMenu_->addSeparator();
        translationMenu_->addAction(translationSettingsAction_);

        viewMenu_ = menuBar()->addMenu(QString());
        themeMenu_ = viewMenu_->addMenu(QString());
        themeMenu_->addAction(lightThemeAction_);
        themeMenu_->addAction(darkThemeAction_);
        themeMenu_->addAction(sepiaThemeAction_);
        viewMenu_->addSeparator();
        viewMenu_->addAction(increaseFontSizeAction_);
        viewMenu_->addAction(decreaseFontSizeAction_);
        viewMenu_->addAction(resetFontSizeAction_);
        viewMenu_->addSeparator();
        viewMenu_->addAction(chooseEditorFontAction_);
        viewMenu_->addAction(choosePreviewFontAction_);
        viewMenu_->addAction(resetFontsAction_);
        viewMenu_->addSeparator();
        viewMenu_->addAction(toggleEditorAction_);
        viewMenu_->addSeparator();
        languageMenu_ = viewMenu_->addMenu(QString());
        languageMenu_->addAction(englishLanguageAction_);
        languageMenu_->addAction(japaneseLanguageAction_);
    }

    void updateUiTexts()
    {
        fileMenu_->setTitle(uiText("file"));
        editMenu_->setTitle(uiText("edit"));
        searchMenu_->setTitle(uiText("search"));
        translationMenu_->setTitle(uiText("translationMenu"));
        viewMenu_->setTitle(uiText("view"));
        themeMenu_->setTitle(uiText("theme"));
        languageMenu_->setTitle(uiText("language"));
        recentFilesMenu_->setTitle(uiText("recent"));

        newAction_->setText(uiText("new"));
        openAction_->setText(uiText("open"));
        saveAction_->setText(uiText("save"));
        saveAsAction_->setText(uiText("saveAs"));
        closeTabAction_->setText(uiText("closeTab"));
        clearRecentFilesAction_->setText(uiText("clearRecent"));
        exitAction_->setText(uiText("exit"));
        undoAction_->setText(uiText("undo"));
        redoAction_->setText(uiText("redo"));
        cutAction_->setText(uiText("cut"));
        copyAction_->setText(uiText("copy"));
        pasteAction_->setText(uiText("paste"));
        findAction_->setText(uiText("find"));
        replaceAction_->setText(uiText("replace"));
        findNextAction_->setText(uiText("findNext"));
        findPreviousAction_->setText(uiText("findPrevious"));
        lightThemeAction_->setText(uiText("light"));
        darkThemeAction_->setText(uiText("dark"));
        sepiaThemeAction_->setText(uiText("sepia"));
        increaseFontSizeAction_->setText(uiText("zoomIn"));
        decreaseFontSizeAction_->setText(uiText("zoomOut"));
        resetFontSizeAction_->setText(uiText("resetZoom"));
        chooseEditorFontAction_->setText(uiText("editorFont"));
        choosePreviewFontAction_->setText(uiText("previewFont"));
        resetFontsAction_->setText(uiText("resetFonts"));
        toggleEditorAction_->setText(uiText("showEditor"));
        originalModeAction_->setText(uiText("trOriginal"));
        bilingualModeAction_->setText(uiText("trBilingual"));
        translatedModeAction_->setText(uiText("trTranslated"));
        translationSettingsAction_->setText(uiText("trSettings"));

        englishLanguageAction_->setText("English");
        japaneseLanguageAction_->setText("日本語");
        englishLanguageAction_->setChecked(currentLanguage_ == "en");
        japaneseLanguageAction_->setChecked(currentLanguage_ == "ja");

        for (DocumentTab *tab : allTabs()) {
            tab->updateUiTexts();
        }

        updateFindDialogTexts();
        updateRecentFilesMenu();
        updateWindowTitle();
    }

    void updateFindDialogTexts()
    {
        if (findDialog_ == nullptr) {
            return;
        }

        findDialog_->setWindowTitle(uiText("findReplaceTitle"));
        findLabel_->setText(uiText("findLabel"));
        replaceLabel_->setText(uiText("replaceLabel"));
        matchCaseCheck_->setText(uiText("matchCase"));
        findNextButton_->setText(uiText("findNext"));
        findPreviousButton_->setText(uiText("findPrevious"));
        replaceButton_->setText(uiText("replaceLabel"));
        replaceAllButton_->setText(uiText("replaceAll"));
        closeFindButton_->setText(uiText("close"));
    }

    void showReadyStatus()
    {
        statusBar()->showMessage(uiText("readyWithVersion").arg(QString::fromUtf8(MDV_VERSION)));
    }

    // --- Tab management --------------------------------------------------

    DocumentTab *newBlankTab()
    {
        auto *tab = new DocumentTab(this);
        tabWidget_->addTab(tab, QString());
        tab->setUntitledLabel(nextUntitledLabel());
        tabWidget_->setCurrentWidget(tab);
        refreshTabLabel(tab);
        return tab;
    }

    QString nextUntitledLabel()
    {
        const QString label = untitledCounter_ == 1
            ? uiText("untitled")
            : QStringLiteral("%1 %2").arg(uiText("untitled")).arg(untitledCounter_);
        ++untitledCounter_;
        return label;
    }

    void refreshTabLabel(DocumentTab *tab)
    {
        const int index = tabWidget_->indexOf(tab);
        if (index < 0) {
            return;
        }
        tabWidget_->setTabText(index, tab->tabLabel());
        tabWidget_->setTabToolTip(index, tab->filePath().isEmpty() ? tab->displayName() : tab->filePath());
    }

    DocumentTab *findTabForPath(const QString &path) const
    {
        const QString absolutePath = QFileInfo(path).absoluteFilePath();
        for (DocumentTab *tab : allTabs()) {
            if (!tab->filePath().isEmpty() && QFileInfo(tab->filePath()).absoluteFilePath() == absolutePath) {
                return tab;
            }
        }
        return nullptr;
    }

    void openPathAsTab(const QString &path)
    {
        if (DocumentTab *existing = findTabForPath(path)) {
            tabWidget_->setCurrentWidget(existing);
            return;
        }

        // Replace a still-blank starter/new tab instead of leaving it
        // sitting next to the file being opened.
        if (tabWidget_->count() == 1) {
            auto *only = qobject_cast<DocumentTab *>(tabWidget_->widget(0));
            if (only != nullptr && only->isPristineUntitled()) {
                if (only->loadFile(path)) {
                    refreshTabLabel(only);
                }
                return;
            }
        }

        auto *tab = new DocumentTab(this);
        tabWidget_->addTab(tab, QString());
        tabWidget_->setCurrentWidget(tab);
        if (!tab->loadFile(path)) {
            const int index = tabWidget_->indexOf(tab);
            if (index >= 0) {
                tabWidget_->removeTab(index);
            }
            tab->deleteLater();
            return;
        }
        refreshTabLabel(tab);
    }

    void closeTab(int index)
    {
        auto *tab = qobject_cast<DocumentTab *>(tabWidget_->widget(index));
        if (tab == nullptr) {
            return;
        }
        tabWidget_->setCurrentIndex(index);
        if (!tab->confirmDiscardChanges()) {
            return;
        }
        tabWidget_->removeTab(index);
        tab->deleteLater();

        if (tabWidget_->count() == 0) {
            DocumentTab *fresh = newBlankTab();
            fresh->setInitialContent(QString());
        }
    }

    void showTabContextMenu(const QPoint &pos)
    {
        const int index = tabWidget_->tabBar()->tabAt(pos);
        if (index < 0) {
            return;
        }
        auto *tab = qobject_cast<DocumentTab *>(tabWidget_->widget(index));
        if (tab == nullptr) {
            return;
        }

        QMenu menu(this);
        QAction *closeAction = menu.addAction(uiText("closeTab"));
        QAction *openInNewWindowAction = nullptr;
        if (tabWidget_->count() > 1) {
            openInNewWindowAction = menu.addAction(uiText("openInNewWindow"));
        }

        QAction *chosen = menu.exec(tabWidget_->tabBar()->mapToGlobal(pos));
        if (chosen == nullptr) {
            return;
        }
        if (chosen == closeAction) {
            closeTab(index);
        } else if (chosen == openInNewWindowAction) {
            moveTabToNewWindow(tab, QPoint());
        }
    }

    // Adds a tab that already belongs to another window (its DocumentTab
    // must have been reparented to this window first via reparentToWindow).
    void adoptTab(DocumentTab *tab)
    {
        tabWidget_->addTab(tab, tab->tabLabel());
        refreshTabLabel(tab);
        tabWidget_->setCurrentWidget(tab);
    }

    // Decides what a tab dropped at globalPos should do: merge into
    // whichever other mdv window (if any) is under the cursor there, or
    // fall back to spawning a brand-new window - but never just to
    // relocate a lone tab, since dragging a window's only tab onto empty
    // space wouldn't change anything.
    void handleTabDropped(DocumentTab *tab, const QPoint &globalPos)
    {
        // Dropped back inside this same window (just not on the bar itself,
        // e.g. over the editor/preview) - treat it as a cancelled drag
        // rather than spawning a pointless new window.
        if (frameGeometry().contains(globalPos)) {
            return;
        }

        if (MainWindow *target = windowAt(globalPos)) {
            moveTabToExistingWindow(tab, target);
            return;
        }

        if (tabWidget_->count() > 1) {
            moveTabToNewWindow(tab, globalPos);
        }
    }

    // Moves a tab into another already-open window. If this window has no
    // tabs left afterward, it closes outright instead of lingering as an
    // empty duplicate.
    void moveTabToExistingWindow(DocumentTab *tab, MainWindow *target)
    {
        const int index = tabWidget_->indexOf(tab);
        if (index < 0) {
            return;
        }

        tab->reparentToWindow(target);
        tabWidget_->removeTab(index);
        target->adoptTab(tab);

        target->raise();
        target->activateWindow();

        if (tabWidget_->count() == 0) {
            close();
        }
    }

    // Moves a tab out of this window into a brand-new one, used by both the
    // tab context menu and the drag-out-of-the-bar gesture. globalPos, if
    // non-null, positions the new window near where the tab was dropped.
    void moveTabToNewWindow(DocumentTab *tab, const QPoint &globalPos)
    {
        const int index = tabWidget_->indexOf(tab);
        if (index < 0) {
            return;
        }

        auto *newWindow = new MainWindow();
        newWindow->setAttribute(Qt::WA_DeleteOnClose);

        // Reparent while the tab is still one of ours, so any in-flight
        // translations still tied to our translator are cancelled/handed
        // off correctly before we lose track of the tab.
        tab->reparentToWindow(newWindow);
        tabWidget_->removeTab(index);
        newWindow->adoptTab(tab);

        if (globalPos.isNull()) {
            newWindow->move(pos() + QPoint(48, 48));
        } else {
            newWindow->move(globalPos - QPoint(60, 20));
        }
        newWindow->resize(size());
        newWindow->show();
        newWindow->raise();
        newWindow->activateWindow();

        if (tabWidget_->count() == 0) {
            DocumentTab *fresh = newBlankTab();
            fresh->setInitialContent(QString());
        }
    }

    void onCurrentTabChanged(int index)
    {
        Q_UNUSED(index);
        DocumentTab *tab = activeTab();
        updateWindowTitle();
        updateEditActions();

        if (tab != nullptr) {
            const QSignalBlocker blocker1(originalModeAction_);
            const QSignalBlocker blocker2(bilingualModeAction_);
            const QSignalBlocker blocker3(translatedModeAction_);
            originalModeAction_->setChecked(tab->previewMode() == "original");
            bilingualModeAction_->setChecked(tab->previewMode() == "bilingual");
            translatedModeAction_->setChecked(tab->previewMode() == "translated");
        }

        if (findDialog_ != nullptr && findDialog_->isVisible()) {
            updatePreviewSearchHighlight();
        }

        if (tab != nullptr) {
            tab->applyPaneVisibility();
            tab->checkForExternalChanges();
        }
    }

    void newFile()
    {
        DocumentTab *tab = newBlankTab();
        tab->setInitialContent(QString());
        statusBar()->showMessage(uiText("newDocument"), 2000);
    }

    void openFile()
    {
        const QString path = QFileDialog::getOpenFileName(
            this,
            uiText("openMarkdown"),
            lastDialogDir_,
            markdownFilter());

        if (path.isEmpty()) {
            return;
        }

        openPathAsTab(path);
    }

    void openRecentFile(const QString &path)
    {
        if (!QFileInfo::exists(path)) {
            QMessageBox::warning(this, uiText("openFailed"), uiText("fileMissing").arg(path));
            recentFiles_.removeAll(path);
            saveRecentFiles();
            updateRecentFilesMenu();
            return;
        }

        openPathAsTab(path);
    }

    void loadRecentFiles()
    {
        QSettings settings;
        recentFiles_ = settings.value("recentFiles").toStringList();

        QStringList existing;
        for (const QString &path : recentFiles_) {
            if (QFileInfo::exists(path) && !existing.contains(path)) {
                existing.append(path);
            }
        }

        recentFiles_ = existing.mid(0, maxRecentFiles_);
    }

    void saveRecentFiles()
    {
        QSettings settings;
        settings.setValue("recentFiles", recentFiles_);
    }

    void updateRecentFilesMenu()
    {
        recentFilesMenu_->clear();

        for (const QString &path : recentFiles_) {
            auto *action = recentFilesMenu_->addAction(QFileInfo(path).fileName());
            action->setToolTip(path);
            connect(action, &QAction::triggered, this, [this, path] { openRecentFile(path); });
        }

        if (!recentFiles_.isEmpty()) {
            recentFilesMenu_->addSeparator();
        }

        recentFilesMenu_->addAction(clearRecentFilesAction_);
        clearRecentFilesAction_->setEnabled(!recentFiles_.isEmpty());
        recentFilesMenu_->setEnabled(!recentFiles_.isEmpty());
    }

    void updateEditActions()
    {
        DocumentTab *tab = activeTab();
        if (tab == nullptr) {
            return;
        }

        undoAction_->setEnabled(tab->editor()->document()->isUndoAvailable());
        redoAction_->setEnabled(tab->editor()->document()->isRedoAvailable());

        const bool hasSelection = tab->editor()->textCursor().hasSelection();
        cutAction_->setEnabled(hasSelection);
        copyAction_->setEnabled(hasSelection);
    }

    // --- Find / Replace (always targets the active tab) ------------------

    void showFindReplaceDialog(bool showReplace)
    {
        DocumentTab *tab = activeTab();
        if (tab == nullptr) {
            return;
        }

        if (findDialog_ == nullptr) {
            findDialog_ = new QDialog(this);

            auto *layout = new QGridLayout(findDialog_);
            findEdit_ = new QLineEdit(findDialog_);
            replaceEdit_ = new QLineEdit(findDialog_);
            findLabel_ = new QLabel(findDialog_);
            replaceLabel_ = new QLabel(findDialog_);
            matchCaseCheck_ = new QCheckBox(findDialog_);

            findNextButton_ = new QPushButton(findDialog_);
            findPreviousButton_ = new QPushButton(findDialog_);
            replaceButton_ = new QPushButton(findDialog_);
            replaceAllButton_ = new QPushButton(findDialog_);
            closeFindButton_ = new QPushButton(findDialog_);

            layout->addWidget(findLabel_, 0, 0);
            layout->addWidget(findEdit_, 0, 1, 1, 4);
            layout->addWidget(replaceLabel_, 1, 0);
            layout->addWidget(replaceEdit_, 1, 1, 1, 4);
            layout->addWidget(matchCaseCheck_, 2, 1, 1, 4);
            layout->addWidget(findPreviousButton_, 3, 0);
            layout->addWidget(findNextButton_, 3, 1);
            layout->addWidget(replaceButton_, 3, 2);
            layout->addWidget(replaceAllButton_, 3, 3);
            layout->addWidget(closeFindButton_, 3, 4);

            connect(findNextButton_, &QPushButton::clicked, this, [this] { findNext(); });
            connect(findPreviousButton_, &QPushButton::clicked, this, [this] { findPrevious(); });
            connect(replaceButton_, &QPushButton::clicked, this, [this] { replaceNext(); });
            connect(replaceAllButton_, &QPushButton::clicked, this, [this] { replaceAll(); });
            connect(closeFindButton_, &QPushButton::clicked, findDialog_, &QDialog::close);
            connect(findEdit_, &QLineEdit::returnPressed, this, [this] { findNext(); });
            connect(findEdit_, &QLineEdit::textChanged, this, [this] {
                updatePreviewSearchHighlight();
            });
            connect(matchCaseCheck_, &QCheckBox::toggled, this, [this] {
                updatePreviewSearchHighlight();
            });
            connect(findDialog_, &QDialog::finished, this, [this] {
                if (DocumentTab *activeTabPtr = activeTab()) {
                    activeTabPtr->preview()->page()->findText(QString());
                }
            });
            updateFindDialogTexts();
        }

        replaceLabel_->setVisible(showReplace);
        replaceEdit_->setVisible(showReplace);
        replaceButton_->setVisible(showReplace);
        replaceAllButton_->setVisible(showReplace);
        findDialog_->show();
        findDialog_->raise();
        findDialog_->activateWindow();

        const QString selectedText = tab->editor()->textCursor().selectedText();
        if (!selectedText.isEmpty() && !selectedText.contains(QChar::ParagraphSeparator)) {
            findEdit_->setText(selectedText);
        }

        findEdit_->setFocus();
        findEdit_->selectAll();
        updatePreviewSearchHighlight();
    }

    QWebEnginePage::FindFlags previewFindFlags(bool backward = false) const
    {
        QWebEnginePage::FindFlags flags;
        if (matchCaseCheck_ != nullptr && matchCaseCheck_->isChecked()) {
            flags |= QWebEnginePage::FindCaseSensitively;
        }
        if (backward) {
            flags |= QWebEnginePage::FindBackward;
        }
        return flags;
    }

    // Chromium's findText highlights every match and emphasizes the current
    // one; an empty needle clears the highlight.
    void updatePreviewSearchHighlight()
    {
        DocumentTab *tab = activeTab();
        if (tab == nullptr) {
            return;
        }
        if (findDialog_ == nullptr || !findDialog_->isVisible()) {
            tab->preview()->page()->findText(QString());
            return;
        }
        tab->preview()->page()->findText(findText(), previewFindFlags());
    }

    QTextDocument::FindFlags findFlags(bool backward = false) const
    {
        QTextDocument::FindFlags flags;
        if (matchCaseCheck_ != nullptr && matchCaseCheck_->isChecked()) {
            flags |= QTextDocument::FindCaseSensitively;
        }
        if (backward) {
            flags |= QTextDocument::FindBackward;
        }
        return flags;
    }

    QString findText() const
    {
        return findEdit_ == nullptr ? QString() : findEdit_->text();
    }

    void findNext() { findTextInDocument(false); }
    void findPrevious() { findTextInDocument(true); }

    bool findTextInDocument(bool backward)
    {
        if (findDialog_ == nullptr) {
            showFindReplaceDialog(false);
        }

        DocumentTab *tab = activeTab();
        if (tab == nullptr) {
            return false;
        }

        const QString text = findText();
        if (text.isEmpty()) {
            statusBar()->showMessage(uiText("enterFind"), 2000);
            return false;
        }

        tab->preview()->page()->findText(text, previewFindFlags(backward));

        if (tab->editor()->find(text, findFlags(backward))) {
            return true;
        }

        QTextCursor cursor = tab->editor()->textCursor();
        cursor.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
        tab->editor()->setTextCursor(cursor);

        if (tab->editor()->find(text, findFlags(backward))) {
            statusBar()->showMessage(uiText("searchWrapped"), 2000);
            return true;
        }

        statusBar()->showMessage(uiText("notFound").arg(text), 3000);
        return false;
    }

    void replaceNext()
    {
        DocumentTab *tab = activeTab();
        const QString text = findText();
        if (tab == nullptr || text.isEmpty()) {
            return;
        }

        QTextCursor cursor = tab->editor()->textCursor();
        const Qt::CaseSensitivity sensitivity = (matchCaseCheck_ != nullptr && matchCaseCheck_->isChecked())
            ? Qt::CaseSensitive
            : Qt::CaseInsensitive;

        if (!cursor.hasSelection() || QString::compare(cursor.selectedText(), text, sensitivity) != 0) {
            if (!findTextInDocument(false)) {
                return;
            }
            cursor = tab->editor()->textCursor();
        }

        cursor.insertText(replaceEdit_->text());
        tab->editor()->setTextCursor(cursor);
        findTextInDocument(false);
    }

    void replaceAll()
    {
        DocumentTab *tab = activeTab();
        const QString text = findText();
        if (tab == nullptr || text.isEmpty()) {
            return;
        }

        QTextCursor cursor(tab->editor()->document());
        cursor.beginEditBlock();

        int count = 0;
        while (true) {
            cursor = tab->editor()->document()->find(text, cursor, findFlags(false));
            if (cursor.isNull()) {
                break;
            }

            cursor.insertText(replaceEdit_->text());
            ++count;
        }

        cursor.endEditBlock();
        statusBar()->showMessage(uiText("replacedCount").arg(count), 3000);
    }

    // --- View settings -----------------------------------------------------

    void loadViewSettings()
    {
        QSettings settings;
        currentTheme_ = settings.value("theme", "light").toString();
        fontSize_ = settings.value("fontSize", defaultFontSize_).toInt();
        currentLanguage_ = settings.contains("language")
            ? settings.value("language").toString()
            : defaultLanguageForSystem();
        editorVisible_ = settings.value("editorVisible", true).toBool();
        editorFontFamily_ = settings.value("editorFontFamily", defaultEditorFontFamily()).toString();
        previewFontFamily_ = settings.value("previewFontFamily", defaultPreviewFontFamily()).toString();
        ollamaEndpoint_ = settings.value("ollamaEndpoint", defaultOllamaEndpoint()).toString();
        ollamaModel_ = settings.value("ollamaModel").toString();
        translationTarget_ = settings.value("translationTarget",
            defaultLanguageForSystem() == "ja" ? "Japanese" : "English").toString();
        ollamaParallel_ = qBound(1, settings.value("ollamaParallel", 2).toInt(), 8);

        if (ollamaEndpoint_.trimmed().isEmpty()) {
            ollamaEndpoint_ = defaultOllamaEndpoint();
        }

        if (currentTheme_ != "light" && currentTheme_ != "dark" && currentTheme_ != "sepia") {
            currentTheme_ = "light";
        }
        if (currentLanguage_ != "en" && currentLanguage_ != "ja") {
            currentLanguage_ = "en";
        }
        if (editorFontFamily_.isEmpty()) {
            editorFontFamily_ = defaultEditorFontFamily();
        }
        if (previewFontFamily_.isEmpty()) {
            previewFontFamily_ = defaultPreviewFontFamily();
        }
        fontSize_ = qBound(minFontSize_, fontSize_, maxFontSize_);
    }

    QString defaultLanguageForSystem() const
    {
        return QLocale::system().language() == QLocale::Japanese ? "ja" : "en";
    }

    void saveViewSettings()
    {
        QSettings settings;
        settings.setValue("theme", currentTheme_);
        settings.setValue("fontSize", fontSize_);
        settings.setValue("language", currentLanguage_);
        settings.setValue("editorVisible", editorVisible_);
        settings.setValue("editorFontFamily", editorFontFamily_);
        settings.setValue("previewFontFamily", previewFontFamily_);
        settings.setValue("ollamaEndpoint", ollamaEndpoint_);
        settings.setValue("ollamaModel", ollamaModel_);
        settings.setValue("translationTarget", translationTarget_);
        settings.setValue("ollamaParallel", ollamaParallel_);
    }

    void updateViewActions()
    {
        lightThemeAction_->setChecked(currentTheme_ == "light");
        darkThemeAction_->setChecked(currentTheme_ == "dark");
        sepiaThemeAction_->setChecked(currentTheme_ == "sepia");
        englishLanguageAction_->setChecked(currentLanguage_ == "en");
        japaneseLanguageAction_->setChecked(currentLanguage_ == "ja");
        toggleEditorAction_->setChecked(editorVisible_);
        decreaseFontSizeAction_->setEnabled(fontSize_ > minFontSize_);
        increaseFontSizeAction_->setEnabled(fontSize_ < maxFontSize_);
    }

    void applyPaneVisibility(bool announce)
    {
        for (DocumentTab *tab : allTabs()) {
            tab->applyPaneVisibility();
        }
        toggleEditorAction_->setChecked(editorVisible_);

        if (announce) {
            statusBar()->showMessage(editorVisible_ ? uiText("editorShown") : uiText("editorHidden"), 2000);
        }
    }

    void changeFontSize(int delta)
    {
        const int nextSize = qBound(minFontSize_, fontSize_ + delta, maxFontSize_);
        if (nextSize == fontSize_) {
            return;
        }

        fontSize_ = nextSize;
        applyViewSettings();
        saveViewSettings();
    }

    void chooseEditorFont()
    {
        QFont currentFont(editorFontFamily_, fontSize_);
        currentFont.setStyleHint(QFont::Monospace);

        bool ok = false;
        // Use Qt's own dialog: the GTK native font chooser previews a localized
        // (Japanese) sample string that Latin-only fonts render as tofu.
        const QFont selectedFont = QFontDialog::getFont(
            &ok, currentFont, this, uiText("editorFont"), QFontDialog::DontUseNativeDialog);
        if (!ok) {
            return;
        }

        editorFontFamily_ = selectedFont.family();
        fontSize_ = qBound(minFontSize_, selectedFont.pointSize() > 0 ? selectedFont.pointSize() : fontSize_, maxFontSize_);
        applyViewSettings();
        saveViewSettings();
    }

    void choosePreviewFont()
    {
        QFont currentFont(previewFontFamily_, fontSize_);

        bool ok = false;
        const QFont selectedFont = QFontDialog::getFont(
            &ok, currentFont, this, uiText("previewFont"), QFontDialog::DontUseNativeDialog);
        if (!ok) {
            return;
        }

        previewFontFamily_ = selectedFont.family();
        fontSize_ = qBound(minFontSize_, selectedFont.pointSize() > 0 ? selectedFont.pointSize() : fontSize_, maxFontSize_);
        applyViewSettings();
        saveViewSettings();
    }

    void applyViewSettings()
    {
        if (currentTheme_ == "dark") {
            setStyleSheet(
                "QMainWindow, QMenuBar, QMenu, QStatusBar { background: #202124; color: #e8eaed; }"
                "QStatusBar QLabel { color: #e8eaed; }"
                "QWidget#previewBar { background: #202124; }"
                "QPlainTextEdit, QTreeWidget { background: #1f1f1f; color: #e8eaed; border: 1px solid #3c4043; selection-background-color: #34517a; }"
                "QLineEdit, QCheckBox, QPushButton, QComboBox { background: #2b2c2f; color: #e8eaed; border: 1px solid #5f6368; padding: 3px; }"
                "QToolButton { background: #2b2c2f; color: #e8eaed; border: 1px solid #5f6368; padding: 3px 10px; }"
                "QToolButton:checked { background: #34517a; }"
                "QTabBar::tab { background: #2b2c2f; color: #e8eaed; padding: 6px 10px; }"
                "QTabBar::tab:selected { background: #34517a; }"
                "QTabWidget::pane { border: 1px solid #3c4043; }"
                "QTreeWidget::item:selected { background: #34517a; }");
        } else if (currentTheme_ == "sepia") {
            setStyleSheet(
                "QMainWindow, QMenuBar, QMenu, QStatusBar { background: #f3ead7; color: #43372b; }"
                "QStatusBar QLabel { color: #43372b; }"
                "QWidget#previewBar { background: #f3ead7; }"
                "QPlainTextEdit, QTreeWidget { background: #fbf4e6; color: #43372b; border: 1px solid #d4c2a3; selection-background-color: #d8c49a; }"
                "QLineEdit, QCheckBox, QPushButton, QComboBox { background: #fbf4e6; color: #43372b; border: 1px solid #d4c2a3; padding: 3px; }"
                "QToolButton { background: #fbf4e6; color: #43372b; border: 1px solid #d4c2a3; padding: 3px 10px; }"
                "QToolButton:checked { background: #d8c49a; }"
                "QTabBar::tab { background: #fbf4e6; color: #43372b; padding: 6px 10px; }"
                "QTabBar::tab:selected { background: #d8c49a; }"
                "QTabWidget::pane { border: 1px solid #d4c2a3; }"
                "QTreeWidget::item:selected { background: #d8c49a; }");
        } else {
            setStyleSheet(
                "QMainWindow, QMenuBar, QMenu, QStatusBar { background: #f6f7f9; color: #202124; }"
                "QStatusBar QLabel { color: #202124; }"
                "QWidget#previewBar { background: #f6f7f9; }"
                "QPlainTextEdit, QTreeWidget { background: #ffffff; color: #202124; border: 1px solid #d0d4dc; selection-background-color: #cfe3ff; selection-color: #111827; }"
                "QLineEdit, QCheckBox, QPushButton, QComboBox { background: #ffffff; color: #202124; border: 1px solid #d0d4dc; padding: 3px; }"
                "QToolButton { background: #ffffff; color: #202124; border: 1px solid #d0d4dc; padding: 3px 10px; }"
                "QToolButton:checked { background: #cfe3ff; color: #111827; }"
                "QTabBar::tab { background: #ffffff; color: #202124; padding: 6px 10px; }"
                "QTabBar::tab:selected { background: #cfe3ff; color: #111827; }"
                "QTabWidget::pane { border: 1px solid #d0d4dc; }"
                "QTreeWidget::item:selected { background: #cfe3ff; color: #111827; }");
        }

        for (DocumentTab *tab : allTabs()) {
            tab->applyFontsAndTheme();
        }

        updateViewActions();
        statusBar()->showMessage(uiText("viewStatus").arg(currentTheme_, QString::number(fontSize_)), 2000);
    }

    void updateWindowTitle()
    {
        DocumentTab *tab = activeTab();
        const QString name = (tab == nullptr || tab->filePath().isEmpty())
            ? uiText("untitled")
            : QFileInfo(tab->filePath()).fileName();
        const bool modified = tab != nullptr && tab->isModified();
        setWindowTitle(uiText("windowTitle").arg(name, modified ? "*" : ""));
    }

    MdvTabWidget *tabWidget_ = nullptr;
    QLabel *versionLabel_ = nullptr;
    QMenu *fileMenu_ = nullptr;
    QMenu *editMenu_ = nullptr;
    QMenu *searchMenu_ = nullptr;
    QMenu *viewMenu_ = nullptr;
    QMenu *themeMenu_ = nullptr;
    QMenu *languageMenu_ = nullptr;
    QMenu *recentFilesMenu_ = nullptr;
    QDialog *findDialog_ = nullptr;
    QLabel *findLabel_ = nullptr;
    QLineEdit *findEdit_ = nullptr;
    QLineEdit *replaceEdit_ = nullptr;
    QLabel *replaceLabel_ = nullptr;
    QCheckBox *matchCaseCheck_ = nullptr;
    QPushButton *findNextButton_ = nullptr;
    QPushButton *findPreviousButton_ = nullptr;
    QPushButton *replaceButton_ = nullptr;
    QPushButton *replaceAllButton_ = nullptr;
    QPushButton *closeFindButton_ = nullptr;
    QString lastDialogDir_ = defaultDialogDir();
    QString saveDialogDir_ = defaultDialogDir();
    QStringList recentFiles_;
    static constexpr int maxRecentFiles_ = 10;
    static constexpr qint64 largeFileWarningBytes_ = 10 * 1024 * 1024;
    static constexpr int defaultFontSize_ = 13;
    static constexpr int minFontSize_ = 9;
    static constexpr int maxFontSize_ = 28;
    QString currentTheme_ = "light";
    QString currentLanguage_ = "en";
    OllamaTranslator *translator_ = nullptr;
    QString ollamaEndpoint_ = defaultOllamaEndpoint();
    QString ollamaModel_;
    QString translationTarget_ = QStringLiteral("Japanese");
    QString editorFontFamily_;
    QString previewFontFamily_;
    int fontSize_ = defaultFontSize_;
    int ollamaParallel_ = 2;
    bool editorVisible_ = true;
    int untitledCounter_ = 1;

    QAction *newAction_ = nullptr;
    QAction *openAction_ = nullptr;
    QAction *saveAction_ = nullptr;
    QAction *saveAsAction_ = nullptr;
    QAction *closeTabAction_ = nullptr;
    QAction *clearRecentFilesAction_ = nullptr;
    QAction *exitAction_ = nullptr;
    QAction *undoAction_ = nullptr;
    QAction *redoAction_ = nullptr;
    QAction *cutAction_ = nullptr;
    QAction *copyAction_ = nullptr;
    QAction *pasteAction_ = nullptr;
    QAction *findAction_ = nullptr;
    QAction *replaceAction_ = nullptr;
    QAction *findNextAction_ = nullptr;
    QAction *findPreviousAction_ = nullptr;
    QActionGroup *themeActionGroup_ = nullptr;
    QAction *lightThemeAction_ = nullptr;
    QAction *darkThemeAction_ = nullptr;
    QAction *sepiaThemeAction_ = nullptr;
    QAction *increaseFontSizeAction_ = nullptr;
    QAction *decreaseFontSizeAction_ = nullptr;
    QAction *resetFontSizeAction_ = nullptr;
    QAction *chooseEditorFontAction_ = nullptr;
    QAction *choosePreviewFontAction_ = nullptr;
    QAction *resetFontsAction_ = nullptr;
    QAction *toggleEditorAction_ = nullptr;
    QActionGroup *languageActionGroup_ = nullptr;
    QAction *englishLanguageAction_ = nullptr;
    QAction *japaneseLanguageAction_ = nullptr;
    QMenu *translationMenu_ = nullptr;
    QActionGroup *translationModeGroup_ = nullptr;
    QAction *originalModeAction_ = nullptr;
    QAction *bilingualModeAction_ = nullptr;
    QAction *translatedModeAction_ = nullptr;
    QAction *translationSettingsAction_ = nullptr;
};

// ---------------------------------------------------------------------------
// DocumentTab out-of-line definitions (need MainWindow to be a complete type).
// ---------------------------------------------------------------------------

DocumentTab::DocumentTab(MainWindow *window, QWidget *parent)
    : QWidget(parent)
    , window_(window)
{
    outline_ = new QTreeWidget(this);
    editor_ = new QPlainTextEdit(this);
    preview_ = new QWebEngineView(this);

    outline_->setHeaderHidden(true);
    outline_->setMinimumWidth(180);
    outline_->setMaximumWidth(360);

    editor_->setLineWrapMode(QPlainTextEdit::WidgetWidth);

    QFont editorFont("Menlo");
    editorFont.setStyleHint(QFont::Monospace);
    editorFont.setPointSize(13);
    editor_->setFont(editorFont);

    preview_->setPage(new PreviewPage(preview_));
    preview_->setContextMenuPolicy(Qt::NoContextMenu);
    // Remote images (README badges etc.) referenced from the local page.
    preview_->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    preview_->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    connect(preview_, &QWebEngineView::loadFinished, this, [this](bool ok) {
        if (!ok) {
            return;
        }
        previewLoaded_ = true;
        pushPreviewContent();
    });

    auto *bridge = new PreviewBridge(this);
    bridge->onScrolled = [this](int headingCount, int segment, double t, double fraction) {
        syncEditorToPreview(headingCount, segment, t, fraction);
    };
    auto *channel = new QWebChannel(preview_->page());
    channel->registerObject(QStringLiteral("mdv"), bridge);
    preview_->page()->setWebChannel(channel);

    previewUpdateTimer_ = new QTimer(this);
    previewUpdateTimer_->setSingleShot(true);
    previewUpdateTimer_->setInterval(120);
    connect(previewUpdateTimer_, &QTimer::timeout, this, [this] {
        updateOutline();
        updatePreview();
    });

    translationPriorityTimer_ = new QTimer(this);
    translationPriorityTimer_->setSingleShot(true);
    translationPriorityTimer_->setInterval(400);
    connect(translationPriorityTimer_, &QTimer::timeout, this, [this] {
        reprioritizeTranslations();
    });

    fileWatcher_ = new QFileSystemWatcher(this);
    connect(fileWatcher_, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
        onFileChangedOnDisk(path);
    });

    auto *previewContainer = new QWidget(this);
    auto *previewLayout = new QVBoxLayout(previewContainer);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(0);

    auto *previewBar = new QWidget(previewContainer);
    previewBar->setObjectName(QStringLiteral("previewBar"));
    auto *previewBarLayout = new QHBoxLayout(previewBar);
    previewBarLayout->setContentsMargins(6, 4, 6, 4);
    previewBarLayout->setSpacing(4);

    originalModeButton_ = new QToolButton(previewBar);
    bilingualModeButton_ = new QToolButton(previewBar);
    translatedModeButton_ = new QToolButton(previewBar);
    for (QToolButton *button : {originalModeButton_, bilingualModeButton_, translatedModeButton_}) {
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setFocusPolicy(Qt::NoFocus);
        previewBarLayout->addWidget(button);
    }
    originalModeButton_->setChecked(true);
    previewBarLayout->addStretch();

    connect(originalModeButton_, &QToolButton::clicked, this, [this] { setPreviewMode("original"); });
    connect(bilingualModeButton_, &QToolButton::clicked, this, [this] { setPreviewMode("bilingual"); });
    connect(translatedModeButton_, &QToolButton::clicked, this, [this] { setPreviewMode("translated"); });

    previewLayout->addWidget(previewBar);
    previewLayout->addWidget(preview_, 1);

    splitter_ = new QSplitter(this);
    splitter_->addWidget(outline_);
    splitter_->addWidget(editor_);
    splitter_->addWidget(previewContainer);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setStretchFactor(2, 1);
    splitter_->setSizes({220, 440, 440});

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(splitter_);

    connect(editor_, &QPlainTextEdit::textChanged, this, [this] {
        documentModified_ = true;
        previewUpdateTimer_->start();
        window_->onTabStateChanged(this);
    });
    connect(editor_, &QPlainTextEdit::copyAvailable, this, [this](bool available) {
        window_->onEditorCopyAvailable(this, available);
    });
    connect(editor_, &QPlainTextEdit::undoAvailable, this, [this](bool available) {
        window_->onEditorUndoAvailable(this, available);
    });
    connect(editor_, &QPlainTextEdit::redoAvailable, this, [this](bool available) {
        window_->onEditorRedoAvailable(this, available);
    });
    connect(editor_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!syncingEditorFromPreview_) {
            syncPreviewToEditor();
        }
        translationPriorityTimer_->start();
    });

    connect(outline_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        jumpToHeading(item);
    });
    connect(outline_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        jumpToHeading(item);
    });

    updateUiTexts();
    applyFontsAndTheme();
    applyPaneVisibility();
}

bool DocumentTab::isActive() const
{
    return window_->activeTab() == this;
}

void DocumentTab::updateOutline()
{
    outline_->clear();

    QVector<QTreeWidgetItem *> latestByLevel(7, nullptr);
    bool inFence = false;
    QTextBlock block = editor_->document()->firstBlock();

    while (block.isValid()) {
        const QString line = block.text();
        const QString trimmed = line.trimmed();

        if (trimmed.startsWith("```") || trimmed.startsWith("~~~")) {
            inFence = !inFence;
            block = block.next();
            continue;
        }

        if (!inFence) {
            const Heading heading = parseHeading(line);
            if (heading.level > 0) {
                auto *item = new QTreeWidgetItem(QStringList(heading.title));
                item->setData(0, Qt::UserRole, block.position());

                QTreeWidgetItem *parent = nullptr;
                for (int level = heading.level - 1; level >= 1; --level) {
                    if (latestByLevel[level] != nullptr) {
                        parent = latestByLevel[level];
                        break;
                    }
                }

                if (parent != nullptr) {
                    parent->addChild(item);
                } else {
                    outline_->addTopLevelItem(item);
                }

                latestByLevel[heading.level] = item;
                for (int level = heading.level + 1; level < latestByLevel.size(); ++level) {
                    latestByLevel[level] = nullptr;
                }
            }
        }

        block = block.next();
    }

    outline_->expandAll();
}

void DocumentTab::updatePreview()
{
    if (previewMode_ != "original") {
        ensureTranslations();
    }
    pendingPreviewHtml_ = composePreviewHtml();

    if (previewBaseUrl() != loadedPreviewBaseUrl_) {
        reloadPreviewTemplate();
        return;
    }
    if (previewLoaded_) {
        pushPreviewContent();
    }
}

void DocumentTab::setPreviewMode(const QString &mode)
{
    // Translating needs a model; give the user a chance to pick one first.
    if (mode != "original" && window_->ollamaModel().trimmed().isEmpty()) {
        window_->showTranslationSettings();
        if (window_->ollamaModel().trimmed().isEmpty()) {
            updateTranslationModeUi();
            return;
        }
    }

    if (previewMode_ == mode) {
        updateTranslationModeUi();
        return;
    }

    previewMode_ = mode;
    if (previewMode_ == "original") {
        cancelOwnTranslations();
        if (isActive()) {
            window_->statusBar()->showMessage(window_->uiText("readyWithVersion").arg(QString::fromUtf8(MDV_VERSION)));
        }
    }
    updateTranslationModeUi();
    updatePreview();
}

void DocumentTab::cancelOwnTranslations()
{
    QSet<QString> keysToCancel;
    for (const QString &key : pendingTranslations_) {
        if (!window_->isKeyPendingInOtherTab(key, this)) {
            keysToCancel.insert(key);
        }
    }
    window_->translator()->cancelKeys(keysToCancel);
    pendingTranslations_.clear();
}

void DocumentTab::updateTranslationModeUi()
{
    originalModeButton_->setChecked(previewMode_ == "original");
    bilingualModeButton_->setChecked(previewMode_ == "bilingual");
    translatedModeButton_->setChecked(previewMode_ == "translated");
}

void DocumentTab::applyFontsAndTheme()
{
    QFont editorFont(window_->editorFontFamily(), window_->fontSize());
    editorFont.setStyleHint(QFont::Monospace);
    editorFont.setPointSize(window_->fontSize());
    editor_->setFont(editorFont);

    QFont outlineFont(window_->previewFontFamily(), qMax(window_->minFontSize(), window_->fontSize() - 1));
    outlineFont.setPointSize(qMax(window_->minFontSize(), window_->fontSize() - 1));
    outline_->setFont(outlineFont);

    pendingPreviewHtml_ = composePreviewHtml();
    reloadPreviewTemplate();
}

void DocumentTab::applyPaneVisibility()
{
    editor_->setVisible(window_->editorVisible());

    if (window_->editorVisible()) {
        splitter_->setSizes({220, 440, 440});
        if (isActive()) {
            editor_->setFocus();
        }
    } else {
        splitter_->setSizes({220, 0, 880});
        if (isActive()) {
            preview_->setFocus();
        }
    }
}

void DocumentTab::updateUiTexts()
{
    editor_->setPlaceholderText(window_->uiText("placeholder"));
    originalModeButton_->setText(window_->uiText("trOriginal"));
    bilingualModeButton_->setText(window_->uiText("trBilingual"));
    translatedModeButton_->setText(window_->uiText("trTranslated"));
}

bool DocumentTab::loadFile(const QString &path)
{
    const QFileInfo info(path);
    if (info.size() > window_->largeFileWarningBytes()) {
        const auto answer = QMessageBox::question(
            window_,
            window_->uiText("largeFileTitle"),
            window_->uiText("largeFileText").arg(QString::number(info.size() / (1024.0 * 1024.0), 'f', 1)),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) {
            return false;
        }
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(window_, window_->uiText("openFailed"), file.errorString());
        return false;
    }

    const QByteArray data = file.readAll();

    // Decode by BOM when present (UTF-16/32), otherwise expect UTF-8 and
    // warn before showing (and potentially saving) a lossy round-trip.
    const auto bomEncoding = QStringConverter::encodingForData(data);
    const QStringConverter::Encoding encoding = bomEncoding.value_or(QStringConverter::Utf8);
    QStringDecoder decoder(encoding, QStringConverter::Flag::Stateless);
    QString text = decoder(data);
    const bool lossy = decoder.hasError();

    if (lossy) {
        const auto answer = QMessageBox::warning(
            window_,
            window_->uiText("encodingTitle"),
            window_->uiText("encodingText"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) {
            return false;
        }
    }

    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    editor_->setPlainText(text);
    currentFile_ = path;
    window_->setLastDialogDir(info.absolutePath());
    fileEncoding_ = encoding;
    encodingLossy_ = lossy;
    documentModified_ = false;
    pendingExternalChange_ = false;
    watchCurrentFile();
    window_->addRecentFile(path);
    updatePreview();
    window_->onTabStateChanged(this);
    window_->statusBar()->showMessage(window_->uiText("opened").arg(path), 3000);
    return true;
}

bool DocumentTab::saveFile()
{
    if (currentFile_.isEmpty()) {
        return saveFileAs();
    }

    return writeFile(currentFile_);
}

bool DocumentTab::saveFileAs()
{
    const QString suggestedName = currentFile_.isEmpty()
        ? QStringLiteral("untitled.md")
        : QFileInfo(currentFile_).fileName();

    const QString path = QFileDialog::getSaveFileName(
        window_,
        window_->uiText("saveMarkdown"),
        QDir(window_->saveDialogDir()).filePath(suggestedName),
        window_->markdownFilter());

    if (path.isEmpty()) {
        return false;
    }

    return writeFile(path);
}

bool DocumentTab::writeFile(const QString &path)
{
    if (encodingLossy_) {
        const auto answer = QMessageBox::warning(
            window_,
            window_->uiText("encodingTitle"),
            window_->uiText("lossySaveText"),
            QMessageBox::Save | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (answer != QMessageBox::Save) {
            return false;
        }
        encodingLossy_ = false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(window_, window_->uiText("saveFailed"), file.errorString());
        return false;
    }

    QTextStream out(&file);
    if (fileEncoding_ != QStringConverter::Utf8) {
        out.setEncoding(fileEncoding_);
        out.setGenerateByteOrderMark(true);
    }
    out << editor_->toPlainText();

    // Our own write replaces the file on disk (QSaveFile writes a temp file
    // and renames it over the target); ignore the resulting fileChanged
    // signal instead of mistaking it for an external edit.
    suppressNextExternalChange_ = true;
    if (!file.commit()) {
        suppressNextExternalChange_ = false;
        QMessageBox::warning(window_, window_->uiText("saveFailed"), file.errorString());
        return false;
    }

    currentFile_ = path;
    window_->setLastDialogDir(QFileInfo(path).absolutePath());
    window_->setSaveDialogDir(QFileInfo(path).absolutePath());
    documentModified_ = false;
    pendingExternalChange_ = false;
    watchCurrentFile();
    window_->addRecentFile(path);
    window_->onTabStateChanged(this);
    window_->statusBar()->showMessage(window_->uiText("saved").arg(path), 3000);
    return true;
}

bool DocumentTab::confirmDiscardChanges()
{
    if (!documentModified_) {
        return true;
    }

    window_->activateTabForDialog(this);

    // If the file also changed on disk and the reload was declined, spell
    // out what Save/Discard mean here: Save overwrites that external change,
    // Discard abandons the local edits and leaves the external version as-is.
    const QString text = pendingExternalChange_
        ? window_->uiText("unsavedExternalText")
        : window_->uiText("unsavedText");

    const auto answer = QMessageBox::warning(
        window_,
        window_->uiText("unsavedTitle"),
        text,
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (answer == QMessageBox::Save) {
        return saveFile();
    }

    return answer == QMessageBox::Discard;
}

void DocumentTab::onFileChangedOnDisk(const QString &path)
{
    // Editors that replace-on-save (including our own QSaveFile-based
    // writeFile) can drop the path from the OS watch list; re-add it so
    // future genuine external edits keep being detected.
    if (QFileInfo::exists(path) && !fileWatcher_->files().contains(path)) {
        fileWatcher_->addPath(path);
    }

    if (suppressNextExternalChange_) {
        suppressNextExternalChange_ = false;
        return;
    }

    pendingExternalChange_ = true;
    if (isActive()) {
        checkForExternalChanges();
    }
}

void DocumentTab::checkForExternalChanges()
{
    if (!pendingExternalChange_ || externalChangeCheckInProgress_) {
        return;
    }

    externalChangeCheckInProgress_ = true;
    pendingExternalChange_ = false;
    window_->activateTabForDialog(this);

    const QString key = documentModified_ ? "externalChangeUnsavedText" : "externalChangeText";
    const auto answer = QMessageBox::question(
        window_,
        window_->uiText("externalChangeTitle"),
        window_->uiText(key).arg(QFileInfo(currentFile_).fileName()),
        QMessageBox::Yes | QMessageBox::No,
        documentModified_ ? QMessageBox::No : QMessageBox::Yes);

    externalChangeCheckInProgress_ = false;

    if (answer == QMessageBox::Yes) {
        loadFile(currentFile_);
    }
}

// Moves this tab to a different MainWindow. Must be called while the tab is
// still attached to its old window's tab bar, so any in-flight translation
// keys can be correctly reasoned about relative to the old window's other
// tabs before we stop being one of them.
void DocumentTab::reparentToWindow(MainWindow *newWindow)
{
    if (newWindow == nullptr || newWindow == window_) {
        return;
    }

    if (!pendingTranslations_.isEmpty()) {
        cancelOwnTranslations();
    }

    window_ = newWindow;

    applyFontsAndTheme();
    applyPaneVisibility();
    updateUiTexts();
    if (previewMode_ != "original") {
        updatePreview();
    }
}

void DocumentTab::pasteFromClipboard()
{
    const QMimeData *mimeData = QApplication::clipboard()->mimeData();
    if (mimeData != nullptr && mimeData->hasImage()) {
        if (pasteImageFromClipboard(mimeData)) {
            return;
        }
    }
    if (mimeData != nullptr && mimeData->hasUrls()) {
        if (pasteImageFileFromClipboard(mimeData)) {
            return;
        }
    }

    editor_->paste();
}

bool DocumentTab::pasteImageFromClipboard(const QMimeData *mimeData)
{
    const QImage image = qvariant_cast<QImage>(mimeData->imageData());
    if (image.isNull()) {
        return false;
    }

    const QString baseDirPath = currentFile_.isEmpty()
        ? QDir::currentPath()
        : QFileInfo(currentFile_).absolutePath();
    QDir baseDir(baseDirPath);

    if (!baseDir.exists("assets") && !baseDir.mkdir("assets")) {
        QMessageBox::warning(window_, window_->uiText("pasteImageFailed"), window_->uiText("createAssetsFailed"));
        return false;
    }

    const QString fileName = "image-" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss-zzz") + ".png";
    const QString absolutePath = baseDir.filePath("assets/" + fileName);
    if (!image.save(absolutePath, "PNG")) {
        QMessageBox::warning(window_, window_->uiText("pasteImageFailed"), window_->uiText("saveClipboardImageFailed"));
        return false;
    }

    const QString markdown = QString("![image](assets/%1)").arg(fileName);
    editor_->insertPlainText(markdown);
    window_->statusBar()->showMessage(window_->uiText("pastedImage").arg(markdown), 3000);
    return true;
}

bool DocumentTab::pasteImageFileFromClipboard(const QMimeData *mimeData)
{
    for (const QUrl &url : mimeData->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }

        const QFileInfo sourceInfo(url.toLocalFile());
        const QString suffix = sourceInfo.suffix().toLower();
        if (suffix != "png" && suffix != "jpg" && suffix != "jpeg" && suffix != "gif" && suffix != "webp") {
            continue;
        }

        const QString baseDirPath = currentFile_.isEmpty()
            ? QDir::currentPath()
            : QFileInfo(currentFile_).absolutePath();
        QDir baseDir(baseDirPath);

        if (!baseDir.exists("assets") && !baseDir.mkdir("assets")) {
            QMessageBox::warning(window_, window_->uiText("pasteImageFailed"), window_->uiText("createAssetsFailed"));
            return false;
        }

        const QString fileName = "image-" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss-zzz")
            + "." + suffix;
        const QString destination = baseDir.filePath("assets/" + fileName);
        if (!QFile::copy(sourceInfo.absoluteFilePath(), destination)) {
            QMessageBox::warning(window_, window_->uiText("pasteImageFailed"), window_->uiText("copyImageFailed"));
            return false;
        }

        const QString markdown = QString("![%1](assets/%2)").arg(sourceInfo.completeBaseName(), fileName);
        editor_->insertPlainText(markdown);
        window_->statusBar()->showMessage(window_->uiText("pastedImage").arg(markdown), 3000);
        return true;
    }

    return false;
}

QString DocumentTab::translationCacheKey(const QString &block) const
{
    return window_->ollamaModel() + QLatin1Char('\x1f') + window_->translationTarget() + QLatin1Char('\x1f') + block;
}

void DocumentTab::ensureTranslations()
{
    window_->translator()->configure(window_->ollamaEndpoint(), window_->ollamaModel(), window_->translationTarget());
    window_->translator()->setMaxInFlight(window_->ollamaParallel());

    QStringList orderedKeys;
    const QList<MdBlock> ordered = translatableBlocksByPriority();
    for (const MdBlock &block : ordered) {
        const QString key = translationCacheKey(block.text);
        if (translationCache_.contains(key)) {
            continue;
        }
        orderedKeys.append(key);
        if (!pendingTranslations_.contains(key)) {
            pendingTranslations_.insert(key);
            window_->translator()->requestTranslation(key, block.text);
        }
    }
    window_->translator()->prioritize(orderedKeys);

    if (!pendingTranslations_.isEmpty() && isActive()) {
        window_->statusBar()->showMessage(window_->uiText("translating").arg(pendingTranslations_.size()));
    }
}

// Called (debounced) when the editor viewport moves: waiting jobs are
// reordered so translation follows the reader.
void DocumentTab::reprioritizeTranslations()
{
    if (previewMode_ == "original" || pendingTranslations_.isEmpty()) {
        return;
    }

    QStringList orderedKeys;
    const QList<MdBlock> ordered = translatableBlocksByPriority();
    for (const MdBlock &block : ordered) {
        const QString key = translationCacheKey(block.text);
        if (pendingTranslations_.contains(key)) {
            orderedKeys.append(key);
        }
    }
    window_->translator()->prioritize(orderedKeys);
}

QString DocumentTab::composePreviewHtml() const
{
    const QString source = editor_->toPlainText();
    if (previewMode_ == "original") {
        return markdownToHtml(source);
    }

    const QList<MdBlock> blocks = splitMarkdownBlocks(source);

    // Untranslated and failed blocks fall back to the original text so
    // the document fills in progressively as results arrive.
    if (previewMode_ == "translated") {
        QStringList parts;
        parts.reserve(blocks.size());
        for (const MdBlock &block : blocks) {
            const QString translated = blockIsTranslatable(block.text)
                ? translationCache_.value(translationCacheKey(block.text))
                : QString();
            parts.append(translated.isEmpty() ? block.text : translated);
        }
        return markdownToHtml(parts.join(QLatin1String("\n\n")));
    }

    QString html;
    for (const MdBlock &blockItem : blocks) {
        const QString &block = blockItem.text;
        html += markdownToHtml(block);
        if (!blockIsTranslatable(block)) {
            continue;
        }
        const QString key = translationCacheKey(block);
        const QString translated = translationCache_.value(key);
        if (!translated.isEmpty()) {
            html += QLatin1String("<div class=\"mdv-tr\">") + markdownToHtml(translated)
                + QLatin1String("</div>");
        } else {
            // Cached-but-empty marks a block whose translation failed.
            const QString marker = translationCache_.contains(key)
                ? window_->uiText("trFailedBlock")
                : window_->uiText("trPending");
            html += QLatin1String("<div class=\"mdv-tr mdv-tr-pending\">")
                + marker.toHtmlEscaped() + QLatin1String("</div>");
        }
    }
    return html;
}

void DocumentTab::refreshTranslatedPreview()
{
    if (previewMode_ == "original") {
        return;
    }

    pendingPreviewHtml_ = composePreviewHtml();
    if (previewLoaded_) {
        pushPreviewContent();
    }

    if (!isActive()) {
        return;
    }

    if (pendingTranslations_.isEmpty()) {
        window_->statusBar()->showMessage(window_->uiText("translationDone"), 3000);
    } else {
        window_->statusBar()->showMessage(window_->uiText("translating").arg(pendingTranslations_.size()));
    }
}

void DocumentTab::reloadPreviewTemplate()
{
    previewLoaded_ = false;
    loadedPreviewBaseUrl_ = previewBaseUrl();
    preview_->setHtml(buildPreviewTemplate(), loadedPreviewBaseUrl_);
}

void DocumentTab::pushPreviewContent()
{
    const QByteArray json = QJsonDocument(QJsonArray{pendingPreviewHtml_}).toJson(QJsonDocument::Compact);
    preview_->page()->runJavaScript(
        QStringLiteral("__mdvSetContent(") + QString::fromUtf8(json) + QStringLiteral("[0]);"));
    syncPreviewToEditor();

    // Replacing the content drops Chromium's find highlights.
    window_->refreshSearchHighlightIfNeeded(this);
}

QString DocumentTab::buildPreviewTemplate() const
{
    QString bg, fg, link, codeBg, codeFg, quoteFg, quoteBorder, border;
    const QString theme = window_->currentTheme();
    if (theme == "dark") {
        bg = "#1f1f1f"; fg = "#e8eaed"; link = "#8ab4f8";
        codeBg = "#2b2c2f"; codeFg = "#f1f3f4";
        quoteFg = "#bdc1c6"; quoteBorder = "#5f6368"; border = "#3c4043";
    } else if (theme == "sepia") {
        bg = "#fbf4e6"; fg = "#43372b"; link = "#7b4f18";
        codeBg = "#efe2cb"; codeFg = "#43372b";
        quoteFg = "#6f604f"; quoteBorder = "#c8ae82"; border = "#d4c2a3";
    } else {
        bg = "#ffffff"; fg = "#202124"; link = "#0b57d0";
        codeBg = "#f1f3f4"; codeFg = "#202124";
        quoteFg = "#5f6368"; quoteBorder = "#dadce0"; border = "#d0d4dc";
    }

    const QString css = QString(
        "body { margin: 16px 20px; background: %1; color: %2; "
        "font-family: %3; font-size: %4pt; line-height: 1.55; overflow-wrap: break-word; }"
        "a { color: %5; }"
        "h1, h2 { border-bottom: 1px solid %6; padding-bottom: 0.3em; }"
        "code, pre, kbd { font-family: %7; background: %8; color: %9; border-radius: 4px; }"
        "code, kbd { padding: 2px 4px; font-size: 0.9em; }"
        "pre { padding: 10px 12px; overflow-x: auto; }"
        "pre code { padding: 0; background: transparent; }"
        "blockquote { margin-left: 0; padding-left: 12px; color: %10; border-left: 3px solid %11; }"
        "img { max-width: 100%; height: auto; }"
        "table { border-collapse: collapse; }"
        "th, td { border: 1px solid %6; padding: 4px 10px; }"
        "th { background: %8; }"
        "hr { border: none; border-top: 1px solid %6; }")
        .arg(bg, fg, window_->previewFontFamilyCss(), QString::number(window_->fontSize()), link,
             border, window_->editorFontFamilyCss(), codeBg, codeFg)
        .arg(quoteFg, quoteBorder)
        + QString(
        ".mdv-tr { margin: 2px 0 18px; padding: 0 0 0 12px; border-left: 3px solid %1; }"
        ".mdv-tr > :first-child { margin-top: 4px; }"
        ".mdv-tr > :last-child { margin-bottom: 4px; }"
        ".mdv-tr-pending { color: %2; font-style: italic; }")
        .arg(link, quoteFg);

    return QStringLiteral(
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><style>")
        + css
        + QStringLiteral("</style></head><body><div id=\"content\"></div><script>")
        + webChannelScript()
        + QStringLiteral("</script><script>")
        + previewScript()
        + QStringLiteral("</script></body></html>");
}

// Maps the editor's top visible position onto the source-position segments
// delimited by headings, then lets the page interpolate between the same
// headings' rendered offsets. Falls back to proportional scrolling when the
// heading lists do not pair up (e.g. setext headings the parser skips).
void DocumentTab::syncPreviewToEditor()
{
    if (!window_->editorVisible() || !previewLoaded_) {
        return;
    }

    const QVector<int> headings = editorHeadingPositions();
    const int sourceEnd = qMax(1, editor_->document()->characterCount() - 1);
    const int topPos = qBound(0, editor_->cursorForPosition(QPoint(0, 0)).position(), sourceEnd);

    QVector<int> anchors;
    anchors.reserve(headings.size() + 2);
    anchors.append(0);
    anchors.append(headings);
    anchors.append(sourceEnd);

    int segment = anchors.size() - 2;
    qreal t = 1.0;
    for (int i = 0; i + 1 < anchors.size(); ++i) {
        if (topPos > anchors.at(i + 1)) {
            continue;
        }
        const int span = anchors.at(i + 1) - anchors.at(i);
        t = span > 0 ? qreal(topPos - anchors.at(i)) / span : 0.0;
        segment = i;
        break;
    }

    preview_->page()->runJavaScript(QStringLiteral("__mdvSync(%1,%2,%3,%4);")
        .arg(headings.size())
        .arg(segment)
        .arg(t, 0, 'f', 4)
        .arg(qreal(topPos) / sourceEnd, 0, 'f', 6));
}

// Inverse of syncPreviewToEditor: the page reports which heading segment
// its viewport top sits in, and the editor scrolls to the interpolated
// source position of the same segment.
void DocumentTab::syncEditorToPreview(int headingCount, int segment, double t, double fraction)
{
    if (!window_->editorVisible() || !previewLoaded_) {
        return;
    }

    const QVector<int> headings = editorHeadingPositions();
    const int sourceEnd = qMax(1, editor_->document()->characterCount() - 1);

    int target;
    if (headingCount > 0 && headings.size() == headingCount) {
        QVector<int> anchors;
        anchors.reserve(headings.size() + 2);
        anchors.append(0);
        anchors.append(headings);
        anchors.append(sourceEnd);

        segment = qBound(0, segment, anchors.size() - 2);
        target = qRound(anchors.at(segment)
            + qBound(0.0, t, 1.0) * (anchors.at(segment + 1) - anchors.at(segment)));
    } else {
        target = qRound(qBound(0.0, fraction, 1.0) * sourceEnd);
    }

    scrollEditorToPosition(qBound(0, target, sourceEnd));
}

// Tabs can only be dragged between windows that live in the same process
// (moving a QWidget across a process boundary isn't possible), but the
// natural way to get "another mdv window" is just running mdv again, which
// would otherwise start a second, unrelated process. This makes a second
// launch hand its file arguments to the already-running instance, which
// opens them in a new window of its own, instead of starting a process of
// its own.
class SingleInstanceCoordinator : public QObject {
    Q_OBJECT

public:
    std::function<void(const QStringList &, bool)> onFilesReceived;

    explicit SingleInstanceCoordinator(QObject *parent = nullptr)
        : QObject(parent)
    {
        // Clears out a stale socket/pipe left behind by a crashed instance;
        // harmless (and a no-op) when nothing is actually listening.
        QLocalServer::removeServer(serverName());
        server_ = new QLocalServer(this);
        server_->listen(serverName());
        connect(server_, &QLocalServer::newConnection, this, &SingleInstanceCoordinator::acceptConnection);
    }

    // Tries to hand off `paths` (and the viewer-mode flag) to an
    // already-running instance. Returns true if one accepted the
    // connection, meaning the caller should exit without opening its own
    // window; false means this process should become the running instance.
    static bool handOffToRunningInstance(const QStringList &paths, bool viewerMode)
    {
        QLocalSocket socket;
        socket.connectToServer(serverName());
        if (!socket.waitForConnected(200)) {
            return false;
        }

        QByteArray payload;
        QDataStream stream(&payload, QIODevice::WriteOnly);
        stream << paths << viewerMode;

        socket.write(payload);
        socket.waitForBytesWritten(500);
        socket.disconnectFromServer();
        return true;
    }

private:
    static QString serverName()
    {
        return QStringLiteral("mdv-single-instance-%1").arg(QString::fromLocal8Bit(qgetenv("USERNAME")));
    }

    void acceptConnection()
    {
        while (QLocalSocket *socket = server_->nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
                QByteArray payload = socket->readAll();
                QDataStream stream(&payload, QIODevice::ReadOnly);
                QStringList paths;
                bool viewerMode = false;
                stream >> paths >> viewerMode;

                if (onFilesReceived) {
                    onFilesReceived(paths, viewerMode);
                }
                socket->deleteLater();
            });
        }
    }

    QLocalServer *server_ = nullptr;
};

// macOS delivers files opened via `open -a`, Finder, or Dock drops as
// QFileOpenEvent instead of command-line arguments.
class MdvApplication : public QApplication {
public:
    using QApplication::QApplication;

    std::function<void(const QString &)> fileOpenHandler;
    QString pendingFileOpen;

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::FileOpen) {
            const QString path = static_cast<QFileOpenEvent *>(event)->file();
            if (fileOpenHandler) {
                fileOpenHandler(path);
            } else {
                pendingFileOpen = path;
            }
            return true;
        }
        return QApplication::event(event);
    }
};

int main(int argc, char *argv[])
{
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    MdvApplication app(argc, argv);
    QApplication::setOrganizationName("mdv");
    QApplication::setApplicationName("mdv");
    QApplication::setApplicationVersion(QString::fromUtf8(MDV_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icon.svg")));
    QGuiApplication::setDesktopFileName(QStringLiteral("mdv"));

    QCommandLineParser parser;
    parser.setApplicationDescription("Simple Markdown viewer/editor");
    parser.addHelpOption();
    QCommandLineOption viewerModeOption(
        QStringList() << "v" << "viewer",
        "Start with the editor pane hidden.");
    parser.addOption(viewerModeOption);
    QCommandLineOption versionOption(
        QStringList() << "version",
        "Displays version information.");
    parser.addOption(versionOption);
    parser.addPositionalArgument("files", "Markdown files to open.", "[files...]");
    parser.process(app);

    if (parser.isSet(versionOption)) {
        QTextStream(stdout) << QApplication::applicationName() << " "
                             << QApplication::applicationVersion() << Qt::endl;
        return 0;
    }

    QStringList filesToOpen = parser.positionalArguments();

    // If mdv is already running, open these files (if any) in a new window
    // of that instance and quit: tabs can only be dragged between windows
    // in the same process, so a second OS process would be a dead end for
    // that even though it looks like "another mdv window" to the user.
    if (SingleInstanceCoordinator::handOffToRunningInstance(filesToOpen, parser.isSet(viewerModeOption))) {
        return 0;
    }

    auto *singleInstance = new SingleInstanceCoordinator(&app);

    MainWindow window(parser.isSet(viewerModeOption));
    app.fileOpenHandler = [&window](const QString &path) {
        window.openFileFromOsRequest(path);
    };

    singleInstance->onFilesReceived = [&window](const QStringList &paths, bool viewerMode) {
        auto *newWindow = new MainWindow(viewerMode);
        newWindow->setAttribute(Qt::WA_DeleteOnClose);
        for (const QString &path : paths) {
            newWindow->openFileFromCommandLine(path);
        }
        newWindow->ensureAtLeastOneTab();
        newWindow->move(window.pos() + QPoint(48, 48));
        newWindow->resize(window.size());
        newWindow->show();
        newWindow->raise();
        newWindow->activateWindow();
    };

    if (!app.pendingFileOpen.isEmpty()) {
        if (!filesToOpen.contains(app.pendingFileOpen)) {
            filesToOpen.prepend(app.pendingFileOpen);
        }
        app.pendingFileOpen.clear();
    }
    for (const QString &path : filesToOpen) {
        window.openFileFromCommandLine(path);
    }
    window.ensureAtLeastOneTab();

    window.show();

    return app.exec();
}

#include "main.moc"
