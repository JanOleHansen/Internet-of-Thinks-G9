from datetime import *
'''
This file contains the rule engine:
All automated rules that the hub should check on the received
sensor data, are described here. The actions should be implemented
in hub_control.py
'''
# Returns true if command should be sent to turn on the light
# Returns false if command should be sent to turn off the light
def checkLight(light, motion, on_time: datetime, now_time: datetime):
    if light == False:
        if motion == True:
            return True
        return None
    if on_time is None:
        return None
    timediff = now_time - on_time
    # Turn off light after 30 seconds of no motion
    # on_time is always resetted in hub_control.py, if motion was detected
    if timediff.total_seconds() > 30:
        return False
    return None # no command

# Returns true, if command should be sent to turn on the heating
# Returns false, if command should be sent to turn off the heating
def checkHeating(temp, window):
    if window == "open":
        return False
    if temp > 30.0:
        return False
    elif temp < 30.0:
        return True
    return None # no command
    
# Returns true, if command should be sent to open the window
# Returns false, if command should be sent to close the window
def checkWindow(window, temp, humidity):
    if humidity > 70 and window == "close":
        return True
    if humidity < 40 and window == "open":
        return False
    if temp < 18.0 and window == "open":
        return False
    return None # no command
