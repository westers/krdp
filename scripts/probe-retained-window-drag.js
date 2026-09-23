// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Observe, but never move, the marked Konsole in the disposable private KWin.
// The authenticated worker probe must perform the entire cross-output drag.

const marker = "KRDP-MONITOR-PROBE";
console.info("krdp-monitor-drag: outputs=" + workspace.screens.map(screen =>
    screen.name + "@" + screen.geometry.x + "," + screen.geometry.y).join(";"));

function inspect(window) {
    if (!window.caption.includes(marker)) return;
    console.info("krdp-monitor-drag: before output=" + (window.output ? window.output.name : "none")
        + " geometry=" + JSON.stringify(window.frameGeometry)
        + " movable=" + window.moveableAcrossScreens);
    window.outputChanged.connect(() => console.info("krdp-monitor-drag: outputChanged="
        + (window.output ? window.output.name : "none")
        + " geometry=" + JSON.stringify(window.frameGeometry)));
}

workspace.windowList().forEach(inspect);
workspace.windowAdded.connect(inspect);
