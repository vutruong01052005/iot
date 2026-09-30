import os, numpy as np, pandas as pd
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS

url = os.getenv('INFLUX_URL')
token = os.getenv('INFLUX_TOKEN')
org = os.getenv('INFLUX_ORG')
bucket = os.getenv('INFLUX_BUCKET')

c = InfluxDBClient(url=url, token=token, org=org)
qapi = c.query_api()
w = c.write_api(write_options=SYNCHRONOUS)

q = f'''from(bucket:"{bucket}") |> range(start:-24h) |> filter(fn:(r)=>r._measurement=="sensor_raw") |> pivot(rowKey:["_time"],columnKey:["_field"],valueColumn:"_value") |> sort(columns:["_time"])'''
x = qapi.query_data_frame(q)
df = pd.concat(x, ignore_index=True) if isinstance(x, list) and x else x
if df is None or len(df) == 0:
    print('No raw data yet')
    raise SystemExit

sensor_cols = [col for col in ['temperature', 'humidity', 'light_lux', 'distance_cm'] if col in df.columns]
numeric_cols = [col for col in ['temperature', 'humidity', 'light_lux', 'distance_cm', 'rssi'] if col in df.columns]
missing_dht = [col for col in ['temperature', 'humidity'] if col not in sensor_cols]
if missing_dht:
    print('DHT fields missing; processing available sensors only:', ', '.join(missing_dht))
if not sensor_cols:
    print('No supported sensor fields found in sensor_raw')
    raise SystemExit

for col in numeric_cols:
    df[col] = pd.to_numeric(df[col], errors='coerce')
df[numeric_cols] = df[numeric_cols].interpolate(limit_direction='both').ffill().bfill()

for col in numeric_cols:
    q1, q3 = df[col].quantile([.25, .75])
    iqr = q3 - q1
    lo, hi = q1 - 1.5 * iqr, q3 + 1.5 * iqr
    bad = (df[col] < lo) | (df[col] > hi)
    df.loc[bad, col] = np.nan
    df[col] = df[col].interpolate(limit_direction='both').ffill().bfill()

df['_time'] = pd.to_datetime(df['_time'], utc=True)
df = df.set_index('_time')

agg = df[sensor_cols].resample('10s').mean().dropna(how='all')
if agg.empty:
    print('No numeric sensor samples to preprocess')
    raise SystemExit

rolling_columns = {
    'temperature': 'temperature_rolling_mean',
    'humidity': 'humidity_rolling_mean',
    'light_lux': 'light_rolling_mean',
}
for source, output in rolling_columns.items():
    if source in agg:
        agg[output] = agg[source].rolling(3, min_periods=1).mean()
if 'distance_cm' in agg:
    agg['distance_delta'] = agg.distance_cm.diff()

for col in sensor_cols:
    mn, mx = agg[col].min(), agg[col].max()
    agg[col + '_norm'] = 0 if mx == mn else (agg[col] - mn) / (mx - mn)

for ts, row in agg.iterrows():
    p = Point('sensor_preprocessed').tag('device', 'esp32')
    for col, val in row.items():
        if pd.notna(val):
            p = p.field(col, float(val))
    w.write(bucket=bucket, org=org, record=p, time=ts)

print('processed windows:', len(agg))