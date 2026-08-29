#include "preview_policy.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using preview_policy::allowsLocalResource;

    QTemporaryDir root;
    require(root.isValid(), "temporary directory unavailable");
    const QString docDir = root.filePath(QStringLiteral("doc"));
    const QString outside = root.filePath(QStringLiteral("outside"));
    require(QDir().mkpath(docDir + "/assets"), "mkpath doc/assets");
    require(QDir().mkpath(outside), "mkpath outside");
    require(writeFile(docDir + "/assets/img.png", "png"), "write img.png");
    require(writeFile(outside + "/secret.txt", "secret"), "write secret.txt");

    const QString dir = docDir;

    require(allowsLocalResource(QUrl::fromLocalFile(docDir + "/assets/img.png"), dir),
        "image inside the document directory must be allowed");
    require(allowsLocalResource(QUrl::fromLocalFile(docDir + "/missing.png"), dir),
        "missing file inside the directory must be allowed (nothing to disclose)");
    require(!allowsLocalResource(QUrl::fromLocalFile(docDir + "/../outside/secret.txt"), dir),
        "'..' escape must be blocked");
    require(!allowsLocalResource(QUrl::fromLocalFile(outside + "/secret.txt"), dir),
        "absolute path outside the directory must be blocked");
    require(!allowsLocalResource(QUrl::fromLocalFile(docDir + "-other/x.png"), dir),
        "sibling directory sharing a prefix must be blocked");
    require(!allowsLocalResource(QUrl::fromLocalFile(docDir), dir),
        "directory itself must be blocked");
    require(!allowsLocalResource(QUrl::fromLocalFile(docDir + "/assets"), dir),
        "subdirectory must be blocked");
    require(!allowsLocalResource(QUrl(QStringLiteral("file:///etc/hostname")), dir),
        "system file must be blocked");
    require(!allowsLocalResource(QUrl(QStringLiteral("http://example.com/x.png")), dir),
        "remote URL must be blocked by the local policy");
    require(!allowsLocalResource(QUrl(QStringLiteral("file://evil.example/share/x.png")), dir),
        "file URL with a remote host must be blocked");
    require(!allowsLocalResource(QUrl::fromLocalFile(docDir + "/assets/img.png"), QString()),
        "empty document directory must block everything");
    require(!allowsLocalResource(QUrl::fromLocalFile(docDir + "/assets/img.png"), docDir + "/nonexistent"),
        "nonexistent document directory must block everything");

#ifdef Q_OS_UNIX
    require(QFile::link(outside + "/secret.txt", docDir + "/link.txt"), "create file symlink");
    require(!allowsLocalResource(QUrl::fromLocalFile(docDir + "/link.txt"), dir),
        "symlink pointing outside the directory must be blocked");
    require(QFile::link(outside, docDir + "/linkdir"), "create directory symlink");
    require(!allowsLocalResource(QUrl::fromLocalFile(docDir + "/linkdir/secret.txt"), dir),
        "file below a directory symlink pointing outside must be blocked");
    require(QFile::link(docDir + "/assets/img.png", docDir + "/inside.png"), "create inside symlink");
    require(allowsLocalResource(QUrl::fromLocalFile(docDir + "/inside.png"), dir),
        "symlink resolving inside the directory must be allowed");

    // A document opened through a symlinked directory keeps working.
    const QString aliasDir = root.filePath(QStringLiteral("alias"));
    require(QFile::link(docDir, aliasDir), "create directory alias");
    require(allowsLocalResource(QUrl::fromLocalFile(aliasDir + "/assets/img.png"), aliasDir),
        "image referenced through the aliased document directory must be allowed");
    require(!allowsLocalResource(QUrl::fromLocalFile(aliasDir + "/link.txt"), aliasDir),
        "escape through the aliased document directory must be blocked");
#endif

    return EXIT_SUCCESS;
}
