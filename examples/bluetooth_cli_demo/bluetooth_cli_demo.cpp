#include "ehal_bluetooth.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

ehal_bluetooth_t *g_bt = nullptr;

void on_event(const ehal_bt_event_t *event, void *)
{
    if (event == nullptr) {
        return;
    }
    std::printf("event=%d result=%d address=%s message=%s\n",
                static_cast<int>(event->type), event->result,
                event->device.address, event->message);
    std::fflush(stdout);
}

void print_result(int ret)
{
    std::printf("result=%s (%d)\n", ehal_bt_error_string(ret), ret);
    if (ret != EHAL_OK) {
        char output[4096] = {};
        if (ehal_bt_get_last_output(g_bt, output, sizeof(output)) == EHAL_OK &&
            output[0] != '\0') {
            std::printf("%s", output);
        }
    }
}

void print_adapter()
{
    ehal_bt_adapter_info_t info{};
    int ret = ehal_bt_get_adapter_info(g_bt, &info);
    print_result(ret);
    if (ret == EHAL_OK) {
        std::printf("address=%s name=%s alias=%s powered=%d pairable=%d "
                    "discoverable=%d discovering=%d version=%s manufacturer=%s\n",
                    info.address, info.name, info.alias, info.powered,
                    info.pairable, info.discoverable, info.discovering,
                    info.version, info.manufacturer);
    }
}

void print_devices()
{
    ehal_bt_device_info_t devices[64] = {};
    uint32_t count = 0U;
    int ret = ehal_bt_get_devices(g_bt, devices, 64U, &count);
    print_result(ret);
    if (ret == EHAL_OK) {
        for (uint32_t i = 0; i < count && i < 64U; ++i) {
            std::printf("%s name=%s alias=%s rssi=%d paired=%d trusted=%d "
                        "connected=%d blocked=%d\n",
                        devices[i].address, devices[i].name, devices[i].alias,
                        devices[i].rssi, devices[i].paired, devices[i].trusted,
                        devices[i].connected, devices[i].blocked);
        }
    }
}

void usage()
{
    std::puts("Commands:");
    std::puts("  adapter");
    std::puts("  power on|off");
    std::puts("  name <name>");
    std::puts("  pairable on|off");
    std::puts("  discoverable on|off");
    std::puts("  scan start|stop|status");
    std::puts("  devices");
    std::puts("  info <MAC>");
    std::puts("  pair|cancel-pair|remove|connect|disconnect <MAC>");
    std::puts("  trust|block <MAC> on|off");
    std::puts("  profile-connect|profile-disconnect <MAC> <a2dp-source|a2dp-sink|hfp-ag|hfp-hf|hsp-ag|hsp-hs>");
    std::puts("  gatt-services <MAC>");
    std::puts("  a2dp-info <MAC>");
    std::puts("  a2dp-play <MAC> <file>");
    std::puts("  avrcp <MAC> play");
    std::puts("  quit");
}

ehal_bt_profile_t profile_from_name(const std::string &value)
{
    if (value == "a2dp-source") return EHAL_BT_PROFILE_A2DP_SOURCE;
    if (value == "a2dp-sink") return EHAL_BT_PROFILE_A2DP_SINK;
    if (value == "hfp-ag") return EHAL_BT_PROFILE_HFP_AG;
    if (value == "hfp-hf") return EHAL_BT_PROFILE_HFP_HF;
    if (value == "hsp-ag") return EHAL_BT_PROFILE_HSP_AG;
    if (value == "hsp-hs") return EHAL_BT_PROFILE_HSP_HS;
    return EHAL_BT_PROFILE_ANY;
}

bool enabled(const std::string &value)
{
    return value == "on" || value == "1";
}

} // namespace

int main()
{
    ehal_bt_config_t config{};
    config.adapter = "hci0";
    config.command_timeout_ms = 15000U;
    config.scan_timeout_ms = 10000U;
    config.event_callback = on_event;

    int ret = ehal_bt_create(&g_bt);
    if (ret == EHAL_OK) ret = ehal_bt_configure(g_bt, &config);
    if (ret == EHAL_OK) ret = ehal_bt_start(g_bt);
    if (ret != EHAL_OK) {
        print_result(ret);
        ehal_bt_destroy(g_bt);
        return 1;
    }
    std::puts("Bluetooth SDK demo started adapter=hci0");
    usage();

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream input(line);
        std::vector<std::string> args;
        std::string arg;
        while (input >> arg) args.push_back(arg);
        if (args.empty()) continue;
        if (args[0] == "quit" || args[0] == "exit") break;
        if (args[0] == "adapter") {
            print_adapter();
        } else if (args[0] == "devices") {
            print_devices();
        } else if (args[0] == "power" && args.size() == 2U) {
            print_result(ehal_bt_set_power(g_bt, enabled(args[1])));
        } else if (args[0] == "name" && args.size() >= 2U) {
            print_result(ehal_bt_set_name(g_bt, line.substr(5U).c_str()));
        } else if (args[0] == "pairable" && args.size() == 2U) {
            print_result(ehal_bt_set_pairable(g_bt, enabled(args[1])));
        } else if (args[0] == "discoverable" && args.size() == 2U) {
            print_result(ehal_bt_set_discoverable(g_bt, enabled(args[1])));
        } else if (args[0] == "scan" && args.size() == 2U) {
            if (args[1] == "start") ret = ehal_bt_start_scan(g_bt);
            else if (args[1] == "stop") ret = ehal_bt_stop_scan(g_bt);
            else {
                int scanning = 0;
                ret = ehal_bt_is_scanning(g_bt, &scanning);
                std::printf("scanning=%d\n", scanning);
            }
            print_result(ret);
        } else if (args[0] == "info" && args.size() == 2U) {
            ehal_bt_device_info_t info{};
            ret = ehal_bt_get_device_info(g_bt, args[1].c_str(), &info);
            print_result(ret);
            if (ret == EHAL_OK) std::printf("name=%s alias=%s connected=%d\n",
                                             info.name, info.alias, info.connected);
        } else if ((args[0] == "pair" || args[0] == "cancel-pair" ||
                    args[0] == "remove" || args[0] == "connect" ||
                    args[0] == "disconnect") && args.size() == 2U) {
            if (args[0] == "pair") ret = ehal_bt_pair(g_bt, args[1].c_str());
            else if (args[0] == "cancel-pair") ret = ehal_bt_cancel_pair(g_bt, args[1].c_str());
            else if (args[0] == "remove") ret = ehal_bt_remove_device(g_bt, args[1].c_str());
            else if (args[0] == "connect") ret = ehal_bt_connect(g_bt, args[1].c_str());
            else ret = ehal_bt_disconnect(g_bt, args[1].c_str());
            print_result(ret);
        } else if ((args[0] == "trust" || args[0] == "block") && args.size() == 3U) {
            ret = args[0] == "trust" ?
                ehal_bt_set_trusted(g_bt, args[1].c_str(), enabled(args[2])) :
                ehal_bt_set_blocked(g_bt, args[1].c_str(), enabled(args[2]));
            print_result(ret);
        } else if ((args[0] == "profile-connect" || args[0] == "profile-disconnect") &&
                   args.size() == 3U) {
            ehal_bt_profile_t profile = profile_from_name(args[2]);
            ret = args[0] == "profile-connect" ?
                ehal_bt_connect_profile(g_bt, args[1].c_str(), profile) :
                ehal_bt_disconnect_profile(g_bt, args[1].c_str(), profile);
            print_result(ret);
        } else if (args[0] == "gatt-services" && args.size() == 2U) {
            print_result(ehal_bt_gatt_discover_services(g_bt, args[1].c_str()));
            char output[4096] = {};
            ehal_bt_get_last_output(g_bt, output, sizeof(output));
            std::printf("%s", output);
        } else if (args[0] == "a2dp-info" && args.size() == 2U) {
            ehal_bt_audio_info_t info{};
            ret = ehal_bt_a2dp_get_info(g_bt, args[1].c_str(), &info);
            print_result(ret);
            std::printf("%s\n", info.pcm_path);
        } else if (args[0] == "a2dp-play" && args.size() == 3U) {
            print_result(ehal_bt_a2dp_play_file(g_bt, args[1].c_str(), args[2].c_str()));
        } else if (args[0] == "avrcp" && args.size() == 3U) {
            if (args[2] == "play") ret = ehal_bt_avrcp_play(g_bt, args[1].c_str());
            else ret = EHAL_ERR_NOT_SUPPORTED;
            print_result(ret);
        } else {
            usage();
        }
    }

    ehal_bt_stop(g_bt);
    ehal_bt_destroy(g_bt);
    return 0;
}
