# customsensor_berry_script.be — alarm monitor for CustomSensor driver
#
# Monitors temperature, humidity, and pressure using a 5-sample moving average.
# Publishes MQTT alerts when thresholds are exceeded and logs events to flash
# via the persist module.

import json
import mqtt
import persist

# alarm thresholds
var TEMP_HI  = 30.0    # °C
var HUM_HI   = 80.0    # %RH
var PRESS_HI = 1030.0  # hPa

# alarm state flags (prevent duplicate alerts)
var temp_alarm  = false
var hum_alarm   = false
var press_alarm = false

# moving average window
var WIN = 5
var buf_t = []
var buf_h = []
var buf_p = []

# resolved lazily to avoid failure before MQTT connects
var topic = nil

def get_topic()
  if topic == nil
    try
      topic = "tele/" + tasmota.cmd("Topic", true)["Topic"] + "/ALERT"
    except .. as e, m
      tasmota.log("CS: topic resolve failed: " + str(m), 2)
    end
  end
  return topic
end

# push value into circular buffer, drop oldest if full
def push(buf, v)
  buf.push(v)
  if buf.size() > WIN
    buf.remove(0)
  end
end

# arithmetic mean of buffer
def avg(buf)
  var s = 0.0
  for x : buf
    s += x
  end
  return s / buf.size()
end

# log alarm event to flash (max 100 entries to prevent exhaustion)
def log_event(event, metric, raw, avg5)
  if persist.cs_log == nil
    persist.cs_log = []
  end

  persist.cs_log.push({
    "ts": tasmota.time_str(tasmota.rtc()["local"]),
    "event": event,
    "metric": metric,
    "raw": raw,
    "avg5": avg5
  })

  # rotate log to prevent flash exhaustion
  while persist.cs_log.size() > 100
    persist.cs_log.remove(0)
  end

  persist.dirty()
  persist.save()
end

# main polling function — called every 10 seconds via cron
def poll()
  try
    var cs = json.load(tasmota.read_sensors())["CustomSensor"]
    if cs == nil || cs.find("Error") != nil
      return
    end

    var t = cs["Temperature"]
    var h = cs["Humidity"]
    var p = cs["Pressure"]

    if t == nil || h == nil || p == nil
      return
    end

    push(buf_t, t)
    push(buf_h, h)
    push(buf_p, p)

    # wait until we have a full window before evaluating
    if buf_t.size() < WIN
      return
    end

    var t_avg5 = avg(buf_t)
    var h_avg5 = avg(buf_h)
    var p_avg5 = avg(buf_p)

    var tp = get_topic()

    # temperature alarm
    if (t_avg5 > TEMP_HI) && !temp_alarm
      temp_alarm = true
      if tp != nil
        mqtt.publish(tp, json.dump({"Alert": "TempHigh", "Temperature": t, "TempAvg5": t_avg5}))
      end
      log_event("TempHigh", "Temp", t, t_avg5)
      tasmota.log("CS: TempHigh avg=" + str(t_avg5), 2)
    elif (t_avg5 <= TEMP_HI) && temp_alarm
      temp_alarm = false
      log_event("TempNormal", "Temp", t, t_avg5)
      tasmota.log("CS: TempNormal avg=" + str(t_avg5), 2)
    end

    # humidity alarm
    if (h_avg5 > HUM_HI) && !hum_alarm
      hum_alarm = true
      if tp != nil
        mqtt.publish(tp, json.dump({"Alert": "HumHigh", "Humidity": h, "HumAvg5": h_avg5}))
      end
      log_event("HumHigh", "Hum", h, h_avg5)
      tasmota.log("CS: HumHigh avg=" + str(h_avg5), 2)
    elif (h_avg5 <= HUM_HI) && hum_alarm
      hum_alarm = false
      log_event("HumNormal", "Hum", h, h_avg5)
      tasmota.log("CS: HumNormal avg=" + str(h_avg5), 2)
    end

    # pressure alarm
    if (p_avg5 > PRESS_HI) && !press_alarm
      press_alarm = true
      if tp != nil
        mqtt.publish(tp, json.dump({"Alert": "PressHigh", "Pressure": p, "PressAvg5": p_avg5}))
      end
      log_event("PressHigh", "Press", p, p_avg5)
      tasmota.log("CS: PressHigh avg=" + str(p_avg5), 2)
    elif (p_avg5 <= PRESS_HI) && press_alarm
      press_alarm = false
      log_event("PressNormal", "Press", p, p_avg5)
      tasmota.log("CS: PressNormal avg=" + str(p_avg5), 2)
    end

  except .. as e, m
    tasmota.log("CS: poll error: " + str(e) + " " + str(m), 2)
  end
end

# remove existing cron entry before adding (safe for script reload)
try
  tasmota.remove_cron("cs10s")
except .. as e, m
end

tasmota.add_cron("*/10 * * * * *", poll, "cs10s")
