import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Aurora.Shell 

Rectangle {
    id: island
    state: "hidden" 
    
    // Flags to delay backend data destruction until animations finish
    property bool pendingReadyForNext: false
    property bool pendingPrivacyAction: false
    property string pendingPrivacyType: ""
    property int pendingPrivacyPid: 0
    property string pendingPrivacyName: ""

    property int expandedHeight: {
        if (Backend.displayMode === "notification") return notifColumn.implicitHeight + 32
        if (Backend.displayMode === "privacy") {
            if (Backend.privacyApps.length === 1) return privacySingleColumn.implicitHeight + 32
            return privacyMultiColumn.implicitHeight + 32
        }
        return AppTheme.expandedMinHeight
    }

    width: AppTheme.pillWidth
    height: AppTheme.pillHeight
    radius: AppTheme.pillRadius
    opacity: 0
    scale: 0.8
    color: AppTheme.bg
    border.color: AppTheme.borderAlpha
    border.width: 2 
    clip: true

    function startTimer() {
        autoDismissTimer.stop()
        if (Backend.displayMode === "osd") {
            autoDismissTimer.interval = 1500
        } else if (island.state === "expanded") {
            autoDismissTimer.interval = Backend.hasActions ? 12000 : 8000
        } else {
            autoDismissTimer.interval = 4000
        }
        autoDismissTimer.restart()
    }

    function requestPrivacyAction(type, pid, name) {
        if (Backend.privacyApps.length === 1 || type === "killAll") {
            island.pendingPrivacyAction = true
            island.pendingPrivacyType = type
            island.pendingPrivacyPid = pid
            island.pendingPrivacyName = name
            island.state = "hidden" 
        } else {
            if (type === "kill") Backend.killPrivacyApp(pid, name)
            else if (type === "ignore") Backend.ignorePrivacyApp(pid, name)
        }
    }

    onStateChanged: {
        if (state === "pill" || state === "expanded") {
            island.startTimer()
        }
    }

    Timer {
        id: autoDismissTimer
        onTriggered: {
            if (island.state === "expanded") {
                island.pendingReadyForNext = true;
                island.state = "pill";
            } else if (island.state === "pill") {
                if (Backend.displayMode === "notification" || Backend.displayMode === "osd") {
                    Backend.readyForNext();
                }
            }
        }
    }

    Connections {
        target: Backend
        function onRequestShow() { 
            if (island.state === "hidden") island.state = "pill"
            island.startTimer() 
        }
        function onRequestHide() { 
            island.state = "hidden" 
            autoDismissTimer.stop()
        }
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

    // ==========================================
    // PILL VIEW
    // ==========================================
    Item {
        id: pillView
        anchors.fill: parent
        
        // THE FIX: Disables the view fully when faded out so it doesn't trap clicks
        visible: opacity > 0
        opacity: (island.state === "pill") ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150; easing.type: Easing.OutQuad } }

        // OSD CONTAINER
        Item {
            id: osdContainer
            anchors.fill: parent
            opacity: Backend.displayMode === "osd" ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 150 } }

            Row {
                anchors.centerIn: parent
                spacing: 12

                SystemIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: Backend.osdIcon
                    iconColor: AppTheme.fg
                    size: 20
                }

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 160
                    height: 6
                    radius: 3
                    color: Qt.rgba(1, 1, 1, 0.2)

                    Rectangle {
                        width: Math.min(Backend.osdLevel, 1.0) * parent.width
                        height: parent.height
                        radius: 3
                        color: AppTheme.fg
                        Behavior on width { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                    }
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: Backend.osdLevel > 1.0
                    text: "+" + Math.round((Backend.osdLevel - 1.0) * 100) + "%"
                    color: AppTheme.fg
                    font.pixelSize: 12
                    font.bold: true
                }
            }
        }

        // TEXT & INDICATOR CONTAINER
        Item {
            id: textContainer
            anchors.fill: parent
            opacity: Backend.displayMode !== "osd" ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 150 } }

            property string lastMode: ""
            property string currentText: {
                if (Backend.displayMode === "notification") return Backend.summary;
                if (Backend.displayMode === "privacy") return Backend.privacySummary;
                return "";
            }

            Row {
                anchors.centerIn: parent
                spacing: privacyIndicator.visible ? 8 : 0

                Rectangle {
                    id: privacyIndicator
                    visible: Backend.displayMode === "privacy" || (island.state === "hidden" && textContainer.lastMode === "privacy")
                    anchors.verticalCenter: parent.verticalCenter
                    width: 12; height: 12; radius: 6
                    color: Backend.privacyHasCam ? AppTheme.colorCam : AppTheme.colorMic
                }

                Item {
                    id: textClipBox
                    anchors.verticalCenter: parent.verticalCenter
                    property int maxAvailableWidth: AppTheme.pillWidth - 48 - (privacyIndicator.visible ? 20 : 0)
                    width: Math.min(Math.max(oldTextLabel.implicitWidth, newTextLabel.implicitWidth), maxAvailableWidth)
                    height: AppTheme.pillHeight
                    clip: true 

                    Text {
                        id: oldTextLabel
                        text: ""
                        width: parent.width; height: parent.height
                        horizontalAlignment: Text.AlignLeft; verticalAlignment: Text.AlignVCenter
                        color: AppTheme.fg; font.pixelSize: AppTheme.summarySize; font.bold: AppTheme.summaryBold
                        elide: Text.ElideRight; y: 0; opacity: 0
                    }

                    Text {
                        id: newTextLabel
                        text: textContainer.currentText
                        width: parent.width; height: parent.height
                        horizontalAlignment: Text.AlignLeft; verticalAlignment: Text.AlignVCenter
                        color: AppTheme.fg; font.pixelSize: AppTheme.summarySize; font.bold: AppTheme.summaryBold
                        elide: Text.ElideRight; y: 0; opacity: 1
                    }
                }
            }

            onCurrentTextChanged: {
                if (currentText === "") return; 

                lastMode = Backend.displayMode;

                if (island.state === "hidden") {
                    newTextLabel.text = currentText
                    oldTextLabel.text = currentText
                    newTextLabel.y = 0
                    newTextLabel.opacity = 1
                    return
                }

                oldTextLabel.text = newTextLabel.text
                oldTextLabel.y = 0
                oldTextLabel.opacity = 1

                newTextLabel.text = currentText
                newTextLabel.y = -parent.height
                newTextLabel.opacity = 0

                slideDownTransition.restart()
            }

            ParallelAnimation {
                id: slideDownTransition
                NumberAnimation { target: oldTextLabel; property: "y"; to: parent.height; duration: 250; easing.type: Easing.InCubic }
                NumberAnimation { target: oldTextLabel; property: "opacity"; to: 0; duration: 250 }
                NumberAnimation { target: newTextLabel; property: "y"; to: 0; duration: 250; easing.type: Easing.OutCubic }
                NumberAnimation { target: newTextLabel; property: "opacity"; to: 1; duration: 250 }
            }
        }
    }

    // ==========================================
    // EXPANDED VIEW
    // ==========================================
    Item {
        id: expandedView
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: expandedHeight 
        
        // THE FIX: Disables the view fully when faded out so invisible buttons don't trap clicks
        visible: opacity > 0
        opacity: (island.state === "expanded") ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }

        // NOTIFICATION CONTENT
        Column {
            id: notifColumn
            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
            anchors.margins: 16
            spacing: 6
            visible: Backend.displayMode === "notification"

            Text {
                text: Backend.summary
                color: AppTheme.fg
                font.pixelSize: AppTheme.summarySize
                font.bold: AppTheme.summaryBold
                width: parent.width; elide: Text.ElideRight
            }

            Text {
                text: Backend.body
                color: AppTheme.fg; opacity: 0.8
                font.pixelSize: AppTheme.bodySize
                wrapMode: Text.WordWrap; width: parent.width
                maximumLineCount: 3; elide: Text.ElideRight
                visible: text.length > 0
            }

            Column {
                width: parent.width
                spacing: 8; topPadding: 10
                visible: Backend.hasActions

                Repeater {
                    model: Backend.actions
                    delegate: Rectangle {
                        width: parent.width; height: 38
                        radius: AppTheme.pillActionRadius
                        color: actionMouse.pressed ? AppTheme.pillActionBgHover : AppTheme.pillActionBg
                        border.color: AppTheme.pillActionBorder; border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: "white"
                            font.pixelSize: AppTheme.bodySize
                            font.bold: true
                        }

                        MouseArea {
                            id: actionMouse; anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: { 
                                Backend.invokeAction(modelData.id) 
                                island.pendingReadyForNext = true
                                island.state = "pill"
                            }
                        }
                    }
                }
            }
        }

        // PRIVACY DASHBOARD (SINGLE APP)
        Column {
            id: privacySingleColumn
            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
            anchors.margins: 16; spacing: 16
            visible: Backend.displayMode === "privacy" && Backend.privacyApps.length === 1

            Item {
                width: parent.width; height: childrenRect.height
                property var appData: Backend.privacyApps.length === 1 ? Backend.privacyApps[0] : null
                
                Column {
                    width: parent.width; spacing: 12
                    
                    SystemIcon {
                        anchors.horizontalCenter: parent.horizontalCenter
                        iconName: parent.parent.appData ? (parent.parent.appData.hasCam && parent.parent.appData.hasMic ? "camera-web-symbolic" : (parent.parent.appData.hasCam ? "video-display-symbolic" : "audio-input-microphone-symbolic")) : ""
                        iconColor: parent.parent.appData ? (parent.parent.appData.hasCam ? AppTheme.colorCam : AppTheme.colorMic) : "white"
                        size: 32 
                    }

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: parent.parent.appData ? (parent.parent.appData.name + " is using your " + (parent.parent.appData.hasCam && parent.parent.appData.hasMic ? "mic & camera" : (parent.parent.appData.hasCam ? "camera" : "microphone"))) : ""
                        color: AppTheme.fg
                        font.pixelSize: AppTheme.summarySize
                        font.bold: AppTheme.summaryBold
                    }

                    Column {
                        width: parent.width; spacing: 8; topPadding: 10

                        Rectangle {
                            width: parent.width; height: 38; radius: AppTheme.pillActionRadius
                            color: killSingleMouse.pressed ? AppTheme.pillActionBgHover : AppTheme.pillActionBg
                            border.color: AppTheme.pillActionBorder; border.width: 1

                            Text { anchors.centerIn: parent; text: parent.parent.parent.parent.appData ? "Kill " + parent.parent.parent.parent.appData.name : "Kill"; color: AppTheme.colorKill; font.pixelSize: AppTheme.bodySize; font.bold: true }
                            MouseArea { id: killSingleMouse; anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: island.requestPrivacyAction("kill", parent.parent.parent.parent.appData.pid, parent.parent.parent.parent.appData.name) }
                        }

                        Rectangle {
                            width: parent.width; height: 38; radius: AppTheme.pillActionRadius
                            color: ignoreSingleMouse.pressed ? AppTheme.pillActionBgHover : AppTheme.pillActionBg
                            border.color: AppTheme.pillActionBorder; border.width: 1

                            Text { anchors.centerIn: parent; text: "Ignore"; color: "white"; font.pixelSize: AppTheme.bodySize; font.bold: true }
                            MouseArea { id: ignoreSingleMouse; anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: island.requestPrivacyAction("ignore", parent.parent.parent.parent.appData.pid, parent.parent.parent.parent.appData.name) }
                        }
                    }
                }
            }
        }

        // PRIVACY DASHBOARD (MULTI APP)
        Column {
            id: privacyMultiColumn
            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
            anchors.margins: 16; spacing: 16
            visible: Backend.displayMode === "privacy" && Backend.privacyApps.length > 1

            SystemIcon { anchors.horizontalCenter: parent.horizontalCenter; iconName: "security-high-symbolic"; iconColor: AppTheme.fg; size: 32 }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: Backend.privacySummary; color: AppTheme.fg; font.pixelSize: AppTheme.summarySize; font.bold: AppTheme.summaryBold }

            Column {
                width: parent.width; spacing: 4

                Repeater {
                    model: Backend.privacyApps
                    delegate: Rectangle {
                        width: parent.width; height: 42; radius: 8 
                        color: rowHover.containsMouse ? AppTheme.pillActionBg : "transparent"
                        
                        MouseArea { id: rowHover; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
                        RowLayout {
                            anchors.fill: parent; anchors.margins: 8; spacing: 8
                            Text { Layout.fillWidth: true; text: modelData.name; color: AppTheme.fg; font.pixelSize: AppTheme.bodySize; elide: Text.ElideRight }
                            SystemIcon { visible: modelData.hasMic; iconName: "audio-input-microphone-symbolic"; iconColor: AppTheme.colorMic; size: 20 }
                            SystemIcon { visible: modelData.hasCam; iconName: "camera-web-symbolic"; iconColor: AppTheme.colorCam; size: 20 }
                            Text { text: "Ignore"; color: ignoreMultiMouse.pressed ? AppTheme.accent : AppTheme.fg; font.pixelSize: 13; font.bold: true; MouseArea { id: ignoreMultiMouse; anchors.fill: parent; anchors.margins: -4; cursorShape: Qt.PointingHandCursor; onClicked: island.requestPrivacyAction("ignore", modelData.pid, modelData.name) } }
                            Text { text: "Kill"; color: killMultiMouse.pressed ? Qt.darker(AppTheme.colorKill, 1.2) : AppTheme.colorKill; font.pixelSize: 13; font.bold: true; Layout.leftMargin: 8; MouseArea { id: killMultiMouse; anchors.fill: parent; anchors.margins: -4; cursorShape: Qt.PointingHandCursor; onClicked: island.requestPrivacyAction("kill", modelData.pid, modelData.name) } }
                        }
                    }
                }
            }

            Rectangle {
                width: parent.width; height: 38; radius: AppTheme.pillActionRadius
                color: killAllMouse.pressed ? AppTheme.pillActionBgHover : AppTheme.pillActionBg
                border.color: AppTheme.pillActionBorder; border.width: 1

                Text { anchors.centerIn: parent; text: "Kill All"; color: AppTheme.colorKill; font.pixelSize: AppTheme.bodySize; font.bold: true }
                MouseArea { id: killAllMouse; anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: island.requestPrivacyAction("killAll", 0, "") }
            }
        }
    }

    // ==========================================
    // INTERACTION HANDLER
    // ==========================================
    MouseArea {
        anchors.fill: parent
        z: -1 
        onClicked: {
            if (Backend.displayMode === "osd") return; 

            if (island.state === "pill") {
                island.state = "expanded"
            } else if (island.state === "expanded") {
                if (Backend.displayMode === "notification" && !Backend.hasActions) {
                    Backend.invokeAction("default")
                    
                    island.pendingReadyForNext = true
                    island.state = "pill"
                } else {
                    island.state = "pill"
                }
            }
        }
    }

    states: [
        State { 
            name: "hidden"
            PropertyChanges { target: island; width: AppTheme.pillWidth; height: AppTheme.pillHeight; radius: AppTheme.pillRadius; opacity: 0; scale: 0.8 } 
        },
        State { 
            name: "pill"
            PropertyChanges { target: island; width: AppTheme.pillWidth; height: AppTheme.pillHeight; radius: AppTheme.pillRadius; opacity: 1; scale: 1.0 } 
        },
        State { 
            name: "expanded"
            PropertyChanges { target: island; width: AppTheme.expandedMinWidth; height: expandedHeight; radius: AppTheme.expandedRadius; opacity: 1; scale: 1.0 } 
        }
    ]

    transitions: [
        Transition {
            to: "pill"
            SequentialAnimation {
                ScriptAction { script: { if (island.state !== "hidden") island.startTimer() } }
                ParallelAnimation {
                    NumberAnimation { target: island; properties: "width,height,radius,opacity,scale"; duration: 400; easing.type: Easing.OutBack; easing.overshoot: 1.2 }
                    NumberAnimation { targets: [pillView, expandedView]; property: "opacity"; duration: 300; easing.type: Easing.InOutQuad }
                }
                ScriptAction { 
                    script: { 
                        if (island.pendingReadyForNext) {
                            island.pendingReadyForNext = false;
                            Backend.readyForNext();
                        }
                    }
                }
            }
        },
        Transition {
            to: "expanded"
            ParallelAnimation {
                NumberAnimation { target: island; properties: "width,height,radius"; duration: 450; easing.type: Easing.OutBack; easing.overshoot: 1.1 }
                NumberAnimation { targets: [pillView, expandedView]; property: "opacity"; duration: 300; easing.type: Easing.InOutQuad }
            }
        },
        Transition {
            from: "expanded"
            to: "hidden"
            SequentialAnimation {
                ParallelAnimation {
                    NumberAnimation { target: island; properties: "width"; to: AppTheme.pillWidth; duration: 300; easing.type: Easing.OutExpo }
                    NumberAnimation { target: island; properties: "height"; to: AppTheme.pillHeight; duration: 300; easing.type: Easing.OutExpo }
                    NumberAnimation { target: island; properties: "radius"; to: AppTheme.pillRadius; duration: 300; easing.type: Easing.OutExpo }
                    NumberAnimation { target: island; properties: "opacity"; to: 0; duration: 250; easing.type: Easing.InCubic }
                    NumberAnimation { target: island; properties: "scale"; to: 0.8; duration: 250; easing.type: Easing.InCubic }
                    NumberAnimation { targets: [pillView, expandedView]; property: "opacity"; duration: 250; easing.type: Easing.InCubic }
                }
                ScriptAction {
                    script: {
                        if (island.pendingPrivacyAction) {
                            island.pendingPrivacyAction = false;
                            if (island.pendingPrivacyType === "kill") Backend.killPrivacyApp(island.pendingPrivacyPid, island.pendingPrivacyName);
                            else if (island.pendingPrivacyType === "ignore") Backend.ignorePrivacyApp(island.pendingPrivacyPid, island.pendingPrivacyName);
                            else if (island.pendingPrivacyType === "killAll") Backend.killAllPrivacyApps();
                        }
                    }
                }
            }
        },
        Transition {
            from: "pill"
            to: "hidden"
            NumberAnimation { target: island; properties: "opacity,scale"; duration: 250; easing.type: Easing.InCubic }
        }
    ]
}