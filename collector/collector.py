import os, json, time
import paho.mqtt.client as mqtt
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS

host = os.getenv('MQTT_HOST', 'localhost')
port = int(os.getenv('MQTT_PORT', '1883'))
topic = os.getenv('MQTT_TOPIC', 'iot/bai2/esp32/telemetry')
url = os.getenv('INFLUX_URL')
token = os.getenv('INFLUX_TOKEN')
org = os.getenv('INFLUX_ORG')
bucket = os.getenv('INFLUX_BUCKET')

db = InfluxDBClient(url=url, token=token, org=org)
write = db.write_api(write_options=SYNCHRONOUS)

required_keys = ['device_id', 'ts', 'seq', 'temperature', 'humidity', 'light_lux', 'distance_cm', 'rssi', 'uptime_s']

def on_connect(c, u, f, rc, properties=None):
    print('MQTT connected', rc)
    c.subscribe(topic, qos=1)

def on_message(c, u, m):
    try:
        d = json.loads(m.payload.decode())
        missing = [k for k in required_keys if k not in d]
        if missing:
            raise ValueError(f'missing telemetry field: {missing}')
        
        p = Point('sensor_raw').tag('device', d['device_id'])
        for k in ['temperature', 'humidity', 'light_lux', 'distance_cm']:
            if d[k] is not None and isinstance(d[k], (int, float)):
                p = p.field(k, float(d[k]))
        for k in ['rssi', 'seq', 'uptime_s']:
            p = p.field(k, int(d[k]))
        p = p.field('gateway_ts', time.time())
        p = p.field('device_ts_ms', int(d['ts']))
        
        write.write(bucket=bucket, org=org, record=p)
        print('stored', d)
    except Exception as e:
        print('INVALID', e, m.payload)

while True:
    try:
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id='iot-collector')
        c.on_connect = on_connect
        c.on_message = on_message
        c.connect(host, port, 60)
        c.loop_forever()
    except Exception as e:
        print('retry', e)
        time.sleep(3)