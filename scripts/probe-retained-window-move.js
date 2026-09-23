// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Load ONLY into the disposable private KWin used by the retained-output probe.
// An explicit title prevents touching an unrelated desktop window.

const marker = "KRDP-MONITOR-PROBE";
const target = workspace.screens.find(screen => screen.name === "Virtual-1");
console.info("krdp-monitor-probe: outputs=" + workspace.screens.map(screen =>
    screen.name + "@" + screen.geometry.x + "," + screen.geometry.y + ":"
    + screen.geometry.width + "x" + screen.geometry.height).join(";"));

function inspect(window) {
    if (window.normalWindow) console.info("krdp-monitor-probe: candidate=" + window.caption);
    if (!window.caption.includes(marker)) return;
    console.info("krdp-monitor-probe: before caption=" + window.caption
        + " output=" + (window.output ? window.output.name : "none")
        + " geometry=" + JSON.stringify(window.frameGeometry)
        + " movable=" + window.moveableAcrossScreens);
    if (!target || !window.normalWindow || !window.moveableAcrossScreens) {
        console.warn("krdp-monitor-probe: target/window unavailable; no move attempted");
        return;
    }
    window.outputChanged.connect(() => console.info("krdp-monitor-probe: outputChanged="
        + (window.output ? window.output.name : "none")
        + " geometry=" + JSON.stringify(window.frameGeometry)));
    workspace.sendClientToScreen(window, target);
    console.info("krdp-monitor-probe: after output=" + (window.output ? window.output.name : "none")
        + " geometry=" + JSON.stringify(window.frameGeometry));
}

workspace.windowList().forEach(inspect);
workspace.windowAdded.connect(inspect);
workspace.cursorPosChanged.connect(() => console.info("krdp-monitor-probe: cursor="
    + workspace.cursorPos.x + "," + workspace.cursorPos.y));
