"use strict";

function initialize() {
    const public_network = document.querySelector("#public-network");
    public_network.addEventListener('change', (event) => {
        fetch("/settings/public-network", { method: "POST", body: "value=" + event.target.checked });
    });
}

window.onload = (event) => {
    initialize();
};