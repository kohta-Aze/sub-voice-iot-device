import json
import os
import time
import boto3
from openai import OpenAI
import paho.mqtt.client as mqtt
import random

# ==========================================
# 設定エリア (環境変数から読み込む安全な設計)
# ==========================================
OPENAI_API_KEY = os.environ.get("OPENAI_API_KEY")
AWS_REGION = "ap-northeast-1"
BUCKET_NAME = os.environ.get("BUCKET_NAME", "YOUR_BUCKET_NAME")
MQTT_SERVER = os.environ.get("MQTT_SERVER", "broker.emqx.io")
MQTT_TOPIC = os.environ.get("MQTT_TOPIC", "YOUR_PATH")
SECRET_PASS = os.environ.get("SECRET_PASS")

# S3 Base URL
S3_BASE_URL = f"http://{BUCKET_NAME}.s3-website-{AWS_REGION}.amazonaws.com"

# ==========================================
# クライアント初期化
# Lambdaには適切なIAMロールを付与するため、アクセスキーの直書きは不要です
# ==========================================
client = OpenAI(api_key=OPENAI_API_KEY)
s3 = boto3.client('s3', region_name=AWS_REGION)

def lambda_handler(event, context):
    try:
        # 1. スマホ(Web)からの入力を受け取る
        body = json.loads(event.get('body', '{}'))
        user_text = body.get('text', 'こんにちは')
        
        print(f"User Input: {user_text}")

        # 2. ChatGPTで文章生成
        gpt_response = client.chat.completions.create(
            model="gpt-4o-mini",
            messages=[
                {"role": "system", "content": "30文字以内で返答してください。"},
                {"role": "user", "content": user_text}
            ]
        )
        ai_text = gpt_response.choices[0].message.content

        # 3. 音声合成 (Lambdaは /tmp/ フォルダのみ書き込み可)
        tmp_filename = "/tmp/speech.mp3"
        response = client.audio.speech.create(
            model="tts-1",
            voice="alloy",
            input=ai_text
        )
        response.stream_to_file(tmp_filename)

        # 4. S3アップロード
        s3_filename = f"speech_{int(time.time())}.mp3"
        s3.upload_file(tmp_filename, BUCKET_NAME, s3_filename, ExtraArgs={'ContentType': 'audio/mpeg'})
        
        audio_url = f"{S3_BASE_URL}/{s3_filename}"

        # 5. MQTT送信
        client_id = f"LambdaSender-{int(time.time())}-{random.randint(0, 1000)}"
        mqtt_client = mqtt.Client(client_id)
        
        mqtt_client.connect(MQTT_SERVER, 1883, 60)
        mqtt_client.loop_start()
        
        payload = {
            "url": audio_url,
            "text": ai_text,
            "magic": SECRET_PASS
        }
        
        print(f"Publishing to {MQTT_TOPIC}...")
        info = mqtt_client.publish(MQTT_TOPIC, json.dumps(payload))
        
        # 送信完了待機
        info.wait_for_publish(timeout=5)
        
        mqtt_client.loop_stop()
        mqtt_client.disconnect()

        return {
            'statusCode': 200,
            'body': json.dumps({'message': 'Success', 'url': audio_url, 'text': ai_text}),
            'headers': {
                'Content-Type': 'application/json',
                'Access-Control-Allow-Origin': '*' # CORS対策
            }
        }

    except Exception as e:
        print(f"Error: {str(e)}")
        return {
            'statusCode': 500,
            'body': json.dumps({'error': str(e)}),
            'headers': {
                'Content-Type': 'application/json',
                'Access-Control-Allow-Origin': '*'
            }
        }