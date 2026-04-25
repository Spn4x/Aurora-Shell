pragma Singleton
import QtQuick
import Aurora.Shell

QtObject {
    // Reactive Bindings (No readonly!)
    property color bg: Backend.themeData["gnome-bg"] ?? "#28282C"
    property color fg: Backend.themeData["gnome-fg"] ?? "#f2f2f2"
    property color accent: Backend.themeData["aurora_accent"] ?? "#cba6f7"

    property color borderAlpha: Qt.rgba(accent.r, accent.g, accent.b, 0.4)

    // Hardware indicator colors
    property color colorMic: "#ff9e64" 
    property color colorCam: "#8ff0a4" 
    property color colorKill: "#ff7b63" 

    // Action Pill CSS Translated
    // 0.08 opacity white = #14ffffff
    property color pillActionBg: "#14ffffff"
    // 0.15 opacity white = #26ffffff
    property color pillActionBgHover: "#26ffffff"
    // 0.05 opacity white = #0dffffff
    property color pillActionBorder: "#0dffffff"
    
    property int pillActionRadius: 12

    // Sizing (Increased to match GTK visual weight)
    property int pillWidth: 280
    property int pillHeight: 46
    property int expandedMinWidth: 400
    property int expandedMinHeight: 120
    
    property int pillRadius: 23
    property int expandedRadius: 16

    // Typography
    property int summarySize: 15 
    property int bodySize: 13    
    property bool summaryBold: true
}