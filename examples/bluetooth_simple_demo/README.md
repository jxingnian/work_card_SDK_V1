# 蓝牙设备管理演示程序

这是一个简单的蓝牙Web演示程序，实现了基础的蓝牙设备管理功能。

## 功能特性

✅ **实时扫描蓝牙设备** - 扫描附近的蓝牙设备  
✅ **设备配对** - 配对未连接的蓝牙设备  
✅ **设备连接/断开** - 连接和断开已配对的设备  
✅ **取消配对** - 移除已配对的设备  
✅ **实时刷新** - 自动刷新设备列表  
✅ **Web界面** - 美观的网页操作界面  

## 编译方法

### 方法1：使用编译脚本（推荐）

```bash
cd work_card_SDK_V1/examples/bluetooth_simple_demo
chmod +x build.sh
./build.sh
```

### 方法2：手动编译

```bash
cd work_card_SDK_V1/examples/bluetooth_simple_demo
mkdir build
cd build
cmake ..
make -j$(nproc)
```

## 运行方法

### 在开发环境运行

```bash
cd work_card_SDK_V1/examples/bluetooth_simple_demo/build
./bluetooth_simple_demo --port 8080 --web-root ../web
```

### 部署到板端运行

1. 将编译好的程序和web文件复制到板端：

```bash
# 在开发机上
scp -r build/bluetooth_simple_demo root@192.168.137.47:/root/
scp -r web root@192.168.137.47:/root/
```

2. 在板端运行：

```bash
# SSH登录到板端
ssh root@192.168.137.47

# 启动蓝牙服务（如果未运行）
bluetoothd &
bluealsa -p a2dp-source -p a2dp-sink &

# 运行程序
cd /root
./bluetooth_simple_demo --port 8080 --web-root ./web
```

3. 在浏览器访问：

```
http://192.168.137.47:8080
```

## 使用说明

1. **开始扫描** - 点击"开始扫描"按钮，程序会搜索附近的蓝牙设备
2. **配对设备** - 在"扫描到的设备"列表中，点击"配对并连接"
3. **连接设备** - 已配对的设备会显示在"已配对设备"列表，点击"连接"按钮
4. **断开设备** - 点击"断开"按钮断开连接
5. **取消配对** - 点击"取消配对"按钮移除设备

## 目录结构

```
bluetooth_simple_demo/
├── bluetooth_simple_demo.cpp    # 主程序
├── CMakeLists.txt               # CMake配置
├── build.sh                     # 编译脚本
├── run_bluetooth_simple_demo.sh # 运行脚本
├── README.md                    # 说明文档
└── web/
    └── index.html              # Web界面
```

## API接口

程序提供以下HTTP API接口：

- `GET /` - 网页首页
- `GET /api/devices` - 获取设备列表
- `GET /api/action?action=start_scan` - 开始扫描
- `GET /api/action?action=stop_scan` - 停止扫描
- `GET /api/action?action=pair&address=XX:XX:XX:XX:XX:XX` - 配对设备
- `GET /api/action?action=connect&address=XX:XX:XX:XX:XX:XX` - 连接设备
- `GET /api/action?action=disconnect&address=XX:XX:XX:XX:XX:XX` - 断开设备
- `GET /api/action?action=remove&address=XX:XX:XX:XX:XX:XX` - 移除设备
- `GET /api/action?action=pair_and_connect&address=XX:XX:XX:XX:XX:XX` - 配对并连接

## 依赖要求

- BlueZ (bluetoothctl)
- BlueALSA (bluealsa, bluealsa-cli)
- ehal_bluetooth SDK

## 注意事项

1. 确保蓝牙适配器已启用（hci0）
2. 确保bluetoothd服务正在运行
3. 确保bluealsa服务正在运行
4. 配对某些设备可能需要等待几秒钟
5. 扫描功能会持续运行直到手动停止

## 故障排查

### 编译错误

如果编译时找不到头文件：
```bash
# 检查SDK库是否存在
ls -l ../../lib/libehal_bluetooth.*
ls -l ../../include/ehal_bluetooth.h
```

### 运行错误

如果程序无法启动：
```bash
# 检查蓝牙服务
ps aux | grep bluetoothd
ps aux | grep bluealsa

# 检查蓝牙设备
hciconfig
```

如果无法访问网页：
```bash
# 检查端口是否被占用
netstat -tuln | grep 8080

# 检查防火墙
iptables -L
```

## 开发者信息

- 基于ehal_bluetooth SDK
- 使用C++11标准
- HTTP服务器使用原生socket实现
- 前端使用纯HTML/CSS/JavaScript
