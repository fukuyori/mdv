#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QFont>
#include <QFontDialog>
#include <QGridLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QStringConverter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextStream>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVector>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include <functional>

#include "md4c-html.h"

#ifndef MDV_VERSION
#define MDV_VERSION "0.1.1"
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

        previewUpdateTimer_ = new QTimer(this);
        previewUpdateTimer_->setSingleShot(true);
        previewUpdateTimer_->setInterval(120);
        connect(previewUpdateTimer_, &QTimer::timeout, this, [this] {
            updateOutline();
            updatePreview();
        });

        splitter_ = new QSplitter(this);
        splitter_->addWidget(outline_);
        splitter_->addWidget(editor_);
        splitter_->addWidget(preview_);
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
        currentLanguage_ = settings.value("language", "en").toString();
        editorVisible_ = settings.value("editorVisible", true).toBool();
        editorFontFamily_ = settings.value("editorFontFamily", defaultEditorFontFamily()).toString();
        previewFontFamily_ = settings.value("previewFontFamily", defaultPreviewFontFamily()).toString();

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

    void saveViewSettings()
    {
        QSettings settings;
        settings.setValue("theme", currentTheme_);
        settings.setValue("fontSize", fontSize_);
        settings.setValue("language", currentLanguage_);
        settings.setValue("editorVisible", editorVisible_);
        settings.setValue("editorFontFamily", editorFontFamily_);
        settings.setValue("previewFontFamily", previewFontFamily_);
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
        const QFont selectedFont = QFontDialog::getFont(&ok, currentFont, this, uiText("editorFont"));
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
        const QFont selectedFont = QFontDialog::getFont(&ok, currentFont, this, uiText("previewFont"));
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
                "QPlainTextEdit, QTreeWidget { background: #1f1f1f; color: #e8eaed; border: 1px solid #3c4043; selection-background-color: #34517a; }"
                "QLineEdit, QCheckBox, QPushButton { background: #2b2c2f; color: #e8eaed; border: 1px solid #5f6368; padding: 3px; }"
                "QTreeWidget::item:selected { background: #34517a; }");
        } else if (currentTheme_ == "sepia") {
            setStyleSheet(
                "QMainWindow, QMenuBar, QMenu, QStatusBar { background: #f3ead7; color: #43372b; }"
                "QPlainTextEdit, QTreeWidget { background: #fbf4e6; color: #43372b; border: 1px solid #d4c2a3; selection-background-color: #d8c49a; }"
                "QLineEdit, QCheckBox, QPushButton { background: #fbf4e6; color: #43372b; border: 1px solid #d4c2a3; padding: 3px; }"
                "QTreeWidget::item:selected { background: #d8c49a; }");
        } else {
            setStyleSheet(
                "QMainWindow, QMenuBar, QMenu, QStatusBar { background: #f6f7f9; color: #202124; }"
                "QPlainTextEdit, QTreeWidget { background: #ffffff; color: #202124; border: 1px solid #d0d4dc; selection-background-color: #cfe3ff; selection-color: #111827; }"
                "QLineEdit, QCheckBox, QPushButton { background: #ffffff; color: #202124; border: 1px solid #d0d4dc; padding: 3px; }"
                "QTreeWidget::item:selected { background: #cfe3ff; color: #111827; }");
        }

        pendingPreviewHtml_ = markdownToHtml(editor_->toPlainText());
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
        pendingPreviewHtml_ = markdownToHtml(editor_->toPlainText());

        if (previewBaseUrl() != loadedPreviewBaseUrl_) {
            reloadPreviewTemplate();
            return;
        }
        if (previewLoaded_) {
            pushPreviewContent();
        }
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
            .arg(quoteFg, quoteBorder);

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
    QString editorFontFamily_;
    QString previewFontFamily_;
    int fontSize_ = defaultFontSize_;
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
