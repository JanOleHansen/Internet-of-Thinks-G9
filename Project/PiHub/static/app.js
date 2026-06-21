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

  document.getElementById("heating").innerText =
    "Heating: " + state.actuator.heating;
}

setInterval(updateDashboard, 1000);
updateDashboard();