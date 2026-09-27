// Stand-in for the real Quickshell bar: a bright top-layer panel on every screen.
import Quickshell
import QtQuick

ShellRoot {
    Variants {
        model: Quickshell.screens
        PanelWindow {
            required property var modelData
            screen: modelData
            anchors { top: true; left: true; right: true }
            implicitHeight: 48
            color: "#00e676"
            Text { anchors.centerIn: parent; text: "TEST BAR " + modelData.name; font.pixelSize: 24 }
        }
    }
}
