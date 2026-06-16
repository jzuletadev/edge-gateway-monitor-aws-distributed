import json
import boto3
from boto3.dynamodb.conditions import Key
from decimal import Decimal

dynamodb = boto3.resource('dynamodb', region_name='us-east-2')
tabla = dynamodb.Table('telemetria_rack')

def limpiar(obj):
    if isinstance(obj, list):
        return [limpiar(i) for i in obj]
    if isinstance(obj, dict):
        return {k: limpiar(v) for k, v in obj.items()}
    if isinstance(obj, Decimal):
        return int(obj) if obj % 1 == 0 else float(obj)
    return obj

def lambda_handler(event, context):
    device = 'nodo-rack-01'
    limite = 50
    resp = tabla.query(
        KeyConditionExpression=Key('device').eq(device),
        ScanIndexForward=False,
        Limit=limite
    )
    items = limpiar(resp.get('Items', []))
    items.reverse()
    return {
        'statusCode': 200,
        'headers': {
            'Content-Type': 'application/json',
            'Access-Control-Allow-Origin': '*'
        },
        'body': json.dumps(items)
    }