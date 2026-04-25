import QtQuick
import QtQuick.Window
import Aurora.Shell 

Window {
    id: root
    width: 600 
    height: 800 
    visible: false 
    color: "transparent"
    flags: Qt.FramelessWindowHint

    DynamicIsland {
        id: island
        objectName: "dynamicPill"
        
        anchors.horizontalCenter: parent.horizontalCenter
        y: 10
    }
}