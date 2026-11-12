// plain — Omarchy's text editor for config files: conventional keys, the
// theme's colors, and saves that never damage a file.

#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>
#include <cstdio>

#include "arguments.h"
#include "document.h"
#include "editing.h"
#include "portalfilepicker.h"
#include "theme.h"
#include "translator.h"

static const char *Usage = "usage: plain [+LINE] [--] [FILE]\n";

int main(int argc, char *argv[]) {
    // Without a display Qt aborts in the constructor, before --help or
    // --version can answer; offscreen lets them, and the window is refused below.
    const bool headless = qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
                       && qEnvironmentVariableIsEmpty("DISPLAY");
    if (headless)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);
    app.setApplicationName("plain");
    app.setApplicationVersion("0.2.0");

    const Arguments arguments = Arguments::parse(app.arguments());
    if (arguments.help) {
        fputs(Usage, stdout);
        return 0;
    }
    if (arguments.version) {
        printf("plain %s\n", qPrintable(app.applicationVersion()));
        return 0;
    }
    if (!arguments.error.isEmpty()) {
        fprintf(stderr, "plain: %s\n%s", qPrintable(arguments.error), Usage);
        return 1;
    }
    if (headless && app.platformName() == "offscreen") {
        fputs("plain: no display to open a window on\n", stderr);
        return 1;
    }

    // Matches the window to plain.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("plain");
    app.setWindowIcon(QIcon::fromTheme("plain"));

    // Omarchy points fontconfig's monospace at the font picked in its menu,
    // so asking for "monospace" follows the user's choice.
    QFont font("monospace");
    font.setStyleHint(QFont::Monospace);
    app.setFont(font);
    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Document document;
    document.setTheme(&theme);
    Editing editing(&document);
    PortalFilePicker picker;

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("doc", &document);
    engine.rootContext()->setContextProperty("editing", &editing);
    engine.rootContext()->setContextProperty("picker", &picker);
    engine.rootContext()->setContextProperty("startPath", arguments.path);
    engine.rootContext()->setContextProperty("startLine", arguments.line);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
