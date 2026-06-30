system_state = {
    "environment": {
        "temperature": None,
        "humidity": None,
        "motion": None,
        "light": None,
        "last_seen": None
    },
    "window": {
        "state": None,
        "last_seen": None
    },
    "actuator": {
        "lighting": "off",
        "heating": "off",
        "last_seen": None,
        "lighting_override_until": None,
        "heating_override_until": None
    }
}

def update_environment(temp, humidity, motion, light, last_seen):
    environment = system_state["environment"]
    environment["temperature"] = temp
    environment["humidity"] = humidity
    environment["motion"] = motion
    environment["light"] = light
    environment["last_seen"] = last_seen

def update_window(state, last_seen):
    window = system_state["window"]
    window["state"] = state
    window["last_seen"] = last_seen

def update_actuator(lighting, heating, last_seen):
    actuator = system_state["actuator"]
    actuator["lighting"] = lighting
    actuator["heating"] = heating
    actuator["last_seen"] = last_seen