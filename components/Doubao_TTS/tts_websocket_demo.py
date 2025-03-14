#coding=utf-8

'''
要求 Python 3.6 或更高版本

安装依赖库:
pip install asyncio
pip install websockets

'''

import asyncio  # 导入异步IO库
import websockets  # 导入WebSocket库
import uuid  # 导入UUID库，用于生成唯一标识符
import json  # 导入JSON库，用于处理JSON数据
import gzip  # 导入gzip库，用于数据压缩
import copy  # 导入copy库，用于对象复制

# 定义消息类型的常量
MESSAGE_TYPES = {
    11: "音频仅服务器响应",
    12: "前端服务器响应",
    15: "来自服务器的错误消息"
}

# 定义消息类型特定标志的常量
MESSAGE_TYPE_SPECIFIC_FLAGS = {
    0: "无序列号",
    1: "序列号 > 0",
    2: "来自服务器的最后消息 (seq < 0)",
    3: "序列号 < 0"
}

# 定义消息序列化方法的常量
MESSAGE_SERIALIZATION_METHODS = {
    0: "无序列化",
    1: "JSON",
    15: "自定义类型"
}

# 定义消息压缩方法的常量
MESSAGE_COMPRESSIONS = {
    0: "无压缩",
    1: "gzip",
    15: "自定义压缩方法"
}

# 配置参数
appid = "xxx"  # 应用ID
token = "xxx"  # 访问令牌
cluster = "xxx"  # 集群信息
voice_type = "xxx"  # 语音类型
host = "openspeech.bytedance.com"  # 服务器主机
api_url = f"wss://{host}/api/v1/tts/ws_binary"  # WebSocket API URL

# 版本: b0001 (4位)
# 头部大小: b0001 (4位)
# 消息类型: b0001 (完整客户端请求) (4位)
# 消息类型特定标志: b0000 (无) (4位)
# 消息序列化方法: b0001 (JSON) (4位)
# 消息压缩: b0001 (gzip) (4位)
# 保留数据: 0x00 (1字节)
default_header = bytearray(b'\x11\x10\x11\x00')  # 默认消息头

# 请求的JSON结构
request_json = {
    "app": {
        "appid": appid,  # 应用ID
        "token": "access_token",  # 访问令牌
        "cluster": cluster  # 集群信息
    },
    "user": {
        "uid": "388808087185088"  # 用户ID
    },
    "audio": {
        "voice_type": "xxx",  # 语音类型
        "encoding": "mp3",  # 音频编码格式
        "speed_ratio": 1.0,  # 语速
        "volume_ratio": 1.0,  # 音量
        "pitch_ratio": 1.0,  # 音调
    },
    "request": {
        "reqid": "xxx",  # 请求ID
        "text": "字节跳动语音合成。",  # 要合成的文本
        "text_type": "plain",  # 文本类型
        "operation": "xxx"  # 操作类型
    }
}

# 异步函数：提交请求
async def test_submit():
    submit_request_json = copy.deepcopy(request_json)  # 深拷贝请求JSON
    submit_request_json["audio"]["voice_type"] = voice_type  # 设置语音类型
    submit_request_json["request"]["reqid"] = str(uuid.uuid4())  # 生成唯一请求ID
    submit_request_json["request"]["operation"] = "submit"  # 设置操作类型为提交
    payload_bytes = str.encode(json.dumps(submit_request_json))  # 将请求JSON转换为字节
    payload_bytes = gzip.compress(payload_bytes)  # 压缩请求数据
    full_client_request = bytearray(default_header)  # 创建完整的客户端请求
    full_client_request.extend((len(payload_bytes)).to_bytes(4, 'big'))  # 添加负载大小（4字节）
    full_client_request.extend(payload_bytes)  # 添加负载数据
    print("\n------------------------ 测试 '提交' -------------------------")
    print("请求的JSON: ", submit_request_json)  # 打印请求的JSON
    print("\n请求的字节: ", full_client_request)  # 打印请求的字节
    file_to_save = open("test_submit.mp3", "wb")  # 打开文件以保存音频数据
    header = {"Authorization": f"Bearer; {token}"}  # 设置请求头
    async with websockets.connect(api_url, extra_headers=header, ping_interval=None) as ws:  # 连接WebSocket
        await ws.send(full_client_request)  # 发送请求
        while True:
            res = await ws.recv()  # 接收响应
            done = parse_response(res, file_to_save)  # 解析响应并保存到文件
            if done:
                file_to_save.close()  # 关闭文件
                break
        print("\n关闭连接...")  # 打印关闭连接信息

# 异步函数：查询请求
async def test_query():
    query_request_json = copy.deepcopy(request_json)  # 深拷贝请求JSON
    query_request_json["audio"]["voice_type"] = voice_type  # 设置语音类型
    query_request_json["request"]["reqid"] = str(uuid.uuid4())  # 生成唯一请求ID
    query_request_json["request"]["operation"] = "query"  # 设置操作类型为查询
    payload_bytes = str.encode(json.dumps(query_request_json))  # 将请求JSON转换为字节
    payload_bytes = gzip.compress(payload_bytes)  # 压缩请求数据
    full_client_request = bytearray(default_header)  # 创建完整的客户端请求
    full_client_request.extend((len(payload_bytes)).to_bytes(4, 'big'))  # 添加负载大小（4字节）
    full_client_request.extend(payload_bytes)  # 添加负载数据
    print("\n------------------------ 测试 '查询' -------------------------")
    print("请求的JSON: ", query_request_json)  # 打印请求的JSON
    print("\n请求的字节: ", full_client_request)  # 打印请求的字节
    file_to_save = open("test_query.mp3", "wb")  # 打开文件以保存音频数据
    header = {"Authorization": f"Bearer; {token}"}  # 设置请求头
    async with websockets.connect(api_url, extra_headers=header, ping_interval=None) as ws:  # 连接WebSocket
        await ws.send(full_client_request)  # 发送请求
        res = await ws.recv()  # 接收响应
        parse_response(res, file_to_save)  # 解析响应并保存到文件
        file_to_save.close()  # 关闭文件
        print("\n关闭连接...")  # 打印关闭连接信息

# 函数：解析响应
def parse_response(res, file):
    print("--------------------------- 响应 ---------------------------")
    # print(f"响应原始字节: {res}")
    protocol_version = res[0] >> 4  # 提取协议版本
    header_size = res[0] & 0x0f  # 提取头部大小
    message_type = res[1] >> 4  # 提取消息类型
    message_type_specific_flags = res[1] & 0x0f  # 提取消息类型特定标志
    serialization_method = res[2] >> 4  # 提取序列化方法
    message_compression = res[2] & 0x0f  # 提取消息压缩方法
    reserved = res[3]  # 提取保留字段
    header_extensions = res[4:header_size*4]  # 提取头部扩展
    payload = res[header_size*4:]  # 提取负载数据
    print(f"            协议版本: {protocol_version:#x} - 版本 {protocol_version}")
    print(f"                 头部大小: {header_size:#x} - {header_size * 4} 字节 ")
    print(f"                消息类型: {message_type:#x} - {MESSAGE_TYPES[message_type]}")
    print(f" 消息类型特定标志: {message_type_specific_flags:#x} - {MESSAGE_TYPE_SPECIFIC_FLAGS[message_type_specific_flags]}")
    print(f"消息序列化方法: {serialization_method:#x} - {MESSAGE_SERIALIZATION_METHODS[serialization_method]}")
    print(f"         消息压缩: {message_compression:#x} - {MESSAGE_COMPRESSIONS[message_compression]}")
    print(f"                    保留: {reserved:#04x}")
    if header_size != 1:
        print(f"           头部扩展: {header_extensions}")  # 打印头部扩展
    if message_type == 0xb:  # 音频仅服务器响应
        if message_type_specific_flags == 0:  # 无序列号作为确认
            print("                负载大小: 0")
            return False
        else:
            sequence_number = int.from_bytes(payload[:4], "big", signed=True)  # 提取序列号
            payload_size = int.from_bytes(payload[4:8], "big", signed=False)  # 提取负载大小
            payload = payload[8:]  # 提取负载数据
            print(f"             序列号: {sequence_number}")
            print(f"                负载大小: {payload_size} 字节")
        file.write(payload)  # 将负载写入文件
        if sequence_number < 0:
            return True
        else:
            return False
    elif message_type == 0xf:  # 错误消息
        code = int.from_bytes(payload[:4], "big", signed=False)  # 提取错误代码
        msg_size = int.from_bytes(payload[4:8], "big", signed=False)  # 提取消息大小
        error_msg = payload[8:]  # 提取错误消息
        if message_compression == 1:  # 如果使用gzip压缩
            error_msg = gzip.decompress(error_msg)  # 解压缩错误消息
        error_msg = str(error_msg, "utf-8")  # 转换为字符串
        print(f"          错误消息代码: {code}")
        print(f"          错误消息大小: {msg_size} 字节")
        print(f"               错误消息: {error_msg}")
        return True
    elif message_type == 0xc:  # 前端消息
        msg_size = int.from_bytes(payload[:4], "big", signed=False)  # 提取消息大小
        payload = payload[4:]  # 提取负载数据
        if message_compression == 1:  # 如果使用gzip压缩
            payload = gzip.decompress(payload)  # 解压缩负载数据
        print(f"            前端消息: {payload}")  # 打印前端消息
    else:
        print("未定义的消息类型!")  # 打印未定义消息类型的提示
        return True

# 主程序入口
if __name__ == '__main__':
    loop = asyncio.get_event_loop()  # 获取事件循环
    loop.run_until_complete(test_submit())  # 执行提交测试
    loop.run_until_complete(test_query())  # 执行查询测试
