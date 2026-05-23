import os
import sys
import time
import requests

USERNAME = os.environ.get("AIO_USERNAME", "xivaniuk")
AIO_KEY = os.environ.get("AIO_KEY", "")

DASHBOARD_NAME = "Plant_Monitoring_Prj"
DASHBOARD_KEY = "plant-monitoring-prj"

BASE_URL = f"https://io.adafruit.com/api/v2/{USERNAME}"

HEADERS = {
    "X-AIO-Key": AIO_KEY,
    "Content-Type": "application/json",
}


FEED = {
    "moisture": "plant-monitoring-feed.plant-dot-moisture",
    "light": "plant-monitoring-feed.plant-dot-light",
    "pump_state": "plant-monitoring-feed.plant-dot-pump-state",
    "tank_state": "plant-monitoring-feed.plant-dot-tank-state",
    "used_water": "plant-monitoring-feed.plant-dot-used-water",
    "alert": "plant-monitoring-feed.plant-dot-alert",
    "pump_control": "plant-monitoring-feed.plant-dot-pump-control",
}


def check_key():
    if not AIO_KEY:
        print("ERROR: AIO_KEY environment variable is not set.")
        print("Run: AIO_USERNAME=xivaniuk AIO_KEY=your_key python setup_dashboard.py")
        sys.exit(1)


def api_get(path):
    r = requests.get(f"{BASE_URL}{path}", headers=HEADERS, timeout=10)
    r.raise_for_status()
    return r.json()


def api_post(path, payload):
    r = requests.post(f"{BASE_URL}{path}", headers=HEADERS, json=payload, timeout=10)
    if not r.ok:
        print(f"POST {path} - {r.status_code}: {r.text}")
        r.raise_for_status()
    return r.json()


def get_feed_id(feed_key):
    data = api_get(f"/feeds/{feed_key}")
    return data["id"]


def get_or_create_dashboard():
    dashboards = api_get("/dashboards")
    for d in dashboards:
        if d["key"] == DASHBOARD_KEY or d["name"] == DASHBOARD_NAME:
            print(f"Dashboard already exists (id={d['id']}), reusing it.")
            return d["key"]

    payload = {
        "dashboard": {
            "name": DASHBOARD_NAME,
            "description": "ESP32 plant monitor - soil moisture, light, pump control",
        }
    }
    data = api_post("/dashboards", payload)
    print(f"Dashboard created (id={data['id']}, key={data['key']})")
    return data["key"]


def create_block(dashboard_key, block_def):
    """
    POST /api/v2/{username}/dashboards/{dashboard_key}/blocks
    block_def must contain:
        visual_type, name, column, row, size_x, size_y,
        block_feeds (list of {feed_key}),
        properties (dict, visual_type-specific)
    """
    block_feeds = []
    for feed_key in block_def.pop("feed_keys"):
        feed_id = get_feed_id(feed_key)
        block_feeds.append({"feed_id": feed_id})

    payload = {
        "block": {
            "visual_type": block_def["visual_type"],
            "name": block_def["name"],
            "column": block_def["column"],
            "row": block_def["row"],
            "size_x": block_def["size_x"],
            "size_y": block_def["size_y"],
            "properties": block_def.get("properties", {}),
            "block_feeds": block_feeds,
        }
    }

    data = api_post(f"/dashboards/{dashboard_key}/blocks", payload)
    print(f"Block '{block_def['name']}' created (id={data['id']})")
    time.sleep(0.4)


def create_all_blocks(dashboard_key):

    blocks = [
        {
            "visual_type": "line_chart",
            "name": "Vlhkosť pôdy (%)",
            "column": 0,
            "row": 0,
            "size_x": 6,
            "size_y": 4,
            "feed_keys": [FEED["moisture"]],
            "properties": {
                "xAxisLabel": "Čas",
                "yAxisLabel": "Vlhkosť (%)",
                "yAxisMin": "0",
                "yAxisMax": "100",
                "historyHours": "24",
                "showYAxisGridLines": "true",
                "showXAxisGridLines": "true",
                "lineColors": "#2E7D9E",
            },
        },
        {
            "visual_type": "line_chart",
            "name": "Intenzita svetla (lux)",
            "column": 6,
            "row": 0,
            "size_x": 6,
            "size_y": 4,
            "feed_keys": [FEED["light"]],
            "properties": {
                "xAxisLabel": "Čas",
                "yAxisLabel": "Lux",
                "historyHours": "24",
                "showYAxisGridLines": "true",
                "showXAxisGridLines": "true",
                "lineColors": "#C47C00",
            },
        },
        {
            "visual_type": "indicator",
            "name": "Stav čerpadla",
            "column": 0,
            "row": 4,
            "size_x": 2,
            "size_y": 2,
            "feed_keys": [FEED["pump_state"]],
            "properties": {
                "offColor": "#B52B2B",
                "onColor": "#2E8B57",
                "onCondition": ">=",
                "onConditionValue": "1",
                "label": "Čerpadlo",
            },
        },
        {
            "visual_type": "indicator",
            "name": "Stav nádrže",
            "column": 2,
            "row": 4,
            "size_x": 2,
            "size_y": 2,
            "feed_keys": [FEED["tank_state"]],
            "properties": {
                "offColor": "#2E8B57",
                "onColor": "#B52B2B",
                "onCondition": ">=",
                "onConditionValue": "1",
                "label": "Nádrž",
            },
        },
        {
            "visual_type": "gauge",
            "name": "Spotreba vody",
            "column": 4,
            "row": 4,
            "size_x": 2,
            "size_y": 2,
            "feed_keys": [FEED["used_water"]],
            "properties": {
                "minValue": "0",
                "maxValue": "100",
                "label": "Litrov",
                "gaugeColor": "#2952A3",
            },
        },
        {
            "visual_type": "text",
            "name": "Aktívny alert",
            "column": 6,
            "row": 4,
            "size_x": 3,
            "size_y": 2,
            "feed_keys": [FEED["alert"]],
            "properties": {
                "fontSize": "14",
            },
        },
        {
            "visual_type": "toggle",
            "name": "Ovládanie čerpadla",
            "column": 9,
            "row": 4,
            "size_x": 3,
            "size_y": 2,
            "feed_keys": [FEED["pump_control"]],
            "properties": {
                "onText": "ON",
                "offText": "OFF",
                "onValue": "ON",
                "offValue": "OFF",
            },
        },
    ]

    for block in blocks:
        create_block(dashboard_key, block)


if __name__ == "__main__":
    check_key()

    print(f"\n=== Adafruit IO Dashboard Setup ===")
    print(f"User: {USERNAME}")
    print(f"Dashboard: {DASHBOARD_NAME}")
    print()

    print("[1/2] Getting or creating dashboard...")
    dash_key = get_or_create_dashboard()

    print(f"\n[2/2] Creating blocks on dashboard '{dash_key}'...")
    create_all_blocks(dash_key)

    print(f"\nDone -- https://io.adafruit.com/{USERNAME}/dashboards/{dash_key}")
