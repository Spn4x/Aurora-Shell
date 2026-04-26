pragma Singleton
import QtQuick
import Aurora.Foresight

QtObject {
    property color bg: Backend.themeData["gnome-bg"] ?? "#28282C"
    property color fg: Backend.themeData["gnome-fg"] ?? "#D7E0BB"
    property color accent: Backend.themeData["aurora_accent"] ?? "#CBA6F7"
    property color surface: Backend.themeData["gnome-highlight"] ?? "#3E3E41"

    property color surfaceAlpha: Qt.rgba(surface.r, surface.g, surface.b, 0.5)
    property color borderAlpha: Qt.rgba(accent.r, accent.g, accent.b, 0.6)

    // Dark text for the selected item to match GTK
    property color selectedText: "#1E1E2E"

    property int launcherWidth: 380      // Perfect visual match for your GTK image
    property int itemHeight: 56          // Increased to comfortably fit 36px icons
    property int searchHeight: 46        // Taller search bar
    
    property int boxRadius: 12
    property int entryRadius: 10
    property int rowRadius: 6
    property int rowSelectedRadius: 8
}