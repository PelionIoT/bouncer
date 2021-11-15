# bouncer

Bouncer intercepts docker socket messages for the purpose of checking image signatures.  It does this by serving a proxy to the socket.

## Build

```
git clone https://github.com/PelionIoT/bouncer.git
cd bouncer
mkdir build
cd build
cmake ..
make
```

## Run

```
./bouncer <listening socket path> <docker.sock path>
```
