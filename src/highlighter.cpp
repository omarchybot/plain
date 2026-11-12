#include "highlighter.h"

#include "theme.h"

#include <QFileInfo>
#include <QSet>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <cctype>
#include <functional>

namespace {
// What a language has. Everything the scanner needs to know, nothing more.
struct Spec {
    QStringList lineComments;       // "#", "--", "//", ";"
    bool commentNeedsSpace = false; // '#' only at the start or after a space (shell, YAML)
    QStringList lineStartComments;  // only as the first thing on a line (INI's ;)
    QString blockOpen, blockClose;  // "/*", "*/"
    bool luaLongBrackets = false;   // --[[ ]], [==[ ]==]
    bool tripleQuotes = false;      // TOML """ and '''
    QString quotes = "\"'";
    bool singleQuoteRaw = false;    // no escapes in '...' (shell, TOML, YAML)
    bool keysBeforeEquals = false;  // key = value
    bool keysBeforeColon = false;   // key: value
    bool stringKeys = false;        // "key": value (JSON)
    bool sections = false;          // [section], name {
    bool variables = false;         // $NAME, ${NAME}
    bool atRules = false;           // @define-color (CSS)
    bool hexColors = false;         // #rrggbb as a number (CSS)
    QSet<QString> keywords;
    QSet<QString> constants;
};

const Spec &spec(const QString &language) {
    static const QHash<QString, Spec> specs = [] {
        QHash<QString, Spec> s;
        Spec lua;
        lua.lineComments = {"--"};
        lua.luaLongBrackets = true;
        lua.keysBeforeEquals = true;
        lua.keywords = {"and", "break", "do", "else", "elseif", "end", "for", "function", "goto", "if",
                        "in", "local", "not", "or", "repeat", "return", "then", "until", "while"};
        lua.constants = {"true", "false", "nil"};
        s["lua"] = lua;

        Spec conf;
        conf.lineComments = {"#"};
        conf.commentNeedsSpace = true;
        conf.lineStartComments = {";"};
        conf.keysBeforeEquals = true;
        conf.sections = true;
        conf.variables = true;
        conf.constants = {"true", "false", "yes", "no", "on", "off"};
        s["conf"] = conf;

        Spec toml;
        toml.lineComments = {"#"};
        toml.tripleQuotes = true;
        toml.singleQuoteRaw = true;
        toml.keysBeforeEquals = true;
        toml.sections = true;
        toml.constants = {"true", "false", "inf", "nan"};
        s["toml"] = toml;

        Spec json;
        json.lineComments = {"//"};
        json.blockOpen = "/*";
        json.blockClose = "*/";
        json.quotes = "\"";
        json.stringKeys = true;
        json.constants = {"true", "false", "null"};
        s["json"] = json;
        s["jsonc"] = json;

        Spec yaml;
        yaml.lineComments = {"#"};
        yaml.commentNeedsSpace = true;
        yaml.singleQuoteRaw = true;
        yaml.keysBeforeColon = true;
        yaml.constants = {"true", "false", "yes", "no", "on", "off", "null", "True", "False", "Null", "~"};
        s["yaml"] = yaml;

        Spec shell;
        shell.lineComments = {"#"};
        shell.commentNeedsSpace = true;
        shell.singleQuoteRaw = true;
        shell.variables = true;
        shell.keysBeforeEquals = true;
        shell.keywords = {"if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done", "case",
                          "esac", "in", "function", "return", "export", "local", "readonly", "source", "exit"};
        shell.constants = {"true", "false"};
        s["shell"] = shell;

        Spec css;
        css.blockOpen = "/*";
        css.blockClose = "*/";
        css.keysBeforeColon = true;
        css.atRules = true;
        css.hexColors = true;
        s["css"] = css;
        return s;
    }();
    static const Spec none;
    const auto it = specs.constFind(language);
    return it != specs.constEnd() ? *it : none;
}

// Block states carried from line to line: inside a block comment, a TOML
// multi-line string, or a Lua long comment or string of a given level.
enum State { Normal = 0, InBlockComment = 1, InTripleDouble = 2, InTripleSingle = 3,
             LuaLongComment = 100, LuaLongString = 200 };

bool isIdentStart(QChar c) { return c.isLetter() || c == '_'; }
bool isIdent(QChar c) { return c.isLetterOrNumber() || c == '_' || c == '-' || c == '.'; }

// Lua's [[, [=[, [==[ ...: the level, or -1 when position isn't one.
int longBracketLevel(const QString &text, qsizetype i) {
    if (i >= text.size() || text.at(i) != '[')
        return -1;
    qsizetype j = i + 1;
    while (j < text.size() && text.at(j) == '=')
        ++j;
    return j < text.size() && text.at(j) == '[' ? int(j - i - 1) : -1;
}

using Emit = std::function<void(qsizetype start, qsizetype length, Highlighter::Token token)>;

// Colors one line. Returns the state the next line starts in.
int scan(const Spec &s, const QString &text, int state, const Emit &emitToken) {
    const qsizetype n = text.size();
    qsizetype i = 0;

    // Finishes a construct that started on an earlier line.
    auto closeLong = [&](const QString &close, Highlighter::Token token) -> bool {
        const qsizetype end = text.indexOf(close, i);
        if (end < 0) {
            emitToken(i, n - i, token);
            i = n;
            return false;
        }
        emitToken(i, end + close.size() - i, token);
        i = end + close.size();
        return true;
    };
    auto luaClose = [](int level) { return "]" + QString(level, '=') + "]"; };

    if (state == InBlockComment && !closeLong(s.blockClose, Highlighter::Comment))
        return InBlockComment;
    if (state == InTripleDouble && !closeLong("\"\"\"", Highlighter::String))
        return InTripleDouble;
    if (state == InTripleSingle && !closeLong("'''", Highlighter::String))
        return InTripleSingle;
    if (state >= LuaLongComment && state < LuaLongString
        && !closeLong(luaClose(state - LuaLongComment), Highlighter::Comment))
        return state;
    if (state >= LuaLongString && !closeLong(luaClose(state - LuaLongString), Highlighter::String))
        return state;

    // Views, not copies: the scan looks at every character, and copying the
    // rest of the line at each one made a long line quadratic.
    const QStringView line(text);
    auto nextNonSpace = [&](qsizetype from) {
        while (from < n && text.at(from).isSpace())
            ++from;
        return from;
    };
    // Where the line's opening run of spaces (and, for keys, list dashes and
    // table openers) ends, worked out once per line.
    const qsizetype indent = nextNonSpace(0);
    qsizetype keyStart = 0;
    while (keyStart < n && (text.at(keyStart).isSpace() || QStringView(u"-{,").contains(text.at(keyStart))))
        ++keyStart;

    // A [section] line.
    if (s.sections && i == 0) {
        const QStringView trimmed = line.trimmed();
        if (trimmed.startsWith(u'[') && trimmed.endsWith(u']')) {
            emitToken(indent, trimmed.size(), Highlighter::Keyword);
            return Normal;
        }
    }

    while (i < n) {
        const QChar c = text.at(i);
        if (c.isSpace()) {
            ++i;
            continue;
        }
        const QStringView rest = line.sliced(i);

        // Comments.
        if (s.luaLongBrackets && rest.startsWith(u"--")) {
            const int level = longBracketLevel(text, i + 2);
            if (level >= 0) {
                const qsizetype start = i;
                i += level + 4;
                const QString close = luaClose(level);
                const qsizetype end = text.indexOf(close, i);
                if (end < 0) {
                    emitToken(start, n - start, Highlighter::Comment);
                    return LuaLongComment + level;
                }
                emitToken(start, end + close.size() - start, Highlighter::Comment);
                i = end + close.size();
                continue;
            }
        }
        bool comment = false;
        for (const QString &marker : s.lineComments)
            if (rest.startsWith(marker) && (!s.commentNeedsSpace || i == 0 || text.at(i - 1).isSpace()))
                comment = true;
        for (const QString &marker : s.lineStartComments)
            if (i == indent && rest.startsWith(marker))
                comment = true;
        if (comment) {
            emitToken(i, n - i, Highlighter::Comment);
            return Normal;
        }
        if (!s.blockOpen.isEmpty() && rest.startsWith(s.blockOpen)) {
            const qsizetype end = text.indexOf(s.blockClose, i + s.blockOpen.size());
            if (end < 0) {
                emitToken(i, n - i, Highlighter::Comment);
                return InBlockComment;
            }
            emitToken(i, end + s.blockClose.size() - i, Highlighter::Comment);
            i = end + s.blockClose.size();
            continue;
        }

        // Strings.
        if (s.luaLongBrackets) {
            const int level = longBracketLevel(text, i);
            if (level >= 0) {
                const QString close = luaClose(level);
                const qsizetype end = text.indexOf(close, i + level + 2);
                if (end < 0) {
                    emitToken(i, n - i, Highlighter::String);
                    return LuaLongString + level;
                }
                emitToken(i, end + close.size() - i, Highlighter::String);
                i = end + close.size();
                continue;
            }
        }
        if (s.tripleQuotes && (rest.startsWith(u"\"\"\"") || rest.startsWith(u"'''"))) {
            const QString quote = rest.first(3).toString();
            const qsizetype end = text.indexOf(quote, i + 3);
            if (end < 0) {
                emitToken(i, n - i, Highlighter::String);
                return quote == "'''" ? InTripleSingle : InTripleDouble;
            }
            emitToken(i, end + 3 - i, Highlighter::String);
            i = end + 3;
            continue;
        }
        if (s.quotes.contains(c)) {
            const bool raw = c == '\'' && s.singleQuoteRaw;
            qsizetype j = i + 1;
            while (j < n && text.at(j) != c)
                j += (!raw && text.at(j) == '\\') ? 2 : 1;
            const qsizetype end = qMin(j + 1, n);
            const bool key = s.stringKeys && nextNonSpace(end) < n && text.at(nextNonSpace(end)) == ':';
            emitToken(i, end - i, key ? Highlighter::Key : Highlighter::String);
            i = end;
            continue;
        }

        // Variables, at-rules and hex colors.
        if (s.variables && c == '$' && i + 1 < n) {
            qsizetype j = i + 1;
            if (text.at(j) == '{') {
                const qsizetype close = text.indexOf('}', j);
                j = close < 0 ? n : close + 1;
            } else {
                while (j < n && (text.at(j).isLetterOrNumber() || text.at(j) == '_'))
                    ++j;
            }
            if (j > i + 1) {
                emitToken(i, j - i, Highlighter::Constant);
                i = j;
                continue;
            }
        }
        if (s.atRules && c == '@') {
            qsizetype j = i + 1;
            while (j < n && isIdent(text.at(j)))
                ++j;
            emitToken(i, j - i, Highlighter::Keyword);
            i = j;
            continue;
        }
        if (s.hexColors && c == '#') {
            qsizetype j = i + 1;
            while (j < n && isxdigit(text.at(j).toLatin1()))
                ++j;
            if (j - i == 4 || j - i == 7 || j - i == 9) {
                emitToken(i, j - i, Highlighter::Number);
                i = j;
                continue;
            }
        }

        // Numbers, only where a word can start.
        const bool wordStart = i == 0 || !isIdent(text.at(i - 1));
        if (wordStart && (c.isDigit() || ((c == '-' || c == '.') && i + 1 < n && text.at(i + 1).isDigit()))) {
            qsizetype j = i + 1;
            while (j < n && (text.at(j).isLetterOrNumber() || text.at(j) == '.' || text.at(j) == '_'))
                ++j;
            emitToken(i, j - i, Highlighter::Number);
            i = j;
            continue;
        }

        // Words: keywords, constants, keys and section names.
        if (wordStart && (isIdentStart(c) || (c == '~' && s.constants.contains("~")))) {
            qsizetype j = i + 1;
            while (j < n && isIdent(text.at(j)))
                ++j;
            const QString word = text.mid(i, j - i);
            const qsizetype after = nextNonSpace(j);
            const QChar next = after < n ? text.at(after) : QChar();
            const bool equalsFollows = next == '=' && (after + 1 >= n || text.at(after + 1) != '=');
            if (s.keywords.contains(word))
                emitToken(i, j - i, Highlighter::Keyword);
            else if (s.constants.contains(word))
                emitToken(i, j - i, Highlighter::Constant);
            else if (s.sections && next == '{')
                emitToken(i, j - i, Highlighter::Keyword);
            else if ((s.keysBeforeEquals && equalsFollows && (i <= keyStart || !s.sections))
                     || (s.keysBeforeColon && next == ':' && (after + 1 >= n || text.at(after + 1).isSpace())
                         && i <= keyStart))
                emitToken(i, j - i, Highlighter::Key);
            i = j;
            continue;
        }
        ++i;
    }
    return Normal;
}
}

Highlighter::Highlighter(QTextDocument *document, Theme *theme)
    : QObject(document), m_document(document), m_theme(theme) {
    updateFormats();
    // Plain text has no colors, so typing in it costs nothing here.
    connect(document, &QTextDocument::contentsChange, this, [this](int from, int, int added) {
        if (!m_marking && m_language != "text")
            highlight(from, from + added);
    });
    // The theme also changes for corner rounding and text size; only new
    // colors are worth recoloring the file for.
    if (theme)
        connect(theme, &Theme::changed, this, [this] {
            if (updateFormats())
                rehighlight();
        });
}

// Without recolor, the caller is about to replace the text, which colors it
// as it arrives; recoloring the old text first would be wasted.
void Highlighter::setLanguage(const QString &language, bool recolor) {
    if (language == m_language)
        return;
    m_language = language;
    if (recolor)
        rehighlight();
}

void Highlighter::rehighlight() {
    highlight(0, m_document->characterCount());
}

// Colors the blocks from `from` to `to`, and on past them while the state a
// block hands the next one keeps changing (an opened or closed multi-line
// comment). Like QSyntaxHighlighter, except the TextEdit hears about it once:
// QSyntaxHighlighter marks every block dirty separately, and Qt Quick's
// TextEdit relays out on each, which made a 10,000-line file take seconds.
void Highlighter::highlight(int from, int to) {
    const Spec &language = spec(m_language);
    const bool none = m_language == "text";
    QTextBlock block = m_document->findBlock(from);
    if (!block.isValid())
        return;
    const int start = block.position();
    int end = start;
    int state = block.previous().isValid() ? qMax(0, block.previous().userState()) : 0;

    for (; block.isValid(); block = block.next()) {
        const bool inRange = block.position() <= to;
        QList<QTextLayout::FormatRange> ranges;
        int next = none ? 0 : state;
        const QString text = block.text();
        // A long line is still scanned, uncolored, so a comment or string it
        // opens or closes is carried to the lines after it.
        if (!none)
            next = scan(language, text, state, [&](qsizetype at, qsizetype length, Token token) {
                if (text.size() <= LongLine)
                    ranges.append({int(at), int(length), m_formats[token]});
            });
        const int previousNext = block.userState();
        block.layout()->setFormats(ranges);
        block.setUserState(next);
        end = block.position() + block.length();
        state = next;
        if (!inRange && next == previousNext)
            break;
    }

    m_marking = true;
    m_document->markContentsDirty(start, qMin(end, m_document->characterCount()) - start);
    m_marking = false;
}

bool Highlighter::updateFormats() {
    const QColor fallback[TokenCount] = {"#808080", "#92a593", "#eb8b54", "#7d82d9", "#c89dc1", "#E9BB4F"};
    QColor colors[TokenCount];
    for (int t = 0; t < TokenCount; ++t)
        colors[t] = fallback[t];
    if (m_theme) {
        colors[Comment] = m_theme->secondaryForeground();
        colors[String] = m_theme->green();
        colors[Number] = m_theme->orange();
        colors[Key] = m_theme->blue();
        colors[Keyword] = m_theme->magenta();
        colors[Constant] = m_theme->yellow();
    }
    bool changed = false;
    for (int t = 0; t < TokenCount; ++t) {
        QTextCharFormat format;
        format.setForeground(colors[t]);
        format.setFontItalic(t == Comment);
        changed = changed || format != m_formats[t];
        m_formats[t] = format;
    }
    return changed;
}

QString Highlighter::tokens(const QString &language, const QStringList &lines) {
    QStringList out;
    int state = Normal;
    for (const QString &line : lines) {
        QString marks(line.size(), '.');
        state = scan(spec(language), line, state, [&](qsizetype start, qsizetype length, Token token) {
            for (qsizetype k = start; k < start + length && k < marks.size(); ++k)
                marks[k] = QLatin1String("csnkwx").at(token);
        });
        out << marks;
    }
    return out.join('\n');
}

QString Highlighter::languageFor(const QString &path, const QString &firstLine) {
    const QFileInfo info(path);
    const QString name = info.fileName().toLower();
    const QString suffix = info.suffix().toLower();

    static const QSet<QString> shellNames{".bashrc", ".bash_profile", ".bash_logout", ".profile", ".zshrc",
                                          ".zprofile", ".zshenv", "bashrc", "envs", "pkgbuild", ".xprofile"};
    static const QSet<QString> confNames{"config", ".gitconfig", "gitconfig", ".editorconfig", "inputrc",
                                         ".inputrc", "mimeapps.list", "user-dirs.dirs", ".env", "makepkg.conf"};
    if (shellNames.contains(name))
        return "shell";
    if (confNames.contains(name))
        return "conf";

    static const QHash<QString, QString> suffixes{
        {"lua", "lua"}, {"toml", "toml"}, {"json", "jsonc"}, {"jsonc", "jsonc"}, {"json5", "jsonc"},
        {"yml", "yaml"}, {"yaml", "yaml"}, {"css", "css"}, {"sh", "shell"}, {"bash", "shell"},
        {"zsh", "shell"}, {"conf", "conf"}, {"ini", "conf"}, {"cfg", "conf"}, {"desktop", "conf"},
        {"service", "conf"}, {"timer", "conf"}, {"socket", "conf"}, {"target", "conf"}, {"mount", "conf"},
        {"path", "conf"}, {"network", "conf"}, {"netdev", "conf"}, {"link", "conf"}, {"env", "conf"},
        {"theme", "conf"}};
    if (suffixes.contains(suffix))
        return suffixes.value(suffix);

    if (firstLine.startsWith("#!")) {
        if (firstLine.contains("lua"))
            return "lua";
        if (firstLine.contains("sh"))
            return "shell";
    }
    return "text";
}

QString Highlighter::commentPrefix(const QString &language) {
    if (language == "lua")
        return "--";
    if (language == "jsonc")
        return "//";
    if (language == "css")
        return "/*";
    if (language == "text" || language == "json")
        return {};
    return "#";
}

QString Highlighter::defaultIndent(const QString &language) {
    return language == "yaml" || language == "jsonc" || language == "json" ? "  " : "    ";
}
