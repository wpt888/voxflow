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
    property string phase: "unavailable"
    property string lockPath: ""
    property bool refreshPending: false

    function refreshStatus() {
        if (lockPath === "")
            return
        if (query.running) {
            refreshPending = true
        } else {
            query.running = true
        }
    }

    function completeStatus(next) {
        phase = next === "ready" || next === "recording" || next === "transcribing" || next === "paused"
            ? next : "unavailable"
    }

    Commons.Translations {
        id: strings
        directory: Qt.resolvedUrl("i18n")
    }

    Process {
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
            if (path.startsWith(prefix) && /^voxflow-[0-9a-f]{16}\/lock$/.test(path.slice(prefix.length))) {
                root.lockPath = path
                root.refreshStatus()
            }
        }
    }

    FileView {
        path: root.lockPath
        watchChanges: root.lockPath !== ""
        printErrors: false
        onFileChanged: {
            reload()
            root.refreshStatus()
        }
        onLoadFailed: root.completeStatus("")
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
        pressable: root.phase !== "unavailable"
        dimmed: root.phase === "unavailable" || root.phase === "paused"
        tooltipText: strings.t(root.phase)
        onPressed: function(mouseButton) {
            if (mouseButton === Qt.LeftButton && !toggle.running) {
                toggle.command = [root.executable, root.phase === "paused" ? "resume" : "pause"]
                toggle.running = true
            }
        }
        iconComponent: Component {
            Image {
                source: Qt.resolvedUrl("voxflow.png")
                fillMode: Image.PreserveAspectFit
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
