cd /home/xingnian/ehal_wifi_cli_demo
./run_wifi_cli_demo.sh scan
./run_wifi_cli_demo.sh status
./run_wifi_cli_demo.sh connect --ssid HOME --password 15975324685
./run_wifi_cli_demo.sh disconnect
./run_wifi_cli_demo.sh ap-start --ssid WorkCard_AP --password 12345678 --channel 6
./run_wifi_cli_demo.sh ap-status
./run_wifi_cli_demo.sh ap-stop