"use strict";

function initialize() {
    document.querySelector("#public-network").addEventListener('change', (event) => {
        fetch("/settings/public-network", { method: "POST", body: "value=" + event.target.checked });
    });
    document.querySelector("#host").addEventListener('change', (event) => {
        fetch("/settings/host-set", { method: "POST", body: "value=" + event.target.value });
    });
}

window.onload = (event) => {
    initialize();
};