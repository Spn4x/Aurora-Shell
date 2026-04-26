import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects 
import Aurora.Shell 

Item {
    id: playerRoot

    property bool isExpanded: false
    property int expandedImplicitHeight: expandedLayout.implicitHeight
    property int pinnedContentWidth: pinnedLyricText.implicitWidth

    property string trackId: Backend.mediaTitle + Backend.mediaArtist
    property bool showIntro: false

    onTrackIdChanged: {
        if (Backend.mediaTitle !== "") {
            showIntro = true;
            introTimer.restart();
        }
    }

    Timer {
        id: introTimer
        interval: 2000 
        onTriggered: showIntro = false
    }
    
    function formatTime(seconds) {
        if (seconds <= 0) return "0:00";
        var m = Math.floor(seconds / 60);
        var s = seconds % 60;
        return m + ":" + (s < 10 ? "0" : "") + s;
    }

    component SystemIcon: Button {
        property string iconName: ""
        property color iconColor: "white"
        property int size: 24
        width: size; height: size
        icon.name: iconName; icon.color: iconColor
        icon.width: size; icon.height: size
        background: Item {} 
        focusPolicy: Qt.NoFocus; hoverEnabled: false; down: false
    }

    // =====================================
    // 1. PILL VIEW (The "Peek")
    // =====================================
    Item {
        anchors.fill: parent
        opacity: !playerRoot.isExpanded ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.InOutQuad } }
        clip: true 

        // STATE A: Centered "Now Playing" Intro
        Item {
            id: introContainer
            anchors.left: parent.left; anchors.right: parent.right
            height: parent.height
            
            y: showIntro && !Backend.mediaPinned ? 0 : -height
            opacity: showIntro && !Backend.mediaPinned ? 1 : 0
            
            Behavior on y { NumberAnimation { duration: 400; easing.type: Easing.OutBack; easing.overshoot: 1.1 } }
            Behavior on opacity { NumberAnimation { duration: 300 } }
            
            Row {
                anchors.centerIn: parent
                spacing: 12 // Slightly more space for bigger icon
                
                SystemIcon { 
                    iconName: "audio-headphones-symbolic"
                    size: 28 // THE FIX: Big Headphones
                    iconColor: AppTheme.accent 
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: "Now Playing"
                    color: AppTheme.accent
                    font.pixelSize: 17 // Bumped to match icon weight
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }

        // STATE B: Main Controls
        RowLayout {
            id: mainRow
            anchors.left: parent.left; anchors.right: parent.right
            anchors.margins: 16
            height: parent.height - 12
            
            y: !showIntro && !Backend.mediaPinned ? 6 : parent.height
            opacity: !showIntro && !Backend.mediaPinned ? 1 : 0
            
            Behavior on y { NumberAnimation { duration: 400; easing.type: Easing.OutBack; easing.overshoot: 1.1 } }
            Behavior on opacity { NumberAnimation { duration: 300 } }

            spacing: 12

            // THE FIX: Bulletproof Album Art using OpacityMask
            Item {
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34

                // Base container holding the fallback icon
                Rectangle {
                    anchors.fill: parent
                    radius: 8 
                    color: AppTheme.pillActionBg 
                    
                    SystemIcon {
                        anchors.centerIn: parent
                        iconName: "audio-x-generic-symbolic"
                        size: 18
                        iconColor: AppTheme.fg
                        visible: pillImg.status !== Image.Ready
                    }
                }

                // The hidden source image
                Image {
                    id: pillImg
                    anchors.fill: parent
                    source: Backend.mediaArt
                    fillMode: Image.PreserveAspectCrop
                    visible: false 
                }

                // The hidden mask shape
                Rectangle {
                    id: pillMaskShape
                    anchors.fill: parent
                    radius: 8
                    visible: false
                }

                // The actual rounded cutout
                OpacityMask {
                    anchors.fill: parent
                    source: pillImg
                    maskSource: pillMaskShape
                    visible: pillImg.status === Image.Ready
                }

                // Hollow overlay border so it stays crisp
                Rectangle {
                    anchors.fill: parent
                    radius: 8
                    color: "transparent"
                    border.color: AppTheme.pillActionBorder; border.width: 1
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0
                
                Text {
                    text: Backend.mediaTitle !== "" ? Backend.mediaTitle : "No Media"
                    color: AppTheme.fg; font.pixelSize: 14; font.bold: true
                    elide: Text.ElideRight; Layout.fillWidth: true
                }
                Text {
                    text: Backend.mediaArtist
                    color: Qt.rgba(AppTheme.fg.r, AppTheme.fg.g, AppTheme.fg.b, 0.6)
                    font.pixelSize: 12
                    elide: Text.ElideRight; Layout.fillWidth: true
                }
            }

            RowLayout {
                spacing: 4
                SystemIcon { 
                    iconName: Backend.mediaStatus === "Playing" ? "media-playback-pause-symbolic" : "media-playback-start-symbolic"
                    size: 24; iconColor: AppTheme.fg
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: Backend.mediaPlayPause() } 
                }
                SystemIcon { 
                    iconName: "media-skip-forward-symbolic"
                    size: 24; iconColor: AppTheme.fg
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: Backend.mediaNext() } 
                }
            }
        }
        
        // STATE C: PINNED LYRICS WITH SLIDING ANIMATION
        Item {
            id: pinnedLyricContainer
            anchors.fill: parent
            opacity: Backend.mediaPinned ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 300 } }
            clip: true
            
            property string currentText: Backend.mediaHasLyrics ? (Backend.mediaCurrentLyric !== "" ? Backend.mediaCurrentLyric : "\u266a") : Backend.mediaTitle
            
            onCurrentTextChanged: {
                if (!Backend.mediaPinned) return;
                oldLyricText.text = pinnedLyricText.text;
                oldLyricText.y = 0;
                oldLyricText.opacity = 1;
                
                pinnedLyricText.text = currentText;
                pinnedLyricText.y = parent.height;
                pinnedLyricText.opacity = 0;
                
                slideAnim.restart();
            }

            ParallelAnimation {
                id: slideAnim
                NumberAnimation { target: oldLyricText; property: "y"; to: -parent.height; duration: 350; easing.type: Easing.InOutQuad }
                NumberAnimation { target: oldLyricText; property: "opacity"; to: 0; duration: 350; easing.type: Easing.InOutQuad }
                NumberAnimation { target: pinnedLyricText; property: "y"; to: 0; duration: 350; easing.type: Easing.InOutQuad }
                NumberAnimation { target: pinnedLyricText; property: "opacity"; to: 1; duration: 350; easing.type: Easing.InOutQuad }
            }
            
            Text {
                id: oldLyricText
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width - 24
                height: parent.height
                color: AppTheme.fg; font.pixelSize: 16; font.bold: true
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight; maximumLineCount: 1; opacity: 0
            }
            
            Text {
                id: pinnedLyricText
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width - 24
                height: parent.height
                text: pinnedLyricContainer.currentText
                color: AppTheme.fg; font.pixelSize: 16; font.bold: true
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight; maximumLineCount: 1
            }
        }
    }

    // =====================================
    // 2. EXPANDED VIEW
    // =====================================
    ColumnLayout {
        id: expandedLayout
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        anchors.margins: 20
        spacing: 16
        
        opacity: playerRoot.isExpanded ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.InOutQuad } }

        // Top Row: Album Art + Info + Inline Controls
        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            // THE FIX: Bulletproof Album Art 64x64
            Item {
                Layout.preferredWidth: 64
                Layout.preferredHeight: 64

                Rectangle {
                    anchors.fill: parent
                    radius: 12
                    color: AppTheme.pillActionBg 
                    
                    SystemIcon {
                        anchors.centerIn: parent
                        iconName: "audio-x-generic-symbolic"
                        size: 32
                        iconColor: AppTheme.fg
                        visible: expandedImg.status !== Image.Ready
                    }
                }

                Image {
                    id: expandedImg
                    anchors.fill: parent
                    source: Backend.mediaArt
                    fillMode: Image.PreserveAspectCrop
                    visible: false 
                }

                Rectangle {
                    id: expandedMaskShape
                    anchors.fill: parent
                    radius: 12
                    visible: false
                }

                OpacityMask {
                    anchors.fill: parent
                    source: expandedImg
                    maskSource: expandedMaskShape
                    visible: expandedImg.status === Image.Ready
                }

                Rectangle {
                    anchors.fill: parent
                    radius: 12
                    color: "transparent"
                    border.color: AppTheme.pillActionBorder; border.width: 1
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                Text { text: Backend.mediaTitle !== "" ? Backend.mediaTitle : "No Media"; color: AppTheme.fg; font.pixelSize: 16; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                Text { text: Backend.mediaArtist; color: Qt.rgba(AppTheme.fg.r, AppTheme.fg.g, AppTheme.fg.b, 0.7); font.pixelSize: 14; elide: Text.ElideRight; Layout.fillWidth: true }
            }
            
            // Inline Controls
            RowLayout {
                Layout.alignment: Qt.AlignVCenter
                spacing: 8
                
                SystemIcon { 
                    iconName: "media-skip-backward-symbolic"; size: 24; iconColor: AppTheme.fg
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: Backend.mediaPrev() } 
                }
                
                SystemIcon { 
                    iconName: Backend.mediaStatus === "Playing" ? "media-playback-pause-symbolic" : "media-playback-start-symbolic"
                    size: 32; iconColor: AppTheme.fg
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: Backend.mediaPlayPause() } 
                }
                
                SystemIcon { 
                    iconName: "media-skip-forward-symbolic"; size: 24; iconColor: AppTheme.fg
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: Backend.mediaNext() } 
                }
            }
        }

        // Bottom Row: Inline Timeline & Pin Side-by-Side
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4 
            spacing: 12

            Text {
                text: formatTime(Backend.mediaPosition)
                color: AppTheme.fg; font.pixelSize: 12; font.bold: true
            }

            Rectangle {
                Layout.fillWidth: true
                height: 6
                radius: 3
                color: Qt.rgba(AppTheme.fg.r, AppTheme.fg.g, AppTheme.fg.b, 0.2)
                
                Rectangle {
                    width: Backend.mediaDuration > 0 ? Math.min(parent.width * (Backend.mediaPosition / Backend.mediaDuration), parent.width) : 0
                    height: parent.height
                    radius: 3
                    color: AppTheme.fg
                    Behavior on width { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }
                }
            }

            Text {
                text: formatTime(Backend.mediaDuration)
                color: Qt.rgba(AppTheme.fg.r, AppTheme.fg.g, AppTheme.fg.b, 0.6)
                font.pixelSize: 12; font.bold: true
            }

            SystemIcon {
                iconName: "view-pin-symbolic"
                size: 18 
                iconColor: Backend.mediaPinned ? AppTheme.accent : Qt.rgba(AppTheme.fg.r, AppTheme.fg.g, AppTheme.fg.b, 0.6)
                
                MouseArea { 
                    anchors.fill: parent; anchors.margins: -12 
                    cursorShape: Qt.PointingHandCursor; 
                    onClicked: {
                        Backend.setMediaPinned(!Backend.mediaPinned);
                        if (Backend.mediaPinned) {
                            island.state = "pill"; 
                        }
                    }
                }
            }
        }
    }
}