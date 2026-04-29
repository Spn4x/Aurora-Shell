import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import Aurora.Shell 

Rectangle {
    id: island
    state: "hidden" 
    
    property bool pendingReadyForNext: false
    property bool physicsReady: false 

    ListModel { id: privacyAppModel }

    function syncPrivacyModel() {
        let newApps = Backend.privacyApps;
        for (let i = privacyAppModel.count - 1; i >= 0; i--) {
            let pid = privacyAppModel.get(i).pid;
            let name = privacyAppModel.get(i).name;
            let found = false;
            for (let j = 0; j < newApps.length; j++) {
                if (newApps[j].pid === pid && newApps[j].name === name) { found = true; break; }
            }
            if (!found) privacyAppModel.remove(i);
        }
        for (let i = 0; i < newApps.length; i++) {
            let newApp = newApps[i];
            let found = false;
            for (let j = 0; j < privacyAppModel.count; j++) {
                if (privacyAppModel.get(j).pid === newApp.pid && privacyAppModel.get(j).name === newApp.name) {
                    privacyAppModel.setProperty(j, "hasMic", newApp.hasMic);
                    privacyAppModel.setProperty(j, "hasCam", newApp.hasCam);
                    found = true; break;
                }
            }
            if (!found) privacyAppModel.insert(i, newApp);
        }
    }

    Component.onCompleted: {
        syncPrivacyModel()
        startupPhysicsTimer.start() 
    }

    Timer {
        id: startupPhysicsTimer
        interval: 150
        onTriggered: island.physicsReady = true
    }

    property int animatedExpandedHeight: {
        if (Backend.displayMode === "notification") return notifColumn.implicitHeight + 32
        if (Backend.displayMode === "media") return mediaComponent.expandedImplicitHeight + 32
        if (Backend.displayMode === "privacy") {
            if (Backend.privacyApps.length === 1) return privacySingleColumn.implicitHeight + 32
            if (Backend.privacyApps.length > 1) return privacyMultiColumn.implicitHeight + 32
        }
        return AppTheme.expandedMinHeight
    }
    
    property int dynamicWidth: {
        if (state === "hidden") return AppTheme.pillWidth;
        if (state === "expanded") return AppTheme.expandedMinWidth;
        if (Backend.displayMode === "media" && Backend.mediaPinned) {
            return Math.min(Math.max(AppTheme.pillWidth, mediaPillComponent.pinnedContentWidth + 64), 800);
        }
        return AppTheme.pillWidth;
    }

    width: dynamicWidth
    height: state === "expanded" ? animatedExpandedHeight : AppTheme.pillHeight
    radius: state === "expanded" ? AppTheme.expandedRadius : AppTheme.pillRadius
    
    Behavior on width { NumberAnimation { duration: 350; easing.type: Easing.OutCubic } }
    Behavior on height { NumberAnimation { duration: 350; easing.type: Easing.OutCubic } }
    Behavior on radius { NumberAnimation { duration: 350; easing.type: Easing.OutCubic } }

    // ==========================================
    // FLAWLESS NATURAL PHYSICS SYSTEM
    // ==========================================
    
    property real targetCenterX: parent ? parent.width / 2 : Screen.width / 2
    property real targetY: 10

    property real physicsCenterX: targetCenterX
    property real physicsY: targetY

    Behavior on physicsCenterX { 
        enabled: physicsReady 
        SpringAnimation { spring: 2.2; damping: 0.2; mass: 1.5; epsilon: 0.01 } 
    }
    
    Behavior on physicsY { 
        enabled: physicsReady 
        SpringAnimation { spring: 2.2; damping: 0.2; mass: 1.5; epsilon: 0.01 } 
    }

    x: physicsCenterX - width / 2
    y: physicsY

    Timer {
        id: snapBackTimer
        interval: 5000 
        onTriggered: {
            island.targetCenterX = Qt.binding(function() { return island.parent ? island.parent.width / 2 : Screen.width / 2 })
            island.targetY = 10
        }
    }

    // ==========================================

    opacity: 0
    scale: 0.8
    color: AppTheme.bg
    border.color: Backend.mediaPinned && Backend.displayMode === "media" ? AppTheme.accent : AppTheme.borderAlpha
    border.width: 2 
    clip: true

    function startTimer() {
        autoDismissTimer.stop()
        
        if (Backend.mediaPinned && Backend.displayMode === "media" && Backend.mediaStatus === "Playing") {
            return;
        }

        if (Backend.displayMode === "osd") {
            autoDismissTimer.interval = 1500
        } else if (island.state === "expanded") {
            autoDismissTimer.interval = Backend.hasActions ? 12000 : 8000
        } else if (Backend.displayMode === "media") {
            autoDismissTimer.interval = 6000 
        } else {
            autoDismissTimer.interval = 4000
        }
        autoDismissTimer.restart()
    }

    function requestPrivacyAction(type, pid, name) {
        if (type === "killAll") Backend.killAllPrivacyApps();
        else if (type === "kill") Backend.killPrivacyApp(pid, name);
        else if (type === "ignore") Backend.ignorePrivacyApp(pid, name);
    }

    onStateChanged: {
        Backend.isExpanded = (state === "expanded"); 
        
        if (state === "pill" || state === "expanded") {
            island.startTimer()
        } else if (state === "hidden") {
            island.targetCenterX = Qt.binding(function() { return island.parent ? island.parent.width / 2 : Screen.width / 2 })
            island.targetY = 10
        }
    }

    Timer {
        id: autoDismissTimer
        onTriggered: {
            if (island.state === "expanded") {
                island.pendingReadyForNext = true;
                island.state = "pill";
            } else if (island.state === "pill") {
                if (Backend.displayMode === "notification" || Backend.displayMode === "osd" || Backend.displayMode === "media") {
                    Backend.readyForNext();
                }
            }
        }
    }

    Connections {
        target: Backend
        function onRequestShow() { 
            if (island.state === "hidden") {
                island.state = "pill";
            }
            island.startTimer() 
        }
        function onRequestHide() { 
            island.state = "hidden" 
            autoDismissTimer.stop()
        }
        function onPrivacyChanged() {
            island.syncPrivacyModel()
        }
        function onMediaChanged() {
            if (Backend.mediaStatus !== "Playing" && Backend.mediaPinned && island.state === "expanded") {
                island.startTimer()
            } else if (Backend.mediaStatus === "Playing" && Backend.mediaPinned) {
                autoDismissTimer.stop()
            }
        }
        function onDisplayModeChanged() {
            if (Backend.displayMode === "notification") {
                island.state = "pill"; 
                island.startTimer();
            } else if (Backend.displayMode === "media" && Backend.mediaPinned) {
                island.state = "pill"; 
            } else if (Backend.displayMode === "privacy") {
                if (island.state === "hidden") island.state = "pill";
            }
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
        
        visible: opacity > 0
        opacity: (island.state === "pill") ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150; easing.type: Easing.OutQuad } }

        Item {
            id: osdContainer
            anchors.fill: parent
            opacity: Backend.displayMode === "osd" ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 150 } }

            Row {
                anchors.centerIn: parent
                spacing: 14

                SystemIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: Backend.osdIcon
                    iconColor: AppTheme.fg
                    size: 26 
                }

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 200 
                    height: 10 
                    radius: 5
                    color: Qt.rgba(1, 1, 1, 0.2)

                    Rectangle {
                        width: Math.min(Backend.osdLevel, 1.0) * parent.width
                        height: parent.height
                        radius: 5
                        color: AppTheme.fg
                        Behavior on width { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                    }
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: Backend.osdLevel > 1.0
                    text: "+" + Math.round((Backend.osdLevel - 1.0) * 100) + "%"
                    color: AppTheme.fg
                    font.pixelSize: 13
                    font.bold: true
                }
            }
        }

        Item {
            id: textContainer
            anchors.fill: parent
            opacity: (Backend.displayMode === "notification" || Backend.displayMode === "privacy") ? 1 : 0
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
        
        Player {
            id: mediaPillComponent
            anchors.fill: parent
            isExpanded: false
            opacity: Backend.displayMode === "media" ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 150 } }
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
        
        height: animatedExpandedHeight 
        
        visible: opacity > 0
        opacity: (island.state === "expanded") ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }

        Column {
            id: notifColumn
            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
            anchors.margins: 16
            spacing: 6
            
            opacity: Backend.displayMode === "notification" ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 200 } }

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
                                if (island.pendingReadyForNext) return;
                                Backend.invokeAction(modelData.id) 
                                island.pendingReadyForNext = true
                                island.state = "pill"
                            }
                        }
                    }
                }
            }
        }

        Column {
            id: privacySingleColumn
            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
            anchors.margins: 16; spacing: 16
            
            opacity: (Backend.displayMode === "privacy" && Backend.privacyApps.length === 1) ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 200 } }

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

        Column {
            id: privacyMultiColumn
            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
            anchors.margins: 16; spacing: 16
            
            opacity: (Backend.displayMode === "privacy" && Backend.privacyApps.length > 1) ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 200 } }

            SystemIcon { anchors.horizontalCenter: parent.horizontalCenter; iconName: "security-high-symbolic"; iconColor: AppTheme.fg; size: 32 }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: Backend.privacySummary; color: AppTheme.fg; font.pixelSize: AppTheme.summarySize; font.bold: AppTheme.summaryBold }

            Column {
                width: parent.width
                spacing: 0 

                Repeater {
                    model: privacyAppModel
                    delegate: Item {
                        id: delegateWrapper
                        width: parent.width
                        height: 46 
                        clip: true 

                        property int rowPid: pid
                        property string rowName: name
                        property bool isDismissing: false

                        opacity: 1
                        Behavior on opacity { NumberAnimation { duration: 150 } }
                        Behavior on height { NumberAnimation { duration: 250; easing.type: Easing.InOutQuad } }

                        Rectangle {
                            id: delegateRow
                            width: parent.width; height: 42; radius: 8 
                            y: 0
                            color: rowHover.containsMouse && !isDismissing ? AppTheme.pillActionBg : "transparent"
                            
                            MouseArea { id: rowHover; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
                            RowLayout {
                                anchors.fill: parent; anchors.margins: 8; spacing: 8
                                Text { Layout.fillWidth: true; text: rowName; color: AppTheme.fg; font.pixelSize: AppTheme.bodySize; elide: Text.ElideRight }
                                SystemIcon { visible: hasMic; iconName: "audio-input-microphone-symbolic"; iconColor: AppTheme.colorMic; size: 20 }
                                SystemIcon { visible: hasCam; iconName: "camera-web-symbolic"; iconColor: AppTheme.colorCam; size: 20 }
                                Text { 
                                    text: "Ignore"; color: ignoreMultiMouse.pressed ? AppTheme.accent : AppTheme.fg; font.pixelSize: 13; font.bold: true; 
                                    MouseArea { id: ignoreMultiMouse; anchors.fill: parent; anchors.margins: -4; cursorShape: Qt.PointingHandCursor; 
                                        onClicked: if(!delegateWrapper.isDismissing) delegateWrapper.dismissItem("ignore") 
                                    } 
                                }
                                Text { 
                                    text: "Kill"; color: killMultiMouse.pressed ? Qt.darker(AppTheme.colorKill, 1.2) : AppTheme.colorKill; font.pixelSize: 13; font.bold: true; Layout.leftMargin: 8; 
                                    MouseArea { id: killMultiMouse; anchors.fill: parent; anchors.margins: -4; cursorShape: Qt.PointingHandCursor; 
                                        onClicked: if(!delegateWrapper.isDismissing) delegateWrapper.dismissItem("kill") 
                                    } 
                                }
                            }
                        }

                        Timer {
                            id: actionTimer
                            interval: 150 
                            property string actionType
                            onTriggered: {
                                delegateWrapper.height = 0; 
                                shrinkTimer.start();
                            }
                        }

                        Timer {
                            id: shrinkTimer
                            interval: 250 
                            onTriggered: {
                                if (actionTimer.actionType === "kill") Backend.killPrivacyApp(rowPid, rowName)
                                else Backend.ignorePrivacyApp(rowPid, rowName)
                            }
                        }

                        function dismissItem(type) {
                            isDismissing = true;
                            delegateWrapper.opacity = 0;
                            actionTimer.actionType = type;
                            actionTimer.start();
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
        
        Player {
            id: mediaComponent
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            isExpanded: true
            opacity: Backend.displayMode === "media" ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 150 } }
        }
    }

// ==========================================
    // INTERACTION HANDLER
    // ==========================================
    MouseArea {
        id: mainInteractionArea
        anchors.fill: parent
        z: -1 
        
        // ACCEPT BOTH LEFT AND RIGHT CLICKS
        acceptedButtons: Qt.LeftButton | Qt.RightButton

        property real startGlobalX: 0
        property real startGlobalY: 0
        property real startTargetCenterX: 0
        property real startTargetY: 0
        property bool wasDragged: false

        onPressed: (mouse) => {
            if (mouse.button === Qt.RightButton) return;

            snapBackTimer.stop()
            wasDragged = false
            
            let globalPos = mapToItem(island.parent, mouse.x, mouse.y)
            startGlobalX = globalPos.x
            startGlobalY = globalPos.y
            startTargetCenterX = island.targetCenterX
            startTargetY = island.targetY
        }

        onPositionChanged: (mouse) => {
            if (!(mouse.buttons & Qt.LeftButton)) return;

            let globalPos = mapToItem(island.parent, mouse.x, mouse.y)
            let deltaX = globalPos.x - startGlobalX
            let deltaY = globalPos.y - startGlobalY

            if (Math.abs(deltaX) > 3 || Math.abs(deltaY) > 3) {
                wasDragged = true
            }

            if (wasDragged) {
                island.targetCenterX = startTargetCenterX + deltaX
                island.targetY = startTargetY + deltaY
            }
        }

        onReleased: (mouse) => {
            if (mouse.button === Qt.RightButton) return;
            snapBackTimer.restart()
        }

        onClicked: (mouse) => {
            if (wasDragged) return;

            // --- RIGHT CLICK DISMISS LOGIC ---
            if (mouse.button === Qt.RightButton) {
                // Cannot dismiss active privacy camera/mic warnings via right click!
                if (Backend.displayMode === "privacy") return; 

                // THE FIX: Unpin media so the backend allows it to be dismissed
                if (Backend.displayMode === "media") {
                    Backend.setMediaPinned(false);
                }

                // Smoothly dismiss Notifications, OSD, AND Media!
                if (island.state === "expanded") {
                    island.pendingReadyForNext = true;
                    island.state = "pill";
                } else {
                    Backend.readyForNext();
                }
                return;
            }

            // --- LEFT CLICK EXPAND/ACTION LOGIC ---
            if (Backend.displayMode === "osd") return; 

            if (island.state === "pill") {
                island.state = "expanded"
            } else if (island.state === "expanded") {
                if (Backend.displayMode === "notification" && !Backend.hasActions) {
                    if (island.pendingReadyForNext) return;
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
        State { name: "hidden"; PropertyChanges { target: island; opacity: 0; scale: 0.8 } },
        State { name: "pill"; PropertyChanges { target: island; opacity: 1; scale: 1.0 } },
        State { name: "expanded"; PropertyChanges { target: island; opacity: 1; scale: 1.0 } }
    ]

    transitions: [
        Transition {
            to: "pill"
            SequentialAnimation {
                ScriptAction { script: { if (island.state !== "hidden") island.startTimer() } }
                ParallelAnimation {
                    NumberAnimation { target: island; properties: "opacity,scale"; duration: 400; easing.type: Easing.OutBack; easing.overshoot: 1.2 }
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
                NumberAnimation { target: island; properties: "opacity,scale"; duration: 450; easing.type: Easing.OutBack; easing.overshoot: 1.1 }
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
            }
        },
        Transition {
            from: "pill"
            to: "hidden"
            NumberAnimation { target: island; properties: "opacity,scale"; duration: 250; easing.type: Easing.InCubic }
        }
    ]
}