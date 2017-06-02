# opcuapp

OPC-UA C++ Wrapper

## Build (Windows)

mkdir build && cd build

cmake .. "-DOPENSSL_ROOT_DIR:PATH=open_ssl_path" "-DOPCUA_ROOT_DIR:PATH=ustack_path"

cmake --build .
