import os, pandas as pd, streamlit as st
from influxdb_client import InfluxDBClient

url = os.getenv('INFLUX_URL')
token = os.getenv('INFLUX_TOKEN')
org = os.getenv('INFLUX_ORG')
bucket = os.getenv('INFLUX_BUCKET')

st.set_page_config(page_title='Bài 2 IoT', layout='wide')
st.title('BÀI THỰC HÀNH 2 — IoT SENSOR MONITORING')

api = InfluxDBClient(url=url, token=token, org=org).query_api()

def get(meas):
    q = f'''from(bucket:"{bucket}") |> range(start:-24h) |> filter(fn:(r)=>r._measurement=="{meas}") |> pivot(rowKey:["_time"],columnKey:["_field"],valueColumn:"_value") |> sort(columns:["_time"])'''
    x = api.query_data_frame(q)
    return pd.concat(x, ignore_index=True) if isinstance(x, list) and x else x

def format_metric(value, precision, unit):
    if pd.isna(value):
        return 'N/A'
    return f'{value:.{precision}f} {unit}'

def latest_metric(frame, name):
    if name not in frame:
        return None
    values = frame[name].dropna()
    return None if values.empty else values.iloc[-1]

raw = get('sensor_raw')
pre = get('sensor_preprocessed')

if raw is None or len(raw) == 0:
    st.warning('Chưa có dữ liệu MQTT. Hãy chạy Wokwi và collector.')
    st.stop()

raw['_time'] = pd.to_datetime(raw['_time'])
if pre is not None and len(pre) > 0:
    pre['_time'] = pd.to_datetime(pre['_time'])
metric_data = pre if pre is not None and len(pre) > 0 else raw

def metric_value(name):
    value = latest_metric(raw, name)
    if value is not None:
        return value
    return latest_metric(metric_data, name)

a, b, c, d = st.columns(4)
a.metric('Nhiệt độ', format_metric(metric_value('temperature'), 2, '°C'))
b.metric('Độ ẩm', format_metric(metric_value('humidity'), 2, '%RH'))
c.metric('Ánh sáng', format_metric(metric_value('light_lux'), 1, 'lux'))
d.metric('Khoảng cách', format_metric(metric_value('distance_cm'), 2, 'cm'))

st.subheader('Raw')
sensor_cols = [name for name in ['temperature', 'humidity', 'light_lux', 'distance_cm'] if name in raw]
if sensor_cols:
    st.line_chart(raw.set_index('_time')[sensor_cols])
else:
    st.info('Chưa có field cảm biến để vẽ biểu đồ.')
latest_raw = raw.iloc[-1]
missing_dht = [name for name in ['temperature', 'humidity']
               if name not in raw or pd.isna(latest_raw.get(name))]
if missing_dht:
    st.warning(f"Mẫu MQTT mới nhất thiếu {', '.join(missing_dht)}; thẻ đang dùng dữ liệu tiền xử lý gần nhất. Kiểm tra DHT và Serial Monitor.")
st.dataframe(raw.tail(20), use_container_width=True)

if pre is not None and len(pre) > 0:
    st.subheader('Sau tiền xử lý')
    cols = [x for x in ['temperature', 'humidity', 'light_lux', 'distance_cm',
                        'temperature_rolling_mean', 'humidity_rolling_mean',
                        'light_rolling_mean'] if x in pre]
    st.line_chart(pre.set_index('_time')[cols])
    st.dataframe(pre.tail(20), use_container_width=True)
else:
    st.info('Chưa có sensor_preprocessed. Chạy docker compose run --rm preprocess')