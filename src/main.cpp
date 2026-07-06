#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileOpenEvent>
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
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStringConverter>
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
#define MDV_VERSION "0.2.0"
#endif

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
// to two requests in flight so the HTTP round-trip overlaps generation.
// Results are keyed, so out-of-order completion is fine.
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
        connect(reply, &QNetworkReply::finished, this, [this, job, reply] {
            active_.remove(reply);
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
    bool cancelling_ = false;
    int maxInFlight_ = 2;
    QList<Job> queue_;
    QString endpoint_;
    QString model_;
    QString targetLanguage_;
};

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(bool startWithEditorHidden = false)
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

        translator_ = new OllamaTranslator(this);
        connect(translator_, &OllamaTranslator::translated, this, [this](const QString &key, const QString &text) {
            onBlockTranslated(key, text);
        });
        connect(translator_, &OllamaTranslator::blockFailed, this, [this](const QString &key, const QString &error) {
            onBlockFailed(key, error);
        });
        connect(translator_, &OllamaTranslator::failed, this, [this](const QString &error) {
            onTranslationFailed(error);
        });

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
        setCentralWidget(splitter_);

        loadViewSettings();
        if (startWithEditorHidden) {
            editorVisible_ = false;
        }
        createActions();
        createMenus();
        updateUiTexts();

        connect(editor_, &QPlainTextEdit::textChanged, this, [this] {
            documentModified_ = true;
            previewUpdateTimer_->start();
            updateWindowTitle();
        });
        connect(editor_, &QPlainTextEdit::copyAvailable, this, [this](bool available) {
            cutAction_->setEnabled(available);
            copyAction_->setEnabled(available);
        });
        connect(editor_, &QPlainTextEdit::undoAvailable, this, [this](bool available) {
            undoAction_->setEnabled(available);
        });
        connect(editor_, &QPlainTextEdit::redoAvailable, this, [this](bool available) {
            redoAction_->setEnabled(available);
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

        editor_->setPlainText(sampleMarkdown());
        documentModified_ = false;
        loadRecentFiles();
        updateRecentFilesMenu();
        updateEditActions();
        updateViewActions();
        applyViewSettings();
        applyPaneVisibility(false);
        updateOutline();
        updatePreview();
        updateWindowTitle();

        resize(1100, 720);
        showReadyStatus();
    }

    void openFileFromCommandLine(const QString &path)
    {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) {
            QMessageBox::warning(this, uiText("openFailed"), uiText("fileMissing").arg(path));
            return;
        }
        loadFile(info.absoluteFilePath());
    }

    // Files handed over by the OS (open -a, Finder "Open With", Dock drops)
    // arrive while the app may already be editing something.
    void openFileFromOsRequest(const QString &path)
    {
        if (!confirmDiscardChanges()) {
            return;
        }
        openFileFromCommandLine(path);
        raise();
        activateWindow();
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        if (confirmDiscardChanges()) {
            event->accept();
        } else {
            event->ignore();
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
        connect(saveAction_, &QAction::triggered, this, [this] { saveFile(); });

        saveAsAction_ = new QAction("Save &As...", this);
        saveAsAction_->setShortcut(QKeySequence::SaveAs);
        connect(saveAsAction_, &QAction::triggered, this, [this] { saveFileAs(); });

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
        connect(undoAction_, &QAction::triggered, editor_, &QPlainTextEdit::undo);

        redoAction_ = new QAction("&Redo", this);
        redoAction_->setShortcut(QKeySequence::Redo);
        connect(redoAction_, &QAction::triggered, editor_, &QPlainTextEdit::redo);

        cutAction_ = new QAction("Cu&t", this);
        cutAction_->setShortcut(QKeySequence::Cut);
        connect(cutAction_, &QAction::triggered, editor_, &QPlainTextEdit::cut);

        copyAction_ = new QAction("&Copy", this);
        copyAction_->setShortcut(QKeySequence::Copy);
        connect(copyAction_, &QAction::triggered, editor_, &QPlainTextEdit::copy);

        pasteAction_ = new QAction("&Paste", this);
        pasteAction_->setShortcut(QKeySequence::Paste);
        connect(pasteAction_, &QAction::triggered, this, [this] { pasteFromClipboard(); });

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
            setPreviewMode(action->data().toString());
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
        originalModeButton_->setText(uiText("trOriginal"));
        bilingualModeButton_->setText(uiText("trBilingual"));
        translatedModeButton_->setText(uiText("trTranslated"));

        englishLanguageAction_->setText("English");
        japaneseLanguageAction_->setText("日本語");
        englishLanguageAction_->setChecked(currentLanguage_ == "en");
        japaneseLanguageAction_->setChecked(currentLanguage_ == "ja");

        editor_->setPlaceholderText(uiText("placeholder"));
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

    void newFile()
    {
        if (!confirmDiscardChanges()) {
            return;
        }

        currentFile_.clear();
        editor_->clear();
        fileEncoding_ = QStringConverter::Utf8;
        encodingLossy_ = false;
        documentModified_ = false;
        updateOutline();
        updatePreview();
        updateWindowTitle();
        statusBar()->showMessage(uiText("newDocument"), 2000);
    }

    void openFile()
    {
        if (!confirmDiscardChanges()) {
            return;
        }

        const QString path = QFileDialog::getOpenFileName(
            this,
            uiText("openMarkdown"),
            lastDialogDir_,
            markdownFilter());

        if (path.isEmpty()) {
            return;
        }

        loadFile(path);
    }

    bool saveFile()
    {
        if (currentFile_.isEmpty()) {
            return saveFileAs();
        }

        return writeFile(currentFile_);
    }

    bool saveFileAs()
    {
        const QString suggestedName = currentFile_.isEmpty()
            ? QStringLiteral("untitled.md")
            : QFileInfo(currentFile_).fileName();

        const QString path = QFileDialog::getSaveFileName(
            this,
            uiText("saveMarkdown"),
            QDir(saveDialogDir_).filePath(suggestedName),
            markdownFilter());

        if (path.isEmpty()) {
            return false;
        }

        return writeFile(path);
    }

    void loadFile(const QString &path)
    {
        const QFileInfo info(path);
        if (info.size() > largeFileWarningBytes_) {
            const auto answer = QMessageBox::question(
                this,
                uiText("largeFileTitle"),
                uiText("largeFileText").arg(QString::number(info.size() / (1024.0 * 1024.0), 'f', 1)),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (answer != QMessageBox::Yes) {
                return;
            }
        }

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, uiText("openFailed"), file.errorString());
            return;
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
                this,
                uiText("encodingTitle"),
                uiText("encodingText"),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (answer != QMessageBox::Yes) {
                return;
            }
        }

        text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
        text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

        editor_->setPlainText(text);
        currentFile_ = path;
        lastDialogDir_ = info.absolutePath();
        fileEncoding_ = encoding;
        encodingLossy_ = lossy;
        documentModified_ = false;
        addRecentFile(path);
        updatePreview();
        updateWindowTitle();
        statusBar()->showMessage(uiText("opened").arg(path), 3000);
    }

    bool writeFile(const QString &path)
    {
        if (encodingLossy_) {
            const auto answer = QMessageBox::warning(
                this,
                uiText("encodingTitle"),
                uiText("lossySaveText"),
                QMessageBox::Save | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (answer != QMessageBox::Save) {
                return false;
            }
            encodingLossy_ = false;
        }

        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, uiText("saveFailed"), file.errorString());
            return false;
        }

        QTextStream out(&file);
        if (fileEncoding_ != QStringConverter::Utf8) {
            out.setEncoding(fileEncoding_);
            out.setGenerateByteOrderMark(true);
        }
        out << editor_->toPlainText();

        if (!file.commit()) {
            QMessageBox::warning(this, uiText("saveFailed"), file.errorString());
            return false;
        }

        currentFile_ = path;
        lastDialogDir_ = QFileInfo(path).absolutePath();
        saveDialogDir_ = lastDialogDir_;
        documentModified_ = false;
        addRecentFile(path);
        updateWindowTitle();
        statusBar()->showMessage(uiText("saved").arg(path), 3000);
        return true;
    }

    void openRecentFile(const QString &path)
    {
        if (!confirmDiscardChanges()) {
            return;
        }

        if (!QFileInfo::exists(path)) {
            QMessageBox::warning(this, uiText("openFailed"), uiText("fileMissing").arg(path));
            recentFiles_.removeAll(path);
            saveRecentFiles();
            updateRecentFilesMenu();
            return;
        }

        loadFile(path);
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
        undoAction_->setEnabled(editor_->document()->isUndoAvailable());
        redoAction_->setEnabled(editor_->document()->isRedoAvailable());

        const bool hasSelection = editor_->textCursor().hasSelection();
        cutAction_->setEnabled(hasSelection);
        copyAction_->setEnabled(hasSelection);
    }

    void pasteFromClipboard()
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

    bool pasteImageFromClipboard(const QMimeData *mimeData)
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
            QMessageBox::warning(this, uiText("pasteImageFailed"), uiText("createAssetsFailed"));
            return false;
        }

        const QString fileName = "image-" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss-zzz") + ".png";
        const QString absolutePath = baseDir.filePath("assets/" + fileName);
        if (!image.save(absolutePath, "PNG")) {
            QMessageBox::warning(this, uiText("pasteImageFailed"), uiText("saveClipboardImageFailed"));
            return false;
        }

        const QString markdown = QString("![image](assets/%1)").arg(fileName);
        editor_->insertPlainText(markdown);
        statusBar()->showMessage(uiText("pastedImage").arg(markdown), 3000);
        return true;
    }

    bool pasteImageFileFromClipboard(const QMimeData *mimeData)
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
                QMessageBox::warning(this, uiText("pasteImageFailed"), uiText("createAssetsFailed"));
                return false;
            }

            const QString fileName = "image-" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss-zzz")
                + "." + suffix;
            const QString destination = baseDir.filePath("assets/" + fileName);
            if (!QFile::copy(sourceInfo.absoluteFilePath(), destination)) {
                QMessageBox::warning(this, uiText("pasteImageFailed"), uiText("copyImageFailed"));
                return false;
            }

            const QString markdown = QString("![%1](assets/%2)").arg(sourceInfo.completeBaseName(), fileName);
            editor_->insertPlainText(markdown);
            statusBar()->showMessage(uiText("pastedImage").arg(markdown), 3000);
            return true;
        }

        return false;
    }

    void showFindReplaceDialog(bool showReplace)
    {
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
                preview_->page()->findText(QString());
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

        const QString selectedText = editor_->textCursor().selectedText();
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
        if (findDialog_ == nullptr || !findDialog_->isVisible()) {
            preview_->page()->findText(QString());
            return;
        }
        preview_->page()->findText(findText(), previewFindFlags());
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

    void findNext()
    {
        findTextInDocument(false);
    }

    void findPrevious()
    {
        findTextInDocument(true);
    }

    bool findTextInDocument(bool backward)
    {
        if (findDialog_ == nullptr) {
            showFindReplaceDialog(false);
        }

        const QString text = findText();
        if (text.isEmpty()) {
            statusBar()->showMessage(uiText("enterFind"), 2000);
            return false;
        }

        preview_->page()->findText(text, previewFindFlags(backward));

        if (editor_->find(text, findFlags(backward))) {
            return true;
        }

        QTextCursor cursor = editor_->textCursor();
        cursor.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
        editor_->setTextCursor(cursor);

        if (editor_->find(text, findFlags(backward))) {
            statusBar()->showMessage(uiText("searchWrapped"), 2000);
            return true;
        }

        statusBar()->showMessage(uiText("notFound").arg(text), 3000);
        return false;
    }

    void replaceNext()
    {
        const QString text = findText();
        if (text.isEmpty()) {
            return;
        }

        QTextCursor cursor = editor_->textCursor();
        const Qt::CaseSensitivity sensitivity = (matchCaseCheck_ != nullptr && matchCaseCheck_->isChecked())
            ? Qt::CaseSensitive
            : Qt::CaseInsensitive;

        if (!cursor.hasSelection() || QString::compare(cursor.selectedText(), text, sensitivity) != 0) {
            if (!findTextInDocument(false)) {
                return;
            }
            cursor = editor_->textCursor();
        }

        cursor.insertText(replaceEdit_->text());
        editor_->setTextCursor(cursor);
        findTextInDocument(false);
    }

    void replaceAll()
    {
        const QString text = findText();
        if (text.isEmpty()) {
            return;
        }

        QTextCursor cursor(editor_->document());
        cursor.beginEditBlock();

        int count = 0;
        while (true) {
            cursor = editor_->document()->find(text, cursor, findFlags(false));
            if (cursor.isNull()) {
                break;
            }

            cursor.insertText(replaceEdit_->text());
            ++count;
        }

        cursor.endEditBlock();
        statusBar()->showMessage(uiText("replacedCount").arg(count), 3000);
    }

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

    static QString defaultOllamaEndpoint()
    {
        return QStringLiteral("http://127.0.0.1:11434");
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
        editor_->setVisible(editorVisible_);
        toggleEditorAction_->setChecked(editorVisible_);

        if (editorVisible_) {
            splitter_->setSizes({220, 440, 440});
            editor_->setFocus();
        } else {
            splitter_->setSizes({220, 0, 880});
            preview_->setFocus();
        }

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

    // The working directory is the natural starting point when launched from
    // a terminal; Finder-launched apps run with cwd "/", where home is nicer.
    static QString defaultDialogDir()
    {
        const QString cwd = QDir::currentPath();
        return (cwd.isEmpty() || cwd == QLatin1String("/")) ? QDir::homePath() : cwd;
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

    static QString cssQuotedFontFamily(const QString &family)
    {
        QString escaped = family;
        escaped.replace("\\", "\\\\");
        escaped.replace("'", "\\'");
        return "'" + escaped + "'";
    }

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

    void applyViewSettings()
    {
        QFont editorFont(editorFontFamily_, fontSize_);
        editorFont.setStyleHint(QFont::Monospace);
        editorFont.setPointSize(fontSize_);
        editor_->setFont(editorFont);

        QFont outlineFont(previewFontFamily_, qMax(minFontSize_, fontSize_ - 1));
        outlineFont.setPointSize(qMax(minFontSize_, fontSize_ - 1));
        outline_->setFont(outlineFont);

        if (currentTheme_ == "dark") {
            setStyleSheet(
                "QMainWindow, QMenuBar, QMenu, QStatusBar { background: #202124; color: #e8eaed; }"
                "QWidget#previewBar { background: #202124; }"
                "QPlainTextEdit, QTreeWidget { background: #1f1f1f; color: #e8eaed; border: 1px solid #3c4043; selection-background-color: #34517a; }"
                "QLineEdit, QCheckBox, QPushButton, QComboBox { background: #2b2c2f; color: #e8eaed; border: 1px solid #5f6368; padding: 3px; }"
                "QToolButton { background: #2b2c2f; color: #e8eaed; border: 1px solid #5f6368; padding: 3px 10px; }"
                "QToolButton:checked { background: #34517a; }"
                "QTreeWidget::item:selected { background: #34517a; }");
        } else if (currentTheme_ == "sepia") {
            setStyleSheet(
                "QMainWindow, QMenuBar, QMenu, QStatusBar { background: #f3ead7; color: #43372b; }"
                "QWidget#previewBar { background: #f3ead7; }"
                "QPlainTextEdit, QTreeWidget { background: #fbf4e6; color: #43372b; border: 1px solid #d4c2a3; selection-background-color: #d8c49a; }"
                "QLineEdit, QCheckBox, QPushButton, QComboBox { background: #fbf4e6; color: #43372b; border: 1px solid #d4c2a3; padding: 3px; }"
                "QToolButton { background: #fbf4e6; color: #43372b; border: 1px solid #d4c2a3; padding: 3px 10px; }"
                "QToolButton:checked { background: #d8c49a; }"
                "QTreeWidget::item:selected { background: #d8c49a; }");
        } else {
            setStyleSheet(
                "QMainWindow, QMenuBar, QMenu, QStatusBar { background: #f6f7f9; color: #202124; }"
                "QWidget#previewBar { background: #f6f7f9; }"
                "QPlainTextEdit, QTreeWidget { background: #ffffff; color: #202124; border: 1px solid #d0d4dc; selection-background-color: #cfe3ff; selection-color: #111827; }"
                "QLineEdit, QCheckBox, QPushButton, QComboBox { background: #ffffff; color: #202124; border: 1px solid #d0d4dc; padding: 3px; }"
                "QToolButton { background: #ffffff; color: #202124; border: 1px solid #d0d4dc; padding: 3px 10px; }"
                "QToolButton:checked { background: #cfe3ff; color: #111827; }"
                "QTreeWidget::item:selected { background: #cfe3ff; color: #111827; }");
        }

        pendingPreviewHtml_ = composePreviewHtml();
        reloadPreviewTemplate();
        updateViewActions();
        statusBar()->showMessage(uiText("viewStatus").arg(currentTheme_, QString::number(fontSize_)), 2000);
    }

    bool confirmDiscardChanges()
    {
        if (!documentModified_) {
            return true;
        }

        const auto answer = QMessageBox::warning(
            this,
            uiText("unsavedTitle"),
            uiText("unsavedText"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);

        if (answer == QMessageBox::Save) {
            return saveFile();
        }

        return answer == QMessageBox::Discard;
    }

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

    QUrl previewBaseUrl() const
    {
        const QString baseDir = currentFile_.isEmpty()
            ? QDir::currentPath()
            : QFileInfo(currentFile_).absolutePath();
        return QUrl::fromLocalFile(baseDir + "/");
    }

    void updatePreview()
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

    void setPreviewMode(const QString &mode)
    {
        // Translating needs a model; give the user a chance to pick one first.
        if (mode != "original" && ollamaModel_.trimmed().isEmpty()) {
            showTranslationSettings();
            if (ollamaModel_.trimmed().isEmpty()) {
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
            translator_->cancelAll();
            pendingTranslations_.clear();
            showReadyStatus();
        }
        updateTranslationModeUi();
        updatePreview();
    }

    void updateTranslationModeUi()
    {
        originalModeButton_->setChecked(previewMode_ == "original");
        bilingualModeButton_->setChecked(previewMode_ == "bilingual");
        translatedModeButton_->setChecked(previewMode_ == "translated");
        originalModeAction_->setChecked(previewMode_ == "original");
        bilingualModeAction_->setChecked(previewMode_ == "bilingual");
        translatedModeAction_->setChecked(previewMode_ == "translated");
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

    QString translationCacheKey(const QString &block) const
    {
        return ollamaModel_ + QLatin1Char('\x1f') + translationTarget_ + QLatin1Char('\x1f') + block;
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

    void ensureTranslations()
    {
        translator_->configure(ollamaEndpoint_, ollamaModel_, translationTarget_);
        translator_->setMaxInFlight(ollamaParallel_);

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
                translator_->requestTranslation(key, block.text);
            }
        }
        translator_->prioritize(orderedKeys);

        if (!pendingTranslations_.isEmpty()) {
            statusBar()->showMessage(uiText("translating").arg(pendingTranslations_.size()));
        }
    }

    // Called (debounced) when the editor viewport moves: waiting jobs are
    // reordered so translation follows the reader.
    void reprioritizeTranslations()
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
        translator_->prioritize(orderedKeys);
    }

    QString composePreviewHtml() const
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
                    ? uiText("trFailedBlock")
                    : uiText("trPending");
                html += QLatin1String("<div class=\"mdv-tr mdv-tr-pending\">")
                    + marker.toHtmlEscaped() + QLatin1String("</div>");
            }
        }
        return html;
    }

    void onBlockTranslated(const QString &key, const QString &text)
    {
        pendingTranslations_.remove(key);
        translationCache_.insert(key, text);
        refreshTranslatedPreview();
    }

    // A failed block is cached as an empty string: it renders as its
    // original text (with a marker in bilingual view) and is not retried
    // until the app restarts or the block is edited.
    void onBlockFailed(const QString &key, const QString &error)
    {
        pendingTranslations_.remove(key);
        translationCache_.insert(key, QString());
        refreshTranslatedPreview();
    }

    void refreshTranslatedPreview()
    {
        if (previewMode_ == "original") {
            return;
        }

        pendingPreviewHtml_ = composePreviewHtml();
        if (previewLoaded_) {
            pushPreviewContent();
        }

        if (pendingTranslations_.isEmpty()) {
            statusBar()->showMessage(uiText("translationDone"), 3000);
        } else {
            statusBar()->showMessage(uiText("translating").arg(pendingTranslations_.size()));
        }
    }

    void onTranslationFailed(const QString &error)
    {
        pendingTranslations_.clear();
        if (previewMode_ == "original") {
            return;
        }

        setPreviewMode("original");
        QMessageBox::warning(this, uiText("translationFailedTitle"),
            uiText("translationFailedText").arg(error));
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

        if (previewMode_ != "original") {
            updatePreview();
        }
    }

    static QString cleanedEndpoint(const QString &endpoint)
    {
        QString cleaned = endpoint.trimmed();
        while (cleaned.endsWith(QLatin1Char('/'))) {
            cleaned.chop(1);
        }
        return cleaned.isEmpty() ? defaultOllamaEndpoint() : cleaned;
    }

    void reloadPreviewTemplate()
    {
        previewLoaded_ = false;
        loadedPreviewBaseUrl_ = previewBaseUrl();
        preview_->setHtml(buildPreviewTemplate(), loadedPreviewBaseUrl_);
    }

    void pushPreviewContent()
    {
        const QByteArray json = QJsonDocument(QJsonArray{pendingPreviewHtml_}).toJson(QJsonDocument::Compact);
        preview_->page()->runJavaScript(
            QStringLiteral("__mdvSetContent(") + QString::fromUtf8(json) + QStringLiteral("[0]);"));
        syncPreviewToEditor();

        // Replacing the content drops Chromium's find highlights.
        if (findDialog_ != nullptr && findDialog_->isVisible() && !findText().isEmpty()) {
            updatePreviewSearchHighlight();
        }
    }

    QString buildPreviewTemplate() const
    {
        QString bg, fg, link, codeBg, codeFg, quoteFg, quoteBorder, border;
        if (currentTheme_ == "dark") {
            bg = "#1f1f1f"; fg = "#e8eaed"; link = "#8ab4f8";
            codeBg = "#2b2c2f"; codeFg = "#f1f3f4";
            quoteFg = "#bdc1c6"; quoteBorder = "#5f6368"; border = "#3c4043";
        } else if (currentTheme_ == "sepia") {
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
            .arg(bg, fg, previewFontFamilyCss(), QString::number(fontSize_), link,
                 border, editorFontFamilyCss(), codeBg, codeFg)
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

    // Maps the editor's top visible position onto the source-position segments
    // delimited by headings, then lets the page interpolate between the same
    // headings' rendered offsets. Falls back to proportional scrolling when the
    // heading lists do not pair up (e.g. setext headings the parser skips).
    void syncPreviewToEditor()
    {
        if (!editorVisible_ || !previewLoaded_) {
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
    void syncEditorToPreview(int headingCount, int segment, double t, double fraction)
    {
        if (!editorVisible_ || !previewLoaded_) {
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

    // QPlainTextEdit has no public "scroll position X to the top" API, so
    // binary-search the scrollbar (its value is monotonic in the top visible
    // position) until the wanted source position is at the top of the view.
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

    void updateOutline()
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

    void updateWindowTitle()
    {
        const QString name = currentFile_.isEmpty() ? uiText("untitled") : QFileInfo(currentFile_).fileName();
        setWindowTitle(uiText("windowTitle").arg(name, documentModified_ ? "*" : ""));
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

    QTreeWidget *outline_ = nullptr;
    QPlainTextEdit *editor_ = nullptr;
    QWebEngineView *preview_ = nullptr;
    QTimer *previewUpdateTimer_ = nullptr;
    QString pendingPreviewHtml_;
    QUrl loadedPreviewBaseUrl_;
    bool previewLoaded_ = false;
    QSplitter *splitter_ = nullptr;
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
    QString currentFile_;
    QString lastDialogDir_ = defaultDialogDir();
    QString saveDialogDir_ = defaultDialogDir();
    QStringList recentFiles_;
    static constexpr int maxRecentFiles_ = 10;
    static constexpr qint64 largeFileWarningBytes_ = 10 * 1024 * 1024;
    QStringConverter::Encoding fileEncoding_ = QStringConverter::Utf8;
    bool encodingLossy_ = false;
    static constexpr int defaultFontSize_ = 13;
    static constexpr int minFontSize_ = 9;
    static constexpr int maxFontSize_ = 28;
    QString currentTheme_ = "light";
    QString currentLanguage_ = "en";
    OllamaTranslator *translator_ = nullptr;
    QString previewMode_ = QStringLiteral("original");
    QHash<QString, QString> translationCache_;
    QSet<QString> pendingTranslations_;
    QString ollamaEndpoint_ = defaultOllamaEndpoint();
    QString ollamaModel_;
    QString translationTarget_ = QStringLiteral("Japanese");
    QToolButton *originalModeButton_ = nullptr;
    QToolButton *bilingualModeButton_ = nullptr;
    QToolButton *translatedModeButton_ = nullptr;
    QString editorFontFamily_;
    QString previewFontFamily_;
    int fontSize_ = defaultFontSize_;
    int ollamaParallel_ = 2;
    QTimer *translationPriorityTimer_ = nullptr;
    bool editorVisible_ = true;
    bool documentModified_ = false;
    bool syncingEditorFromPreview_ = false;

    QAction *newAction_ = nullptr;
    QAction *openAction_ = nullptr;
    QAction *saveAction_ = nullptr;
    QAction *saveAsAction_ = nullptr;
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
    parser.addPositionalArgument("file", "Markdown file to open.", "[file]");
    parser.process(app);

    MainWindow window(parser.isSet(viewerModeOption));
    app.fileOpenHandler = [&window](const QString &path) {
        window.openFileFromOsRequest(path);
    };

    const QStringList positional = parser.positionalArguments();
    if (!app.pendingFileOpen.isEmpty()) {
        window.openFileFromCommandLine(app.pendingFileOpen);
        app.pendingFileOpen.clear();
    } else if (!positional.isEmpty()) {
        window.openFileFromCommandLine(positional.first());
    }
    window.show();

    return app.exec();
}

#include "main.moc"
