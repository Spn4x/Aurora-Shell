import QtQuick
import QtQuick.Window
import Aurora.Foresight

Window {
    id: root
    // THE FIX: Provide fallback dimensions so Wayland never kills the client
    // for having a 0x0 size if it's not anchored to the screen edges.
    width: 1920
    height: 1080
    
    visible: false
    color: "transparent"
    flags: Qt.FramelessWindowHint

    Connections {
        target: Backend
        function onRequestShow() {
            foresight.show()
        }
        function onRequestHide() { 
            foresight.hide() 
        }
    }

    // Catch clicks on the transparent background to close the launcher
    MouseArea {
        anchors.fill: parent
        onClicked: Backend.Hide()
    }

    Foresight {
        id: foresight
        anchors.centerIn: parent

        onOpacityChanged: {
            if (opacity === 0.0 && scale < 1.0) {
                Backend.notifyHidden()
            }
        }
    }
}