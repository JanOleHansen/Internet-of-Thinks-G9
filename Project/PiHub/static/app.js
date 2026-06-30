let cachedState = null;

async function updateDashboard() {
  const res = await fetch("/api/state");
  const state = await res.json();
  cachedState = state;

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
  document.getElementById("heating").innerText  = state.actuator.heating;

  updateOverride("lighting-override", state.actuator.lighting_override_until);
  updateOverride("heating-override",  state.actuator.heating_override_until);

  updateHeartbeat(state.environment.last_seen, "env-status");
  updateHeartbeat(state.window.last_seen,      "window-status");
  updateHeartbeat(state.actuator.last_seen,    "act-status");
}

function updateOverride(elementId, overrideUntil) {
  const el = document.getElementById(elementId);
  if (!overrideUntil) {
    el.innerText = "";
    return;
  }
  const remaining = Math.max(0, Math.floor((new Date(overrideUntil) - new Date()) / 1000));
  if (remaining <= 0) {
    el.innerText = "";
  } else {
    const mins = Math.floor(remaining / 60);
    const secs = (remaining % 60).toString().padStart(2, "0");
    el.innerText = `Manual override (${mins}:${secs})`;
  }
}

async function sendCommand(device, action) {
  try {
    const res = await fetch(`/api/command/${device}/${action}`, { method: "POST" });
    const data = await res.json();
    if (!data.success) {
      console.warn("Command failed:", data.error);
    }
    await updateDashboard();
  } catch (e) {
    console.error("Command error:", e);
  }
}

function updateHeartbeat(lastSeen, ledId) {
  const led = document.getElementById(ledId);
  if (!lastSeen) {
    led.classList.remove("online");
    led.classList.add("offline");
    return;
  }
  const diff = (new Date() - new Date(lastSeen)) / 1000;
  if (diff <= 15) {
    led.classList.remove("offline");
    led.classList.add("online");
  } else {
    led.classList.remove("online");
    led.classList.add("offline");
  }
}

document.getElementById("lighting-card").addEventListener("click", () => {
  if (!cachedState) return;
  const action = cachedState.actuator.lighting === "on" ? "off" : "on";
  sendCommand("lighting", action);
});

document.getElementById("heating-card").addEventListener("click", () => {
  if (!cachedState) return;
  const action = cachedState.actuator.heating === "on" ? "off" : "on";
  sendCommand("heating", action);
});

setInterval(updateDashboard, 1000);
updateDashboard();
