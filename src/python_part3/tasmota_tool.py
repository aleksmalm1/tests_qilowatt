#!/usr/bin/env python3
# tasmota_tool.py — Tasmota testing & provisioning CLI tool
#
# Discovers Tasmota devices via mDNS, configures WiFi/MQTT via HTTP API,
# validates sensor payloads over MQTT, and generates a JSON test report.

import argparse
import json
import socket
import time
from datetime import datetime, timezone
from typing import Optional

import requests
from zeroconf import Zeroconf, ServiceBrowser, ServiceListener
import paho.mqtt.client as mqtt


# ── 1. Discovery ────────────────────────────────────────────────────────────

class TasmotaListener(ServiceListener):
    """Collects IPv4 addresses from mDNS — no blocking work in callbacks."""

    def __init__(self):
        self.ips = set()

    def add_service(self, zc, stype, name):
        info = zc.get_service_info(stype, name)
        if not info:
            return
        for addr in info.addresses:
            if len(addr) == 4:  # IPv4 only; inet_ntoa crashes on IPv6
                self.ips.add(socket.inet_ntoa(addr))

    def remove_service(self, *a): pass
    def update_service(self, *a): pass


# HTTP GET with optional Tasmota WebUI authentication
def http_get_json(ip, path, *, params=None, timeout=3,
                  web_user=None, web_pass=None):
    url = f"http://{ip}{path}"
    params = dict(params or {})
    if web_user and web_pass:
        params["user"] = web_user
        params["password"] = web_pass
    r = requests.get(url, params=params, timeout=timeout)
    r.raise_for_status()
    return r.json()


# verify an IP is a Tasmota device via Status 0
def probe_tasmota(ip, *, web_user=None, web_pass=None):
    try:
        data = http_get_json(ip, "/cm", params={"cmnd": "Status 0"},
                             timeout=2, web_user=web_user, web_pass=web_pass)
        if not isinstance(data, dict) or "StatusNET" not in data or "Status" not in data:
            return None
        host = data.get("StatusNET", {}).get("Hostname", ip)
        topic = data.get("Status", {}).get("Topic", None)
        return {"ip": ip, "hostname": host, "topic": topic}
    except Exception:
        return None


# discover Tasmota devices via mDNS, then probe each candidate
def discover(timeout=5.0, *, web_user=None, web_pass=None):
    print(f"[*] mDNS scan ({timeout}s) ...")
    zc = Zeroconf()
    listener = TasmotaListener()
    browser = ServiceBrowser(zc, "_http._tcp.local.", listener)
    try:
        time.sleep(timeout)
    finally:
        try:
            browser.cancel()
        except Exception:
            pass
        zc.close()

    print(f"[*] Found {len(listener.ips)} candidate IP(s), probing ...")
    devices = []
    for ip in sorted(listener.ips):
        dev = probe_tasmota(ip, web_user=web_user, web_pass=web_pass)
        if dev:
            devices.append(dev)
    return devices


# ── 2. Configuration ────────────────────────────────────────────────────────

# send a single Tasmota command via the HTTP API
def cmd(ip, command, *, web_user=None, web_pass=None):
    return http_get_json(ip, "/cm", params={"cmnd": command},
                         timeout=6, web_user=web_user, web_pass=web_pass)


# poll Status 0 until the device responds or timeout expires
def wait_for_device(ip, timeout=30, *, web_user=None, web_pass=None):
    print(f"[*] Waiting for {ip} to come back (up to {timeout}s) ...")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if probe_tasmota(ip, web_user=web_user, web_pass=web_pass):
            print(f"[✓] Device {ip} is back online")
            return True
        time.sleep(1)
    print(f"[!] Device {ip} did not respond within {timeout}s")
    return False


# configure WiFi and MQTT via individual HTTP commands
# commands sent individually (not Backlog) because Backlog splits on ';'
# before parsing — values containing ';' would silently corrupt config
# spaces are handled by URL encoding (requests library)
def configure(ip, ssid, wifi_pass, mqtt_host, mqtt_port,
              mqtt_user, mqtt_pass, topic, *, web_user=None, web_pass=None):
    print(f"[*] Configuring {ip} ...")
    res = {}
    res["SSID1"]     = cmd(ip, f"SSID1 {ssid}", web_user=web_user, web_pass=web_pass)
    res["Password1"] = cmd(ip, f"Password1 {wifi_pass}", web_user=web_user, web_pass=web_pass)
    res["MqttHost"]  = cmd(ip, f"MqttHost {mqtt_host}", web_user=web_user, web_pass=web_pass)
    res["MqttPort"]  = cmd(ip, f"MqttPort {mqtt_port}", web_user=web_user, web_pass=web_pass)
    if mqtt_user:
        res["MqttUser"] = cmd(ip, f"MqttUser {mqtt_user}", web_user=web_user, web_pass=web_pass)
    if mqtt_pass:
        res["MqttPassword"] = cmd(ip, f"MqttPassword {mqtt_pass}", web_user=web_user, web_pass=web_pass)
    if topic:
        res["Topic"] = cmd(ip, f"Topic {topic}", web_user=web_user, web_pass=web_pass)
    res["TelePeriod"] = cmd(ip, "TelePeriod 10", web_user=web_user, web_pass=web_pass)
    print("[✓] Configuration applied")

    # redact secrets before returning
    res["Password1"] = "***"
    if "MqttPassword" in res:
        res["MqttPassword"] = "***"
    return res


# ── 3. Validation ────────────────────────────────────────────────────────────

REQUIRED_KEYS = ["Temperature", "Humidity", "Pressure"]


# validate a Tasmota SENSOR payload against expected CustomSensor format
def validate_payload(payload):
    errors = []
    cs = payload.get("CustomSensor")
    if cs is None:
        return ["Missing 'CustomSensor' key"]
    for k in REQUIRED_KEYS:
        v = cs.get(k)
        if v is None:
            errors.append(f"Missing '{k}'")
        elif not isinstance(v, (int, float)):
            errors.append(f"'{k}' not numeric: {v!r}")
    return errors


# subscribe to tele/<topic>/SENSOR and validate each message
def subscribe_and_validate(mqtt_host, mqtt_port, topic, duration,
                           mqtt_user, mqtt_pass):
    results = []
    sub_topic = f"tele/{topic}/SENSOR"

    def on_connect(client, ud, flags, rc, props=None):
        if rc == 0:
            print(f"[*] Subscribed to {sub_topic}")
            client.subscribe(sub_topic)
        else:
            print(f"[!] MQTT connect failed rc={rc}")

    def on_message(client, ud, msg):
        ts = datetime.now(timezone.utc).isoformat()
        try:
            payload = json.loads(msg.payload)
        except Exception as e:
            results.append({"ts": ts, "valid": False, "errors": [str(e)]})
            return
        errs = validate_payload(payload)
        results.append({"ts": ts, "valid": len(errs) == 0, "errors": errs, "payload": payload})

    # paho-mqtt 2.x uses CallbackAPIVersion; fall back for 1.x
    try:
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except Exception:
        c = mqtt.Client()

    if mqtt_user:
        c.username_pw_set(mqtt_user, mqtt_pass)
    c.on_connect = on_connect
    c.on_message = on_message

    print(f"[*] Listening on MQTT {mqtt_host}:{mqtt_port} for {duration}s ...")
    c.connect(mqtt_host, mqtt_port)
    c.loop_start()
    time.sleep(duration)
    c.loop_stop()
    c.disconnect()
    return results


# ── 4. Report ────────────────────────────────────────────────────────────────

# generate JSON test report with pass/fail summary
def generate_report(devices, config_res, validation, path):
    passed = sum(1 for v in validation if v["valid"])
    report = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "summary": {
            "devices_found": len(devices),
            "messages_received": len(validation),
            "valid": passed,
            "invalid": len(validation) - passed,
            "pass": passed > 0 and passed == len(validation),
        },
        "devices": devices,
        "configuration": config_res,
        "validation": validation,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"[✓] Report saved to {path}")
    return report


# ── CLI ──────────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(description="Tasmota test & provisioning tool")
    p.add_argument("--device-ip",    help="skip discovery, provision this IP directly")
    p.add_argument("--web-user",     default=None, help="Tasmota WebUI username")
    p.add_argument("--web-pass",     default=None, help="Tasmota WebUI password")
    p.add_argument("--ssid",         required=True, help="WiFi SSID to configure")
    p.add_argument("--wifi-pass",    required=True, help="WiFi password to configure")
    p.add_argument("--mqtt-host",    required=True, help="MQTT broker address")
    p.add_argument("--mqtt-port",    type=int, default=1883)
    p.add_argument("--mqtt-user",    default="")
    p.add_argument("--mqtt-pass",    default="")
    p.add_argument("--mqtt-topic",   default=None, help="device topic (auto-detected if omitted)")
    p.add_argument("--duration",     type=float, default=30, help="MQTT listen duration in seconds")
    p.add_argument("--output",       default="report.json", help="report output path")
    p.add_argument("--mdns-timeout", type=float, default=5.0)
    args = p.parse_args()

    # 1. discover
    if args.device_ip:
        print(f"[*] Using provided device IP: {args.device_ip}")
        dev = probe_tasmota(args.device_ip, web_user=args.web_user, web_pass=args.web_pass)
        devices = [dev] if dev else []
        if not dev:
            print(f"[!] {args.device_ip} did not respond as a Tasmota device")
    else:
        devices = discover(args.mdns_timeout, web_user=args.web_user, web_pass=args.web_pass)

    if not devices:
        print("[!] No devices found")
        return

    print(f"[✓] {len(devices)} device(s) found")

    # resolve MQTT topic: explicit argument or read from device
    topic = args.mqtt_topic or devices[0].get("topic")
    if not topic:
        print("[!] Could not determine topic; pass --mqtt-topic explicitly")
        return
    print(f"[*] Using topic: {topic}")

    # 2. configure
    target = devices[0]["ip"]
    config_res = configure(
        target, args.ssid, args.wifi_pass,
        args.mqtt_host, args.mqtt_port, args.mqtt_user, args.mqtt_pass,
        topic, web_user=args.web_user, web_pass=args.web_pass,
    )

    if not wait_for_device(target, web_user=args.web_user, web_pass=args.web_pass):
        print("[!] Proceeding anyway ...")

    # 3. validate
    validation = subscribe_and_validate(
        args.mqtt_host, args.mqtt_port, topic,
        args.duration, args.mqtt_user, args.mqtt_pass,
    )

    # 4. report
    report = generate_report(devices, config_res, validation, args.output)
    status = "PASS ✓" if report["summary"]["pass"] else "FAIL ✗"
    print(f"\nResult: {status}  ({report['summary']['valid']}/{report['summary']['messages_received']} valid)")


if __name__ == "__main__":
    main()
