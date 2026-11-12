import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 980
    height: 720
    minimumWidth: 480
    minimumHeight: 320
    visible: true
    title: (doc.modified ? "● " : "") + doc.fileName + " — Plain"
    color: theme.background
    // The chrome (banner, dialogs, labels) at the desktop's text size too.
    font.pixelSize: theme.textSize

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.foreground: theme.foreground
    Material.background: theme.background
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    // What to do once the unsaved-changes question is answered: "close" or "open".
    property string pendingAction: ""
    // Set once the user agreed to lose changes, so closing doesn't ask again.
    property bool discardConfirmed: false

    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    Settings {
        id: settings
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/plain/settings.conf"
        // Px above or below the desktop's text size, so the editor keeps
        // following Omarchy's Text size slider after Ctrl+Plus or Ctrl+Minus.
        property int zoom: 0
        property bool wrap: false
    }

    readonly property int textSize: Math.max(8, Math.min(48, theme.textSize + settings.zoom))

    // An untitled file is named in the save dialog first, and finishes in
    // onSaveSelected or onCancelled.
    function save() {
        Qt.inputMethod.commit()
        if (doc.path.length === 0)
            picker.saveFile("")
        else
            afterSave(doc.save())
    }

    // A save asked for while closing or opening another file goes on to do
    // that once it succeeds. Whatever happened, nothing waits on it after.
    function afterSave(saved) {
        const action = pendingAction
        pendingAction = ""
        if (saved && action.length > 0)
            finish(action)
    }

    function saveAs() {
        if (!doc.editable && !doc.locked)
            return
        Qt.inputMethod.commit()
        picker.saveFile(doc.path)
    }

    // Close or open-another, asking first about unsaved changes.
    function request(action) {
        if (doc.modified && !discardConfirmed) {
            pendingAction = action
            unsaved.open()
            return
        }
        finish(action)
    }

    function finish(action) {
        pendingAction = ""
        if (action === "close") {
            discardConfirmed = true
            win.close()
        } else if (action === "open") {
            picker.openFile(doc.path.length > 0 ? doc.path.substring(0, doc.path.lastIndexOf("/")) : "")
        }
    }

    onClosing: function(close) {
        if (doc.modified && !discardConfirmed) {
            close.accepted = false
            request("close")
        }
    }

    Shortcut { sequence: "Ctrl+S"; context: Qt.ApplicationShortcut; onActivated: win.save() }
    Shortcut { sequence: "Ctrl+Shift+S"; context: Qt.ApplicationShortcut; onActivated: win.saveAs() }
    Shortcut { sequence: "Ctrl+O"; context: Qt.ApplicationShortcut; onActivated: win.request("open") }
    Shortcut { sequences: ["Ctrl+W", "Ctrl+Q"]; context: Qt.ApplicationShortcut; onActivated: win.request("close") }
    Shortcut { sequence: "Ctrl+F"; context: Qt.ApplicationShortcut; onActivated: findBar.show(false) }
    Shortcut { sequence: "Ctrl+H"; context: Qt.ApplicationShortcut; onActivated: findBar.show(true) }
    Shortcut { sequences: ["F3", StandardKey.FindNext]; context: Qt.ApplicationShortcut; onActivated: findBar.next(false) }
    Shortcut { sequences: ["Shift+F3", StandardKey.FindPrevious]; context: Qt.ApplicationShortcut; onActivated: findBar.next(true) }
    Shortcut { sequence: "Ctrl+G"; context: Qt.ApplicationShortcut; onActivated: goToLine.open() }
    Shortcut { sequence: "Ctrl+/"; context: Qt.ApplicationShortcut; enabled: doc.editable; onActivated: editor.toggleComment() }
    Shortcut { sequence: "Alt+Z"; context: Qt.ApplicationShortcut; onActivated: settings.wrap = !settings.wrap }
    // Each key once: listing StandardKey.ZoomIn/ZoomOut as well registers
    // Ctrl++ and Ctrl+- twice, which Qt treats as ambiguous and ignores.
    Shortcut { sequences: ["Ctrl+=", "Ctrl++"]; context: Qt.ApplicationShortcut; onActivated: if (win.textSize < 48) settings.zoom += 1 }
    Shortcut { sequence: "Ctrl+-"; context: Qt.ApplicationShortcut; onActivated: if (win.textSize > 8) settings.zoom -= 1 }
    Shortcut { sequence: "Ctrl+0"; context: Qt.ApplicationShortcut; onActivated: settings.zoom = 0 }
    Shortcut { sequences: ["F1", "Ctrl+?"]; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: win.helpVisible; onActivated: win.helpVisible = false }

    Connections {
        target: picker
        function onOpenSelected(url) {
            win.discardConfirmed = false
            doc.openUrl(url)
            editor.goToLine(1)
        }
        function onSaveSelected(url) { win.afterSave(doc.saveAsUrl(url)) }
        function onCancelled() { win.afterSave(false) }
        function onFailed(message) {
            pickerError.text = message
            win.afterSave(false)
        }
    }

    Component.onCompleted: {
        doc.attach(editor.area)
        if (startPath.length > 0)
            doc.open(startPath)
        editor.goToLine(Math.max(1, startLine))
    }

    // Right-to-left languages mirror the chrome; the text keeps its own
    // direction line by line.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            Banner {
                Layout.fillWidth: true
                onSaveAsRequested: win.saveAs()
                onDone: editor.focusText()
            }

            Label {
                id: pickerError
                Layout.fillWidth: true
                Layout.margins: 10
                visible: text.length > 0
                color: theme.red
                wrapMode: Text.Wrap
                MouseArea { anchors.fill: parent; onClicked: pickerError.text = "" }
            }

            FindBar {
                id: findBar
                Layout.fillWidth: true
                editor: editor
            }

            EditorView {
                id: editor
                objectName: "editorView"
                Layout.fillWidth: true
                Layout.fillHeight: true
                fontSize: win.textSize
                wrap: settings.wrap
                LayoutMirroring.enabled: false
                LayoutMirroring.childrenInherit: true
            }

            Footer {
                Layout.fillWidth: true
                onSaveRequested: win.save()
                onOpenRequested: win.request("open")
                onHelpRequested: win.helpVisible = !win.helpVisible
            }
        }

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
            onDismissed: win.helpVisible = false
        }
    }

    GoToLine {
        id: goToLine
        anchors.centerIn: parent
        onAccepted: function(line) { editor.goToLine(line) }
        onClosed: editor.focusText()
    }

    // Unsaved changes: Cancel, Discard, Save, laid out like Omawrite's.
    // Escape cancels; Alt+C, Alt+D and Alt+S pick.
    Popup {
        id: unsaved
        anchors.centerIn: parent
        width: Math.min(440, win.width - 48)
        modal: true
        focus: true
        padding: 22
        onOpened: saveButton.forceActiveFocus()
        // Closed by Escape or a click outside, rather than a choice: nothing
        // waits any more. A choice is only cleared here, since the popup
        // finishes closing after the choice's own work has started.
        onClosed: {
            if (win.pendingAction.length > 0 && !choosing) {
                win.pendingAction = ""
                editor.focusText()
            }
            choosing = false
        }
        property bool choosing: false
        Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.55) }

        function choose(choice) {
            choosing = true
            close()
            if (choice === "save") {
                // A failed save leaves the banner up and the window open.
                win.save()
            } else if (choice === "discard") {
                win.discardConfirmed = win.pendingAction === "close"
                win.finish(win.pendingAction)
            } else {
                win.pendingAction = ""
                editor.focusText()
            }
        }

        Shortcut { sequence: "Alt+C"; enabled: unsaved.opened; onActivated: unsaved.choose("cancel") }
        Shortcut { sequence: "Alt+D"; enabled: unsaved.opened; onActivated: unsaved.choose("discard") }
        Shortcut { sequence: "Alt+S"; enabled: unsaved.opened; onActivated: unsaved.choose("save") }

        background: DialogBackground {}

        contentItem: ColumnLayout {
            spacing: 12
            Label {
                text: qsTr("Unsaved changes")
                color: theme.brightForeground
                font.pixelSize: Math.round(theme.textSize * 1.333)
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                text: win.pendingAction === "open"
                    ? qsTr("Save changes to %1 before opening another file?").arg(doc.fileName)
                    : qsTr("Save changes to %1 before closing?").arg(doc.fileName)
                color: theme.foreground
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Layout.topMargin: 14
                spacing: 8
                DialogButton {
                    id: cancelButton
                    text: qsTr("Cancel")
                    formattedText: qsTr("<u>C</u>ancel")
                    KeyNavigation.tab: discardButton
                    onClicked: unsaved.choose("cancel")
                }
                DialogButton {
                    id: discardButton
                    text: qsTr("Discard")
                    formattedText: qsTr("<u>D</u>iscard")
                    KeyNavigation.tab: saveButton
                    onClicked: unsaved.choose("discard")
                }
                DialogButton {
                    id: saveButton
                    primary: true
                    text: qsTr("Save")
                    formattedText: qsTr("<u>S</u>ave")
                    KeyNavigation.tab: cancelButton
                    onClicked: unsaved.choose("save")
                }
            }
        }
    }
}
