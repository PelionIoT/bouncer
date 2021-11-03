# socket-proxy

Proxy a unix socket

## Build

```
git clone https://github.com/PelionIoT/socket-proxy.git
cd socket-proxy
mkdir build
cd build
cmake ..
make
```

## Run

```
./socket-proxy <listening socket path> <docker.sock path>
```
