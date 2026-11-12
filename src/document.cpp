#include "document.h"

#include "highlighter.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMetaEnum>
#include <QQuickTextDocument>
#include <QSaveFile>
#include <QStringDecoder>
#include <QStringTokenizer>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QUrl>

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace {
const QByteArray Bom("\xEF\xBB\xBF");

// Only regular files. Opened without blocking and checked once open, since
// opening a FIFO for reading waits for a writer, and the path can be swapped
// for one between a check and the open.
bool readFile(const QString &path, QByteArray *bytes) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st;
    QFile file;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)
        || !file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return false;
    }
    *bytes = file.readAll();
    return file.error() == QFile::NoError;
}

bool writable(const QString &path) {
    return ::access(QFile::encodeName(path).constData(), W_OK) == 0;
}

QString ownerName(uid_t uid) {
    const passwd *entry = getpwuid(uid);
    return entry ? QString::fromLocal8Bit(entry->pw_name) : QString::number(uid);
}

QString groupName(gid_t gid) {
    const group *entry = getgrgid(gid);
    return entry ? QString::fromLocal8Bit(entry->gr_name) : QString::number(gid);
}
}

Document::Document(QObject *parent) : QObject(parent) {
    // Editors and Omarchy's scripts replace files rather than writing into
    // them, which drops the watch; the debounce collapses the burst and
    // diskChanged() re-arms it.
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(100);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Document::diskChanged);
}

// Bytes to editable text, or not ok when saving the text again couldn't give
// back the same file: invalid UTF-8 (a sequence cut off at the end too), NUL
// bytes, carriage returns that aren't part of CRLF, or U+2029, which
// QTextDocument turns into a line break.
Document::Decoded Document::decode(const QByteArray &input) {
    Decoded result;
    QByteArray bytes = input;
    if (bytes.startsWith(Bom)) {
        result.bom = true;
        bytes.remove(0, Bom.size());
    }
    if (bytes.contains('\0'))
        return result;

    // Stateless, or a sequence cut off at the end of the file is held back as
    // waiting for more bytes rather than reported, and saving drops it.
    // ConvertInitialBom keeps a second BOM as text instead of eating it.
    QStringDecoder decoder(QStringConverter::Utf8,
                           QStringConverter::Flag::Stateless | QStringConverter::Flag::ConvertInitialBom);
    QString text = decoder.decode(bytes);
    if (decoder.hasError() || text.contains(QChar::ParagraphSeparator))
        return result;

    int crlf = 0, lf = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i) == '\r') {
            if (i + 1 >= text.size() || text.at(i + 1) != '\n')
                return result;
            ++crlf;
            ++i;
        } else if (text.at(i) == '\n') {
            ++lf;
        }
    }
    result.crlf = crlf > lf;
    result.mixed = crlf > 0 && lf > 0;
    if (crlf > 0)
        text.replace("\r\n", "\n");
    result.text = text;
    result.ok = true;
    return result;
}

// Block by block rather than toPlainText(), which turns non-breaking spaces
// into ordinary ones. The last block has no line ending after it, so a final
// newline is the empty last line the user can see.
QByteArray Document::encode(const QTextDocument *document, bool bom, bool crlf) {
    QString text;
    const QString ending = crlf ? "\r\n" : "\n";
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        text += block.text();
        if (block.next().isValid())
            text += ending;
    }
    return (bom ? Bom : QByteArray()) + text.toUtf8();
}

// Tabs when most indented lines start with one; otherwise the most common
// step between one line's indentation and the next deeper one.
QString Document::detectIndent(const QString &text) {
    int tabs = 0, spaced = 0, previous = 0;
    QHash<int, int> steps;
    for (const QStringView line : qTokenize(text, u'\n')) {
        if (line.trimmed().isEmpty())
            continue;
        if (line.startsWith(u'\t')) {
            ++tabs;
            continue;
        }
        int width = 0;
        while (width < line.size() && line.at(width) == u' ')
            ++width;
        if (width > 0)
            ++spaced;
        if (width > previous && width - previous <= 8)
            ++steps[width - previous];
        previous = width;
    }
    if (tabs > spaced)
        return "\t";
    int best = 0, bestCount = 0;
    for (auto it = steps.constBegin(); it != steps.constEnd(); ++it)
        if (it.value() > bestCount || (it.value() == bestCount && it.key() < best))
            best = it.key(), bestCount = it.value();
    return best > 0 ? QString(best, ' ') : QString();
}

QByteArray Document::hash(const QByteArray &bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

// What identifies a version of a file without reading it: device, inode, size
// and modification time. Empty when there's no file.
QByteArray Document::stamp(const QString &path) {
    struct stat st;
    if (::stat(QFile::encodeName(path).constData(), &st) != 0)
        return {};
    return QByteArray::number(qulonglong(st.st_dev)) + ':' + QByteArray::number(qulonglong(st.st_ino)) + ':'
         + QByteArray::number(qlonglong(st.st_size)) + ':' + QByteArray::number(qlonglong(st.st_mtim.tv_sec)) + '.'
         + QByteArray::number(qlonglong(st.st_mtim.tv_nsec));
}

QString Document::displayPath() const {
    if (m_path.isEmpty())
        return {};
    const QString home = QDir::homePath();
    return m_path.startsWith(home + "/") ? "~" + m_path.mid(home.size()) : m_path;
}

QString Document::fileName() const {
    return m_path.isEmpty() ? tr("Untitled") : QFileInfo(m_path).fileName();
}

QString Document::commentPrefix() const {
    return Highlighter::commentPrefix(m_language);
}

bool Document::modified() const {
    return m_document && m_document->isModified();
}

QString Document::problem() const {
    return QString::fromLatin1(QMetaEnum::fromType<Problem>().valueToKey(int(m_problem)));
}

void Document::attach(QQuickItem *textEdit) {
    if (!textEdit)
        return;
    m_textEdit = textEdit;
    if (auto *document = textEdit->property("textDocument").value<QQuickTextDocument *>())
        attachDocument(document->textDocument());
}

void Document::attachDocument(QTextDocument *document) {
    if (!document || m_document == document)
        return;
    m_document = document;
    m_highlighter = new Highlighter(m_document, m_theme);
    connect(m_document, &QTextDocument::modificationChanged, this, &Document::modifiedChanged);
}

bool Document::openUrl(const QUrl &url) {
    return url.isLocalFile() && open(url.toLocalFile());
}

bool Document::open(const QString &path) {
    // Absolute, but not canonical: the user opened the link, not its target.
    m_path = QFileInfo(path).absoluteFilePath();
    m_allowLarge = false;
    return load(false);
}

// After a reload refused the file, the editor still holds the text and its
// undo history, so opening it anyway is an undoable reload too.
void Document::openAnyway() {
    m_allowLarge = true;
    load(!m_target.isEmpty());
}

void Document::reload() {
    if (!m_path.isEmpty())
        load(true);
}

bool Document::tooLarge(qint64 size, const QString &text) {
    if (size > LargeFileBytes)
        return true;
    qsizetype start = 0;
    while (start <= text.size()) {
        qsizetype end = text.indexOf('\n', start);
        if (end < 0)
            end = text.size();
        if (end - start > LongLineChars)
            return true;
        start = end + 1;
    }
    return false;
}

// The text goes in with its language already set, so it is colored once, as
// it arrives. Opening goes through the TextEdit (see attach()); a reload is
// one undoable replacement.
void Document::setText(const QString &text, bool undoably) {
    setLanguageFor(m_path, text.left(text.indexOf(QChar(u'\n'))), false);
    if (undoably) {
        QTextCursor cursor(m_document);
        cursor.beginEditBlock();
        cursor.select(QTextCursor::Document);
        cursor.insertText(text);
        cursor.endEditBlock();
        // Only the text property turns on drawing just the visible lines, past
        // Qt's 10,000 characters. Never turned off here, since undo can bring
        // the long text back without it.
        if (m_textEdit && text.size() > 10000)
            m_textEdit->setFlag(QQuickItem::ItemObservesViewport);
    } else if (m_textEdit) {
        m_textEdit->setProperty("text", text);
    } else {
        m_document->setPlainText(text);
    }
    m_document->setModified(false);
}

// Opens m_path into the editor, or with reload, replaces the editor's text
// with what's on disk now. A reload that can't read a usable file leaves the
// editor and everything about the file as they were, and says why.
bool Document::load(bool reload) {
    if (!m_document)
        return false;
    auto refuse = [&](Problem problem) {
        if (!reload) {
            m_editable = false;
            m_locked = false;
            m_target.clear();
            forgetDisk();
            setText({}, false);
            watch();
            emit fileChanged();
        }
        setProblem(problem, displayPath());
        return false;
    };

    const QFileInfo info(m_path);
    if (!info.exists()) {
        if (reload)
            return refuse(Problem::DeletedOnDisk);
        // A new file, created on first save. A dangling link saves to where
        // it points.
        const QString target = info.isSymLink() ? info.symLinkTarget() : m_path;
        if (!QFileInfo(QFileInfo(target).absolutePath()).isDir())
            return refuse(Problem::NotFound);
        m_target = target;
        forgetDisk();
        m_bom = m_crlf = false;
        setText({}, false);
        m_indentUnit = Highlighter::defaultIndent(m_language);
        // A new file in a folder the user can't write opens read-only too.
        m_locked = !writable(QFileInfo(target).absolutePath());
        m_editable = !m_locked;
        setProblem(m_locked ? Problem::NotWritable : Problem::None, displayPath());
        watch();
        emit fileChanged();
        return true;
    }
    if (!info.isFile())
        return refuse(Problem::NotRegular);
    // Asked before reading, so a huge file isn't read only to be put aside.
    if (!m_allowLarge && info.size() > LargeFileBytes)
        return refuse(Problem::TooLarge);

    const QString target = info.canonicalFilePath();
    QByteArray bytes;
    if (!readFile(target, &bytes))
        return refuse(Problem::NotReadable);
    const Decoded decoded = decode(bytes);
    if (!decoded.ok)
        return refuse(Problem::NotText);
    if (!m_allowLarge && tooLarge(bytes.size(), decoded.text))
        return refuse(Problem::TooLarge);

    if (reload)
        emit aboutToReload();
    m_target = target;
    rememberDisk(bytes);
    m_bom = decoded.bom;
    m_crlf = decoded.crlf;
    setText(decoded.text, reload);
    const QString indent = detectIndent(decoded.text);
    m_indentUnit = indent.isEmpty() ? Highlighter::defaultIndent(m_language) : indent;
    // A file the user can't write opens read-only, saying so, rather than
    // letting them type changes the save will refuse.
    m_locked = !writable(m_target);
    m_editable = !m_locked;
    if (m_locked)
        setProblem(Problem::NotWritable, displayPath());
    else
        setProblem(decoded.mixed ? Problem::MixedEndings : Problem::None);
    watch();
    emit fileChanged();
    if (reload)
        emit reloaded();
    return true;
}

void Document::setLanguageFor(const QString &path, const QString &firstLine, bool recolor) {
    m_language = Highlighter::languageFor(path, firstLine);
    if (m_highlighter)
        m_highlighter->setLanguage(m_language, recolor);
}

// The version on disk the user has seen: what Plain last read or wrote.
void Document::rememberDisk(const QByteArray &bytes) {
    m_diskHash = hash(bytes);
    m_diskStamp = stamp(m_target);
}

void Document::forgetDisk() {
    m_diskHash.clear();
    m_diskStamp.clear();
}

// Accepts what's on disk at the target now (or its absence) as seen.
void Document::acknowledgeDisk() {
    QByteArray bytes;
    if (QFileInfo::exists(m_target) && readFile(m_target, &bytes))
        rememberDisk(bytes);
    else
        forgetDisk();
}

// Where a save would go right now: the link's current target, or the path.
QString Document::currentTarget() const {
    const QFileInfo info(m_path);
    if (info.exists())
        return info.canonicalFilePath();
    return info.isSymLink() ? info.symLinkTarget() : m_path;
}

bool Document::save() {
    return checkAndWrite(false);
}

bool Document::overwriteInPlace() {
    return checkAndWrite(true);
}

// Never save over a version of the file the user hasn't seen, however the
// save was asked for. The watcher usually says so earlier; this check is the
// one that counts.
bool Document::checkAndWrite(bool inPlace) {
    if (m_path.isEmpty() || !m_editable || !m_document)
        return false;

    const QString target = currentTarget();
    if (!m_target.isEmpty() && target != m_target) {
        setProblem(Problem::Retargeted, target);
        return false;
    }
    const bool exists = QFileInfo::exists(target);
    if (m_diskHash.isEmpty()) {
        if (exists) {
            setProblem(Problem::Conflict, displayPath());
            return false;
        }
    } else {
        if (!exists) {
            setProblem(Problem::DeletedOnDisk, displayPath());
            return false;
        }
        QByteArray bytes;
        if (!readFile(target, &bytes)) {
            setProblem(Problem::SaveFailed, tr("%1 can't be read back to check it hasn't changed").arg(displayPath()));
            return false;
        }
        if (hash(bytes) != m_diskHash) {
            setProblem(Problem::Conflict, displayPath());
            return false;
        }
    }

    if (!write(target, inPlace)) {
        if (m_problem == Problem::NotWritable) {
            m_locked = true;
            emit fileChanged();
        }
        return false;
    }
    return true;
}

// Accepts the file as it is on disk now (or its absence) as the version the
// user has seen, then saves over it. Also how a retargeted link is accepted.
bool Document::overwrite() {
    m_target = currentTarget();
    acknowledgeDisk();
    setProblem(Problem::None);
    return save();
}

void Document::keepMine() {
    acknowledgeDisk();
    setProblem(Problem::None);
}

void Document::dismissProblem() {
    setProblem(Problem::None);
}

// The portal already asked about replacing an existing file, so Save As
// replaces it without asking again: in place when a rename would lose
// something. The window takes on the new file only once it's written.
bool Document::saveAsUrl(const QUrl &url) {
    // A file Plain refused to open left the editor empty, not holding it, so
    // saving that would replace whatever the user picks (by default the
    // refused file itself) with nothing.
    if (!url.isLocalFile() || !m_document || (!m_editable && !m_locked))
        return false;
    const QString path = QFileInfo(url.toLocalFile()).absoluteFilePath();
    const QFileInfo info(path);
    const QString target = info.exists() ? info.canonicalFilePath() : info.isSymLink() ? info.symLinkTarget() : path;
    const bool inPlace = info.exists() && !unsafeForRename(target).isEmpty();
    if (!write(target, inPlace))
        return false;
    m_path = path;
    m_target = target;
    m_diskStamp = stamp(target);
    m_editable = true;
    m_locked = false;
    setLanguageFor(m_path, m_document->firstBlock().text());
    watch();
    emit fileChanged();
    return true;
}

// Why replacing the file by rename would lose something, or empty when it's
// safe. The rename makes a new inode owned by the user, in the user's group
// (or the folder's, when the folder is setgid), with ordinary permission bits,
// so anything beyond those is a reason to ask.
QString Document::unsafeForRename(const QString &target) const {
    const QByteArray path = QFile::encodeName(target);
    const QByteArray folder = QFile::encodeName(QFileInfo(target).absolutePath());
    struct stat st, folderSt;
    if (::stat(path.constData(), &st) != 0 || ::stat(folder.constData(), &folderSt) != 0)
        return {};
    if (::access(folder.constData(), W_OK) != 0)
        return tr("its folder isn't writable");
    if (st.st_nlink > 1)
        return tr("it has other hard links");
    if (st.st_uid != ::geteuid())
        return tr("it belongs to %1").arg(ownerName(st.st_uid));
    const gid_t newGroup = (folderSt.st_mode & S_ISGID) ? folderSt.st_gid : ::getegid();
    if (st.st_gid != newGroup)
        return tr("it belongs to the %1 group").arg(groupName(st.st_gid));
    if (st.st_mode & (S_ISUID | S_ISGID | S_ISVTX))
        return tr("it has special permission bits");
    if (::listxattr(path.constData(), nullptr, 0) > 0)
        return tr("it has extended attributes or ACLs");
    return {};
}

// Writes the editor's text to target and records it as the version on disk.
// Callers decide whether that's safe; this only refuses what it can't do.
bool Document::write(const QString &target, bool inPlace) {
    if (!m_document || target.isEmpty())
        return false;
    const bool exists = QFileInfo::exists(target);
    if (!writable(exists ? target : QFileInfo(target).absolutePath())) {
        setProblem(Problem::NotWritable, target);
        return false;
    }
    if (!inPlace) {
        const QString reason = unsafeForRename(target);
        if (!reason.isEmpty()) {
            setProblem(Problem::NeedsInPlace, reason);
            return false;
        }
    }

    const QByteArray bytes = encode(m_document, m_bom, m_crlf);
    bool modeRestored = true;
    if (inPlace) {
        struct stat before;
        const bool known = ::stat(QFile::encodeName(target).constData(), &before) == 0;
        QFile file(target);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size()
            || !file.flush() || ::fsync(file.handle()) != 0) {
            setProblem(Problem::SaveFailed, file.errorString());
            return false;
        }
        // The kernel clears setuid and setgid when a non-root user writes a
        // file; put them back, which the owner is allowed to do.
        if (known)
            modeRestored = ::fchmod(file.handle(), before.st_mode & 07777) == 0;
    } else {
        QSaveFile file(target);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            setProblem(Problem::SaveFailed, file.errorString());
            return false;
        }
    }

    m_diskHash = hash(bytes);
    m_diskStamp = stamp(target);
    m_document->setModified(false);
    setProblem(modeRestored ? Problem::None : Problem::ModeLost, displayPath());
    watch();
    return true;
}

void Document::setProblem(Problem problem, const QString &detail) {
    if (problem == m_problem && detail == m_problemDetail)
        return;
    m_problem = problem;
    m_problemDetail = detail;
    emit problemChanged();
}

void Document::watch() {
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    if (m_path.isEmpty())
        return;
    QStringList paths{m_path, QFileInfo(m_path).absolutePath()};
    if (!m_target.isEmpty())
        paths << m_target << QFileInfo(m_target).absolutePath();
    paths.removeDuplicates();
    for (const QString &path : paths)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);
}

// News from the disk. The folder is watched too, since files are replaced
// rather than written, so most news is about other files: the stamp, without
// reading anything, says whether this one changed at all. Then only content
// counts: a touch leaves the hash as it was and changes nothing.
void Document::diskChanged() {
    watch();
    if (m_path.isEmpty() || m_target.isEmpty())
        return;
    // Permissions aren't in the stamp, so a file made writable (chmod, chown)
    // is noticed here, and the editor unlocks without reopening it.
    if (m_locked && writable(QFileInfo::exists(m_target) ? m_target : QFileInfo(m_target).absolutePath())) {
        m_locked = false;
        m_editable = true;
        if (m_problem == Problem::NotWritable)
            setProblem(Problem::None);
        emit fileChanged();
    }
    const QString target = currentTarget();
    const QByteArray now = stamp(target);
    if (target == m_target && now == m_diskStamp)
        return;
    if (now.isEmpty()) {
        if (!m_diskHash.isEmpty())
            setProblem(Problem::DeletedOnDisk, displayPath());
        return;
    }
    // What load() would refuse before reading isn't read here either; a reload
    // refuses it and says why.
    const QFileInfo info(target);
    if (!info.isFile() || (!m_allowLarge && info.size() > LargeFileBytes)) {
        if (!modified() && target == m_target)
            load(true);
        else
            setProblem(Problem::ChangedOnDisk, displayPath());
        return;
    }
    QByteArray bytes;
    if (!readFile(target, &bytes))
        return;
    if (target == m_target && hash(bytes) == m_diskHash) {
        m_diskStamp = now;
        if (m_problem == Problem::DeletedOnDisk || m_problem == Problem::ChangedOnDisk)
            setProblem(Problem::None);
        return;
    }
    if (!modified() && target == m_target)
        load(true);
    else
        setProblem(Problem::ChangedOnDisk, displayPath());
}
