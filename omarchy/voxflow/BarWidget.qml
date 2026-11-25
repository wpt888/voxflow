import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons as Commons
import qs.Ui

BarWidget {
    id: root
    moduleName: "local.voxflow"
    implicitWidth: button.implicitWidth
    implicitHeight: button.implicitHeight

    readonly property string executable: Quickshell.env("HOME") + "/.local/bin/voxflow"
    readonly property string runtimeDir: Quickshell.env("XDG_RUNTIME_DIR").replace(/\/+$/, "")
    readonly property bool busy: phase === "recording" || phase === "transcribing"
    readonly property bool available: phase !== "unavailable"
    readonly property bool dictationEnabled: available && phase !== "paused" && phase !== "stopped"
    property bool opened: false
    property string phase: "unavailable"
    property string lockPath: ""
    property bool refreshPending: false

    function refreshStatus() {
        if (query.running) {
            refreshPending = true
        } else {
            query.running = true
        }
    }

    function completeStatus(next) {
        phase = next === "ready" || next === "recording" || next === "transcribing"
                || next === "paused" || next === "stopped"
            ? next : "unavailable"
    }

    function open() {
        if (lockPath === "" && !locate.running)
            locate.running = true
        refreshStatus()
        opened = true
    }

    function close() {
        opened = false
    }

    Commons.Translations {
        id: strings
        directory: Qt.resolvedUrl("i18n")
    }

    Process {
        id: locate
        command: [root.executable, "status-path"]
        running: true
        stdout: StdioCollector {
            id: pathOutput
            waitForEnd: true
        }
        onExited: function(exitCode) {
            if (exitCode !== 0)
                return
            let path = pathOutput.text.trim()
            let prefix = root.runtimeDir + "/"
            if (path.startsWith(prefix) && /^voxflow-[0-9a-f]{16}\/lock$/.test(path.slice(prefix.length)))
                root.lockPath = path
        }
    }

    Component.onCompleted: refreshStatus()

    FileView {
        path: root.lockPath
        watchChanges: root.lockPath !== ""
        printErrors: false
        onFileChanged: {
            reload()
            root.refreshStatus()
        }
        onLoadFailed: root.refreshStatus()
    }

    Process {
        id: query
        command: [root.executable, "status"]
        stdout: StdioCollector {
            id: statusOutput
            waitForEnd: true
        }
        onExited: function(exitCode) {
            root.completeStatus(exitCode === 0 ? statusOutput.text.trim() : "")
        }
        onRunningChanged: if (!running && root.refreshPending) {
            root.refreshPending = false
            Qt.callLater(root.refreshStatus)
        }
    }

    Process {
        id: toggle
        onExited: root.refreshStatus()
    }

    // A killed worker releases the lock without a file change.
    Timer {
        interval: 1000
        repeat: true
        running: root.busy
        onTriggered: root.refreshStatus()
    }

    BarIconButton {
        id: button
        anchors.fill: parent
        bar: root.bar
        active: root.opened
        dimmed: !root.dictationEnabled
        tooltipText: strings.t(root.phase)
        onPressed: function(mouseButton) {
            if (mouseButton !== Qt.LeftButton)
                return
            if (root.opened)
                root.close()
            else
                root.open()
        }
        iconComponent: Component {
            Row {
                width: implicitWidth
                height: implicitHeight
                spacing: 1.6

                Repeater {
                    model: [6, 12, 20, 14, 8]
                    Rectangle {
                        required property int modelData
                        width: 2.8
                        height: modelData
                        radius: width / 2
                        anchors.verticalCenter: parent.verticalCenter
                        color: button.foreground
                    }
                }
            }
        }
    }

    KeyboardPanel {
        id: panel
        anchorItem: button
        bar: root.bar
        owner: root
        open: root.opened
        focusTarget: powerSwitch
        contentWidth: panel.fittedContentWidth(Commons.Style.space(280))
        contentHeight: panel.fittedContentHeight(powerSwitch.implicitHeight)

        Toggle {
            id: powerSwitch
            anchors.fill: parent
            label: strings.t("dictation")
            description: strings.t(root.available ? (root.dictationEnabled ? "onHint" : "offHint") : "unavailableHint")
            checked: root.dictationEnabled
            enabled: root.available && !toggle.running
            Keys.onEscapePressed: root.close()
            onClicked: {
                toggle.command = [root.executable, root.dictationEnabled ? "stop" : "resume"]
                toggle.running = true
            }
        }
    }

    Rectangle {
        visible: root.busy
        width: Math.round(button.visualIconSize * 0.32)
        height: width
        radius: width / 2
        x: (parent.width + button.visualIconSize) / 2 - width
        y: (parent.height + button.visualIconSize) / 2 - height
        color: root.phase === "recording" ? "#ff6168" : "#ffbe52"
    }
}
