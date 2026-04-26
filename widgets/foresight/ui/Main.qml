import QtQuick
import QtQuick.Window
import Aurora.Foresight

Window {
    id: root
    visible: false // THE FIX: Must be false! C++ will show it after LayerShell is ready.
    color: "transparent"
    flags: Qt.FramelessWindowHint

    Connections {
        target: Backend
        function onRequestHide() { 
            foresight.hide() 
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: foresight.hide()
    }

    Foresight {
        id: foresight
        anchors.centerIn: parent

        onOpacityChanged: {
            if (opacity === 0.0 && scale < 1.0) {
                Qt.quit()
            }
        }
    }

    Component.onCompleted: {
        foresight.show()
    }
}