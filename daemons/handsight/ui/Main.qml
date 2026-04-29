import QtQuick
import QtQuick.Window
import Aurora.Shell 

Window {
    id: root
    width: Screen.width 
    height: Screen.height 
    visible: false 
    color: "transparent"
    flags: Qt.FramelessWindowHint

    DynamicIsland {
        id: island
        objectName: "dynamicPill"
    }
}