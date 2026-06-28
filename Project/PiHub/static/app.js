async function updateDashboard() {
  const res = await fetch("/api/state");
  const state = await res.json();

  document.getElementById("temperature").innerText =
    "Temperature: " + state.environment.temperature + " °C";

  document.getElementById("humidity").innerText =
    "Humidity: " + state.environment.humidity + " %";

  document.getElementById("light").innerText =
    "Light: " + state.environment.light;

  document.getElementById("motion").innerText =
    "Motion: " + state.environment.motion;

  document.getElementById("window").innerText =
    "Window: " + state.window.state;

  document.getElementById("lighting").innerText = state.actuator.lighting;
  document.getElementById("heating").innerText = state.actuator.heating;

  updateHeartbeat(state.environment.last_seen, "env-status");
  updateHeartbeat(state.window.last_seen, "window-status");
  updateHeartbeat(state.actuator.last_seen, "act-status");
}

function updateHeartbeat(lastSeen, ledId) {

    const led = document.getElementById(ledId);

    if (!lastSeen) {

        led.classList.remove("online");
        led.classList.add("offline");
        return;
    }

    const last = new Date(lastSeen);

    const now = new Date();

    const diff = (now - last) / 1000;

    if (diff <= 15) {

        led.classList.remove("offline");
        led.classList.add("online");

    } else {

        led.classList.remove("online");
        led.classList.add("offline");
    }

}

setInterval(updateDashboard, 1000);
updateDashboard();