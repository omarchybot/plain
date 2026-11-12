#include <QDir>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextBlock>
#include <QTextLayout>
#include <QTextDocument>
#include <QtTest>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "arguments.h"
#include "document.h"
#include "editing.h"
#include "portalfilepicker.h"
#include "highlighter.h"
#include "theme.h"
#include "translator.h"

class PlainTests : public QObject {
    Q_OBJECT

private slots:
    // Decoding.

    void decodesLfAndCrlf()
    {
        auto lf = Document::decode("a\nb\n");
        QVERIFY(lf.ok);
        QVERIFY(!lf.crlf);
        QCOMPARE(lf.text, QString("a\nb\n"));

        auto crlf = Document::decode("a\r\nb\r\n");
        QVERIFY(crlf.ok);
        QVERIFY(crlf.crlf);
        QVERIFY(!crlf.mixed);
        QCOMPARE(crlf.text, QString("a\nb\n"));

        auto mixed = Document::decode("a\r\nb\r\nc\n");
        QVERIFY(mixed.ok);
        QVERIFY(mixed.crlf);
        QVERIFY(mixed.mixed);
    }

    void refusesWhatCantRoundTrip()
    {
        QVERIFY(!Document::decode(QByteArray("a\0b", 3)).ok);
        QVERIFY(!Document::decode("caf\xE9\n").ok);           // Latin-1, not UTF-8
        QVERIFY(!Document::decode("a\rb\n").ok);              // lone CR
        QVERIFY(!Document::decode("a\xE2\x80\xA9" "b").ok);   // U+2029
        QVERIFY(Document::decode("").ok);
        // A sequence cut off at the end, which a stateful decoder holds back
        // without an error, so saving would drop it.
        QVERIFY(!Document::decode("a\xE2\x82").ok);
    }

    void keepsTheBom()
    {
        auto d = Document::decode("\xEF\xBB\xBFkey = 1\n");
        QVERIFY(d.ok);
        QVERIFY(d.bom);
        QCOMPARE(d.text, QString("key = 1\n"));
    }

    void roundTripsByteForByte_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("lf") << QByteArray("a = 1\nb = 2\n");
        QTest::newRow("no final newline") << QByteArray("a = 1\nb = 2");
        QTest::newRow("blank final lines") << QByteArray("a\n\n\n");
        QTest::newRow("crlf") << QByteArray("[s]\r\nk=v\r\n");
        QTest::newRow("bom") << QByteArray("\xEF\xBB\xBF" "x\n");
        QTest::newRow("double bom") << QByteArray("\xEF\xBB\xBF\xEF\xBB\xBF" "x\n");
        QTest::newRow("nbsp and tabs") << QByteArray("a\xC2\xA0" "b\n\tc\n");
        QTest::newRow("line separator") << QByteArray("a\xE2\x80\xA8" "b\n");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("only newline") << QByteArray("\n");
        QTest::newRow("unicode") << QByteArray("名前 = \"ø\" # 🎉\n");
    }

    void roundTripsByteForByte()
    {
        QFETCH(QByteArray, bytes);
        const auto decoded = Document::decode(bytes);
        QVERIFY(decoded.ok);
        QTextDocument document;
        document.setPlainText(decoded.text);
        QCOMPARE(Document::encode(&document, decoded.bom, decoded.crlf), bytes);
    }

    void asksBeforeOpeningLargeFiles()
    {
        QVERIFY(!Document::tooLarge(1000, "a\nb\n"));
        QVERIFY(Document::tooLarge(Document::LargeFileBytes + 1, "a"));
        QVERIFY(Document::tooLarge(30000, QString(Document::LongLineChars + 1, 'x')));
        QVERIFY(Document::tooLarge(30000, "a\n" + QString(Document::LongLineChars + 1, 'x')));

        QTemporaryDir dir;
        const QString path = dir.filePath("long.json");
        write(path, QByteArray(Document::LongLineChars + 10, 'x'));
        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(!doc.open(path));
        QCOMPARE(doc.problem(), QString("TooLarge"));
        QVERIFY(text.isEmpty());
        doc.openAnyway();
        QCOMPARE(doc.problem(), QString("None"));
        QCOMPARE(text.characterCount() - 1, Document::LongLineChars + 10);
        QVERIFY(doc.editable());
    }

    void detectsIndentation()
    {
        QCOMPARE(Document::detectIndent("a {\n    b {\n        c\n    }\n}\n"), QString("    "));
        QCOMPARE(Document::detectIndent("a:\n  b:\n    c: 1\n  d: 2\n"), QString("  "));
        QCOMPARE(Document::detectIndent("a {\n\tb\n\tc\n}\n"), QString("\t"));
        QCOMPARE(Document::detectIndent("flat\nfile\n"), QString());
    }

    // Opening.

    void opensAndRefuses()
    {
        QTemporaryDir dir;
        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);

        QVERIFY(!doc.open(dir.filePath("missing/new.conf")));
        QCOMPARE(doc.problem(), QString("NotFound"));

        QVERIFY(!doc.open(dir.path()));
        QCOMPARE(doc.problem(), QString("NotRegular"));

        QVERIFY(::mkfifo(QFile::encodeName(dir.filePath("fifo")).constData(), 0600) == 0);
        QVERIFY(!doc.open(dir.filePath("fifo")));
        QCOMPARE(doc.problem(), QString("NotRegular"));

        write(dir.filePath("binary"), QByteArray("\x7F" "ELF\0\0", 6));
        QVERIFY(!doc.open(dir.filePath("binary")));
        QCOMPARE(doc.problem(), QString("NotText"));
        QVERIFY(!doc.editable());

        write(dir.filePath("secret"), "x\n");
        QFile::setPermissions(dir.filePath("secret"), QFile::WriteOwner);
        if (::geteuid() != 0) {
            QVERIFY(!doc.open(dir.filePath("secret")));
            QCOMPARE(doc.problem(), QString("NotReadable"));
        }

        write(dir.filePath("ok.toml"), "a = 1\r\nb = 2\n");
        QVERIFY(doc.open(dir.filePath("ok.toml")));
        QCOMPARE(doc.problem(), QString("MixedEndings"));
        QCOMPARE(doc.language(), QString("toml"));
        QVERIFY(!doc.modified());
    }

    // Saving.

    void savesAtomicallyKeepingTheMode()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.conf");
        write(path, "a = 1\n");
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup);
        const ino_t before = inode(path);

        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(path));
        append(&text, "b = 2\n");
        QVERIFY(doc.modified());
        QVERIFY(doc.save());
        QVERIFY(!doc.modified());
        QCOMPARE(read(path), QByteArray("a = 1\nb = 2\n"));
        QCOMPARE(mode(path), 0640);
        QVERIFY(inode(path) != before); // replaced, not rewritten
    }

    void savesThroughSymlinks()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("store");
        const QString target = dir.filePath("store/real.lua"), link = dir.filePath("link.lua");
        write(target, "x = 1\n");
        QVERIFY(QFile::link(target, link));

        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(link));
        QCOMPARE(doc.path(), link);
        append(&text, "y = 2\n");
        QVERIFY(doc.save());
        QVERIFY(QFileInfo(link).isSymLink());
        QCOMPARE(read(target), QByteArray("x = 1\ny = 2\n"));
    }

    void asksWhenALinkIsRetargeted()
    {
        QTemporaryDir dir;
        const QString one = dir.filePath("one"), two = dir.filePath("two"), link = dir.filePath("link");
        write(one, "1\n");
        write(two, "2\n");
        QVERIFY(QFile::link(one, link));

        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(link));
        QFile::remove(link);
        QVERIFY(QFile::link(two, link));
        append(&text, "more\n");
        QVERIFY(!doc.save());
        QCOMPARE(doc.problem(), QString("Retargeted"));
        QCOMPARE(read(two), QByteArray("2\n"));
        QCOMPARE(read(one), QByteArray("1\n"));
    }

    void neverSavesOverAnUnseenVersion()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.conf");
        write(path, "a = 1\n");

        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(path));
        // Same size, different content: mtime and size wouldn't catch it.
        write(path, "a = 2\n");
        append(&text, "mine\n");
        QVERIFY(!doc.save());
        QCOMPARE(doc.problem(), QString("Conflict"));
        QCOMPARE(read(path), QByteArray("a = 2\n"));

        QVERIFY(doc.overwrite());
        QCOMPARE(read(path), QByteArray("a = 1\nmine\n"));
    }

    void asksBeforeRecreatingADeletedFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("gone.conf");
        write(path, "a\n");
        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(path));
        QFile::remove(path);
        append(&text, "b\n");
        QVERIFY(!doc.save());
        QCOMPARE(doc.problem(), QString("DeletedOnDisk"));
        QVERIFY(!QFile::exists(path));
        QVERIFY(doc.overwrite());
        QCOMPARE(read(path), QByteArray("a\nb\n"));
    }

    void asksBeforeWritingInPlace()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("linked.conf"), other = dir.filePath("other-name.conf");
        write(path, "a\n");
        QVERIFY(::link(QFile::encodeName(path).constData(), QFile::encodeName(other).constData()) == 0);

        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(path));
        append(&text, "b\n");
        QVERIFY(!doc.save());
        QCOMPARE(doc.problem(), QString("NeedsInPlace"));
        QCOMPARE(read(path), QByteArray("a\n"));

        QVERIFY(doc.overwriteInPlace());
        QCOMPARE(read(other), QByteArray("a\nb\n")); // the hard link survived
        QVERIFY(!doc.modified());
    }

    // Overwriting in place, after the question, still never saves over a
    // version of the file the user hasn't seen.
    void overwritingInPlaceChecksTheDiskFirst()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("linked.conf"), other = dir.filePath("other.conf");
        write(path, "a\n");
        QVERIFY(::link(QFile::encodeName(path).constData(), QFile::encodeName(other).constData()) == 0);
        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(path));
        append(&text, "mine\n");
        QVERIFY(!doc.save());
        QCOMPARE(doc.problem(), QString("NeedsInPlace"));

        write(other, "theirs\n");
        QVERIFY(!doc.overwriteInPlace());
        QCOMPARE(doc.problem(), QString("Conflict"));
        QCOMPARE(read(path), QByteArray("theirs\n"));
    }

    void stopsWhenTheDiskCantBeChecked()
    {
        if (::geteuid() == 0)
            QSKIP("root can read anything");
        QTemporaryDir dir;
        const QString path = dir.filePath("a.conf");
        write(path, "a\n");
        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(path));
        QFile::setPermissions(path, QFile::WriteOwner);
        append(&text, "b\n");
        QVERIFY(!doc.save());
        QCOMPARE(doc.problem(), QString("SaveFailed"));
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
        QCOMPARE(read(path), QByteArray("a\n"));
    }

    // A reload that can't use what's on disk keeps the editor as it was.
    void failedReloadKeepsTheEdits()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.conf");
        write(path, "a\n");
        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(path));
        append(&text, "mine\n");
        write(path, QByteArray("\xFF\xFE", 2));
        doc.reload();
        QCOMPARE(doc.problem(), QString("NotText"));
        QCOMPARE(text.toPlainText(), QString("a\nmine\n"));
        QVERIFY(doc.modified());
        QVERIFY(doc.editable());

        QFile::remove(path);
        doc.reload();
        QCOMPARE(doc.problem(), QString("DeletedOnDisk"));
        QCOMPARE(text.toPlainText(), QString("a\nmine\n"));
    }

    void failedSaveAsKeepsTheFile()
    {
        if (::geteuid() == 0)
            QSKIP("root can write anything");
        QTemporaryDir dir;
        const QString path = dir.filePath("a.conf");
        write(path, "a\n");
        QDir(dir.path()).mkdir("sealed");
        QFile::setPermissions(dir.filePath("sealed"), QFile::ReadOwner | QFile::ExeOwner);
        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(path));
        QVERIFY(!doc.saveAsUrl(QUrl::fromLocalFile(dir.filePath("sealed/b.conf"))));
        QCOMPARE(doc.path(), path);
        QFile::setPermissions(dir.filePath("sealed"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    // A refused file leaves the editor empty; Save As over it, which the
    // dialog suggests, would truncate it.
    void saveAsNeedsAFileToSave()
    {
        QTemporaryDir dir;
        const QString binary = dir.filePath("binary");
        write(binary, QByteArray("\x7F" "ELF\0\0", 6));
        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(!doc.open(binary));
        QVERIFY(!doc.saveAsUrl(QUrl::fromLocalFile(binary)));
        QCOMPARE(read(binary), QByteArray("\x7F" "ELF\0\0", 6));

        if (::geteuid() == 0)
            return;
        // A file the user can't write is loaded, and Save As is its way out.
        const QString locked = dir.filePath("locked.conf"), copy = dir.filePath("copy.conf");
        write(locked, "a = 1\n");
        QFile::setPermissions(locked, QFile::ReadOwner);
        QVERIFY(doc.open(locked));
        QVERIFY(doc.locked());
        QVERIFY(doc.saveAsUrl(QUrl::fromLocalFile(copy)));
        QCOMPARE(read(copy), QByteArray("a = 1\n"));
    }

    // A rename would hand the file to the user's own group.
    void asksBeforeChangingTheGroup()
    {
        gid_t groups[64];
        const int count = ::getgroups(64, groups);
        gid_t other = ::getegid();
        for (int i = 0; i < count; ++i)
            if (groups[i] != ::getegid())
                other = groups[i];
        if (other == ::getegid())
            QSKIP("needs a second group to give the file");

        QTemporaryDir dir;
        const QString path = dir.filePath("shared.conf");
        write(path, "a\n");
        QVERIFY(::chown(QFile::encodeName(path).constData(), -1, other) == 0);
        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(path));
        append(&text, "b\n");
        QVERIFY(!doc.save());
        QCOMPARE(doc.problem(), QString("NeedsInPlace"));
        QVERIFY(doc.overwriteInPlace());
        struct stat st;
        QVERIFY(::stat(QFile::encodeName(path).constData(), &st) == 0);
        QCOMPARE(st.st_gid, other);
    }

    void asksBeforeDroppingSpecialBits()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("tool.sh");
        write(path, "echo hi\n");
        QVERIFY(::chmod(QFile::encodeName(path).constData(), 02755) == 0);
        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(path));
        append(&text, "echo bye\n");
        QVERIFY(!doc.save());
        QCOMPARE(doc.problem(), QString("NeedsInPlace"));
        QVERIFY(doc.overwriteInPlace());
        QCOMPARE(mode(path), 02755);
    }

    void reportsFilesItCantWrite()
    {
        if (::geteuid() == 0)
            QSKIP("root can write anything");
        QTemporaryDir dir;
        const QString path = dir.filePath("ro.conf");
        write(path, "a\n");
        QFile::setPermissions(path, QFile::ReadOwner);
        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(path));
        QCOMPARE(doc.problem(), QString("NotWritable"));
        QVERIFY(!doc.editable());
        QVERIFY(doc.locked());
        QVERIFY(!doc.save());
        QCOMPARE(read(path), QByteArray("a\n"));

        // Writable at open, read-only by save time: the save says so.
        const QString later = dir.filePath("later.conf");
        write(later, "a\n");
        QVERIFY(doc.open(later));
        QVERIFY(doc.editable());
        QVERIFY(!doc.locked());
        QFile::setPermissions(later, QFile::ReadOwner);
        append(&text, "b\n");
        QVERIFY(!doc.save());
        QCOMPARE(doc.problem(), QString("NotWritable"));

        // A new file in a folder the user can't write opens locked, too.
        QDir(dir.path()).mkdir("sealed");
        QFile::setPermissions(dir.filePath("sealed"), QFile::ReadOwner | QFile::ExeOwner);
        QVERIFY(doc.open(dir.filePath("sealed/new.conf")));
        QVERIFY(doc.locked());
        QVERIFY(!doc.editable());
        QCOMPARE(doc.problem(), QString("NotWritable"));
        QFile::setPermissions(dir.filePath("sealed"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    // Qt Quick draws only the visible lines of a long text, but turns that on
    // only when the text arrives through the TextEdit itself. Loaded behind
    // its back, every keystroke in a 10,000-line file redrew it all.
    void longFilesDrawOnlyWhatsVisible()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nTextEdit { textFormat: TextEdit.PlainText }", {});
        std::unique_ptr<QQuickItem> edit(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY(edit);

        QTemporaryDir dir;
        QByteArray lines;
        for (int i = 0; i < 2000; ++i)
            lines += "key_" + QByteArray::number(i) + " = value\n";
        write(dir.filePath("long.conf"), lines);
        Document doc;
        doc.attach(edit.get());
        QVERIFY(doc.open(dir.filePath("long.conf")));
        QVERIFY(edit->flags() & QQuickItem::ItemObservesViewport);

        // A reload replaces the text undoably, past the text property.
        write(dir.filePath("grows.conf"), "a = 1\n");
        QVERIFY(doc.open(dir.filePath("grows.conf")));
        QVERIFY(!(edit->flags() & QQuickItem::ItemObservesViewport));
        write(dir.filePath("grows.conf"), lines);
        doc.reload();
        QCOMPARE(edit->property("text").toString().size(), lines.size());
        QVERIFY(edit->flags() & QQuickItem::ItemObservesViewport);

        // Shrinking it keeps the flag, since undo brings the long text back.
        write(dir.filePath("grows.conf"), "a = 1\n");
        doc.reload();
        undo(edit.get());
        QCOMPARE(edit->property("text").toString().size(), lines.size());
        QVERIFY(edit->flags() & QQuickItem::ItemObservesViewport);
    }

    void createsNewFiles()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("new.yaml");
        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(path));
        QCOMPARE(doc.problem(), QString("None"));
        QCOMPARE(doc.indentUnit(), QString("  "));
        append(&text, "a: 1");
        QVERIFY(doc.save());
        QCOMPARE(read(path), QByteArray("a: 1"));
    }

    void followsTheDisk()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("live.conf");
        write(path, "a = 1\n");
        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(path));

        // No unsaved changes: reloads quietly, and the reload can be undone.
        replace(path, "a = 2\n");
        QTRY_COMPARE(text.toPlainText(), QString("a = 2\n"));
        QVERIFY(!doc.modified());
        QCOMPARE(doc.problem(), QString("None"));

        // Unsaved changes: asks.
        append(&text, "mine\n");
        replace(path, "a = 3\n");
        QTRY_COMPARE(doc.problem(), QString("ChangedOnDisk"));
        QVERIFY(text.toPlainText().contains("mine"));
        doc.keepMine();
        QVERIFY(doc.save());
        QCOMPARE(read(path), QByteArray("a = 2\nmine\n"));

        // Plain's own save doesn't count as news.
        QTest::qWait(250);
        QCOMPARE(doc.problem(), QString("None"));
    }

    // A file that grows past the limit while open is put aside, and opening
    // it anyway replaces the text as one undoable step, like any reload.
    void openAnywayAfterAReloadCanBeUndone()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("grows.conf");
        write(path, "a = 1\n");
        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(path));

        replace(path, QByteArray(Document::LargeFileBytes + 1, 'x'));
        QTRY_COMPARE(doc.problem(), QString("TooLarge"));
        QCOMPARE(text.toPlainText(), QString("a = 1\n"));
        append(&text, "mine\n");
        doc.openAnyway();
        QCOMPARE(text.characterCount() - 1, Document::LargeFileBytes + 1);
        text.undo();
        QCOMPARE(text.toPlainText(), QString("a = 1\nmine\n"));
    }

    // Reading a FIFO waits for a writer, which would freeze the window.
    void neverReadsAFifoThatReplacedTheFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.conf");
        write(path, "a = 1\n");
        QTextDocument text;
        Document doc;
        doc.attachDocument(&text);
        QVERIFY(doc.open(path));

        const QString fifo = dir.filePath("fifo");
        QVERIFY(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
        QVERIFY(::rename(QFile::encodeName(fifo).constData(), QFile::encodeName(path).constData()) == 0);
        QTRY_COMPARE(doc.problem(), QString("NotRegular"));
        QCOMPARE(text.toPlainText(), QString("a = 1\n"));
        QVERIFY(!doc.save());
    }

    // Arguments.

    void parsesArguments()
    {
        auto a = Arguments::parse({"plain", "+12", "--", "-weird name"});
        QCOMPARE(a.line, 12);
        QCOMPARE(a.path, QString("-weird name"));
        QVERIFY(a.error.isEmpty());

        QCOMPARE(Arguments::parse({"plain", "--", "+5"}).path, QString("+5"));
        QCOMPARE(Arguments::parse({"plain", "file:///etc/hosts"}).path, QString("/etc/hosts"));
        QCOMPARE(Arguments::parse({"plain", "a:12"}).path, QString("a:12"));
        QVERIFY(!Arguments::parse({"plain", "https://example.com/x"}).error.isEmpty());
        QVERIFY(!Arguments::parse({"plain", "a", "b"}).error.isEmpty());
        QVERIFY(!Arguments::parse({"plain", "--wait", "a"}).error.isEmpty());
        QVERIFY(Arguments::parse({"plain"}).path.isEmpty());
    }

    // Languages and highlighting.

    void picksLanguages()
    {
        QCOMPARE(Highlighter::languageFor("/h/.config/hypr/bindings.lua", ""), QString("lua"));
        QCOMPARE(Highlighter::languageFor("/h/.config/hypr/hypridle.conf", ""), QString("conf"));
        QCOMPARE(Highlighter::languageFor("/h/.config/waybar/config.jsonc", ""), QString("jsonc"));
        QCOMPARE(Highlighter::languageFor("/h/.config/waybar/style.css", ""), QString("css"));
        QCOMPARE(Highlighter::languageFor("/h/.bashrc", ""), QString("shell"));
        QCOMPARE(Highlighter::languageFor("/h/PKGBUILD", ""), QString("shell"));
        QCOMPARE(Highlighter::languageFor("/h/bin/thing", "#!/bin/bash"), QString("shell"));
        QCOMPARE(Highlighter::languageFor("/h/notes", "hello"), QString("text"));
        QCOMPARE(Highlighter::commentPrefix("jsonc"), QString("//"));
        QCOMPARE(Highlighter::commentPrefix("text"), QString());
    }

    void highlightsLua()
    {
        QCOMPARE(Highlighter::tokens("lua", {"local x = \"a -- b\" -- c"}),
                 QString("wwwww.k...ssssssss.cccc"));
        // A long comment holding ]] ends only at its own level.
        QCOMPARE(Highlighter::tokens("lua", {"--[==[ a ]] b", "]==] x = 1"}),
                 QString("ccccccccccccc\ncccc.k...n"));
        QCOMPARE(Highlighter::tokens("lua", {"s = 'it\\'s' == nil"}),
                 QString("k...sssssss....xxx"));
        QCOMPARE(Highlighter::tokens("conf", {"; ini comment", "key = a;b"}),
                 QString("ccccccccccccc\nkkk......"));
    }

    void highlightsShell()
    {
        QCOMPARE(Highlighter::tokens("shell", {"echo ${#arr} # note"}),
                 QString(".....xxxxxxx.cccccc"));
        QCOMPARE(Highlighter::tokens("shell", {"export EDITOR='a#b'"}),
                 QString("wwwwww.kkkkkk.sssss"));
    }

    void leavesVeryLongLinesUncolored()
    {
        // A document reports its changes only once it has a layout, which the
        // TextEdit's always does.
        QTextDocument document;
        document.documentLayout();
        Highlighter highlighter(&document, nullptr);
        highlighter.setLanguage("jsonc");
        const QString longLine = "\"k\": " + QString(Highlighter::LongLine, '1');
        document.setPlainText(longLine + "\n\"k\": 1");
        QVERIFY(document.firstBlock().layout()->formats().isEmpty());
        QVERIFY(!document.lastBlock().layout()->formats().isEmpty());
    }

    void highlightsDataFormats()
    {
        QCOMPARE(Highlighter::tokens("jsonc", {"{\"a\": \"b\", // c"}),
                 QString(".kkk..sss..cccc"));
        QCOMPARE(Highlighter::tokens("toml", {"[server]", "msg = \"\"\"", "# not a comment", "\"\"\""}),
                 QString("wwwwwwww\nkkk...sss\nsssssssssssssss\nsss"));
        QCOMPARE(Highlighter::tokens("yaml", {"- name: x#y # z", "  on: true"}),
                 QString("..kkkk......ccc\n..xx..xxxx"));
        QCOMPARE(Highlighter::tokens("conf", {"general {", "    gaps_in = 5 # px", "}"}),
                 QString("wwwwwww..\n....kkkkkkk...n.cccc\n."));
        QCOMPARE(Highlighter::tokens("css", {"@define-color fg #ffcead; /* c */"}),
                 QString("wwwwwwwwwwwww....nnnnnnn..ccccccc"));
    }

    // Edits, through a real TextEdit and its own undo stack.

    void editsAreSingleUndoSteps()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nTextEdit { textFormat: TextEdit.PlainText }", {});
        std::unique_ptr<QQuickItem> edit(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY(edit);

        QTemporaryDir dir;
        const QString path = dir.filePath("t.lua");
        write(path, "a = 1\n    b = 2\n");
        Document doc;
        doc.attach(edit.get());
        Editing editing(&doc);
        QVERIFY(doc.open(path));
        auto text = [&] { return edit->property("text").toString(); };
        auto undo = [&] { QMetaObject::invokeMethod(edit.get(), "undo"); };
        auto redo = [&] { QMetaObject::invokeMethod(edit.get(), "redo"); };
        QVERIFY(!edit->property("canUndo").toBool());

        editing.toggleComment(0, 12);
        // Lined up at the shallowest indentation of the lines, as VSCode does.
        QCOMPARE(text(), QString("-- a = 1\n--     b = 2\n"));
        undo();
        QCOMPARE(text(), QString("a = 1\n    b = 2\n"));
        QVERIFY(!doc.modified());
        redo();
        QCOMPARE(text(), QString("-- a = 1\n--     b = 2\n"));
        editing.toggleComment(0, 20);
        QCOMPARE(text(), QString("a = 1\n    b = 2\n"));

        editing.indent(0, 10);
        QCOMPARE(text(), QString("    a = 1\n        b = 2\n"));
        editing.outdent(0, 20);
        editing.outdent(0, 20);
        QCOMPARE(text(), QString("a = 1\nb = 2\n"));
        undo();
        QCOMPARE(text(), QString("a = 1\n    b = 2\n"));

        // Enter keeps the indentation.
        const int end = text().indexOf("2") + 1;
        QCOMPARE(editing.newline(end, end), end + 5);
        QCOMPARE(text(), QString("a = 1\n    b = 2\n    \n"));
        undo();

        QCOMPARE(editing.matchCount("= ", false), 2);
        QCOMPARE(editing.replaceAll("= ", ":= ", true), 2);
        QCOMPARE(text(), QString("a := 1\n    b := 2\n"));
        undo();
        QCOMPARE(text(), QString("a = 1\n    b = 2\n"));
        QCOMPARE(editing.find("=", 3, false, false), QVariantList({12, 13}));
        QCOMPARE(editing.find("=", 13, false, false), QVariantList({2, 3})); // wraps around
        QCOMPARE(editing.lineOf(7), 2);
        QCOMPARE(editing.positionOfLine(2), 6);
    }

    // QML calls these from Ctrl+O and Ctrl+S; plain C++ methods it can't.
    void filePickerIsCallableFromQml()
    {
        QVERIFY(PortalFilePicker::staticMetaObject.indexOfMethod("openFile(QString)") >= 0);
        QVERIFY(PortalFilePicker::staticMetaObject.indexOfMethod("saveFile(QString)") >= 0);
    }

    // Theme and translator, from the template.

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#FFD60A"));
        QCOMPARE(theme.textSize(), 12);
    }

    // The display panel's Text size: the user's override, else the theme's.
    void themeFollowsTheDesktopTextSize()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/shell.toml",
              "[bar]\nbase-size = 99\n\n[font]\n# base-size = 20\nbase-size = 13\n");
        Theme theme(dir.path());
        QCOMPARE(theme.textSize(), 13);

        write(dir.path() + "/shell.toml", "[font]\nbase-size = 16\n");
        QTRY_COMPARE(theme.textSize(), 16);
        QFile::remove(dir.path() + "/shell.toml");
        QTRY_COMPARE(theme.textSize(), 13);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

private:
    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(contents);
    }

    // Replaces the file the way other editors do, so the watch has to re-arm.
    static void replace(const QString &path, const QByteArray &contents)
    {
        write(path + ".tmp", contents);
        QVERIFY(::rename(QFile::encodeName(path + ".tmp").constData(), QFile::encodeName(path).constData()) == 0);
    }

    static QByteArray read(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<unreadable>");
    }

    static void undo(QQuickItem *edit)
    {
        edit->property("textDocument").value<QQuickTextDocument *>()->textDocument()->undo();
    }

    static void append(QTextDocument *document, const QString &text)
    {
        QTextCursor cursor(document);
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(text);
    }

    static int mode(const QString &path)
    {
        struct stat st;
        return ::stat(QFile::encodeName(path).constData(), &st) == 0 ? int(st.st_mode & 07777) : -1;
    }

    static ino_t inode(const QString &path)
    {
        struct stat st;
        return ::stat(QFile::encodeName(path).constData(), &st) == 0 ? st.st_ino : 0;
    }
};

QTEST_MAIN(PlainTests)
#include "plain_tests.moc"
