#!/usr/bin/env python
# coding=utf-8
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server_address = ('127.0.0.1', 6666)
sock.connect(server_address)

message = 'hello\r\n'
sock.sendall(message)

data = sock.recv(1024)
print(data)
