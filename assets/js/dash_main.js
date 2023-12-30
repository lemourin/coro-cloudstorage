"use strict";

async function init() {
    const video = document.getElementById('video');
    const ui = video['ui'];
    const controls = ui.getControls();
    const player = controls.getPlayer();
    await player.load(video.dataset.src);
}

document.addEventListener('shaka-ui-loaded', init);
