#!/bin/sh

g++ -O3 -Wall -o bin/secure_tunnel -I./src src/functions.cpp src/socket_broker.cpp src/secure_tunnel.cpp 

g++ -O3 -Wall -o bin/port_reflection -I./src src/functions.cpp src/socket_broker.cpp src/port_reflection.cpp 

