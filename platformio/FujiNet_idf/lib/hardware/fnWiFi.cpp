#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"

#include <cstring>
/*
#include "nvs_flash.h"

//#include "lwip/err.h"
//#include "lwip/sys.h"
*/

#include "../../include/debug.h"
#include "../utils/utils.h"
#include "fnWiFi.h"
#include "fnSystem.h"

// Global object to manage WiFi
WiFiManager fnWiFi;

WiFiManager::~WiFiManager()
{
    stop();
}

// Set up requried resources and start WiFi driver
int WiFiManager::start()
{
    // Initilize an event group
    if (_wifi_event_group == nullptr)
        _wifi_event_group = xEventGroupCreate();

    // Make sure our network interface is initialized
    tcpip_adapter_init();

    // Create the default event loop, which is where the WiFi driver sends events
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Configure basic WiFi settings
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    // TODO: Provide way to change WiFi region/country?
    // Default is to automatically set the value based on the AP the device is talking to

    // Register for events we care about
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, _wifi_event_handler, this));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, _wifi_event_handler, this));

    // Set WiFi mode to Station
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_start());

    _started = true;
    return 0;
}

int WiFiManager::connect(const char *ssid, const char *password)
{
    if (_connected == true)
    {
        ESP_ERROR_CHECK(esp_wifi_disconnect());
        fnSystem.delay(750);
    }

    // Set WiFi mode to Station
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Some more config details...
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));

    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

    wifi_config.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));

    esp_err_t e = esp_wifi_connect();
    Debug_printf("esp_wifi_connect returned %d\n", e);
    return e;
}

// Remove resources and shut down WiFi driver
void WiFiManager::stop()
{
   
    // Un-register event handler
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, _wifi_event_handler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, _wifi_event_handler));

    // Remove event group
    if (_wifi_event_group != nullptr)
        vEventGroupDelete(_wifi_event_group);

    ESP_ERROR_CHECK(esp_wifi_disconnect());
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());

    if(_scan_records != nullptr)
        free(_scan_records);
    _scan_records = nullptr;
    _scan_record_count = 0;

    _started = false;
    _connected = false;
}

/*
This should be handled by an ESP-IDF connect/disconnect event handler,
but this is compatible with the Arduino WiFi setup we currently have
*/
bool WiFiManager::connected()
{
    /*
    wifi_ap_record_t apinfo;
    esp_err_t e = esp_wifi_sta_get_ap_info(&apinfo);

    return (e == ESP_OK);
    */
   return _connected;
}

/* Initiates a WiFi network scan and returns number of networks found
*/
uint8_t WiFiManager::scan_networks(uint8_t maxresults)
{
    // Free any existing scan records
    if(_scan_records != nullptr)
        free(_scan_records);
    _scan_record_count = 0;

    wifi_scan_config_t scan_conf;
    scan_conf.bssid = 0;
    scan_conf.ssid = 0;
    scan_conf.channel = 0;
    scan_conf.show_hidden = false;
    scan_conf.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_conf.scan_time.active.min = 100; // ms; 100 is what Arduino-ESP uses
    scan_conf.scan_time.active.max = 300; // ms; 300 is what Arduino-ESP uses

    int retries = 0;
    uint16_t result = 0;
    esp_err_t e;

    do
    {
        e =  esp_wifi_scan_start(&scan_conf, true);
        if(e == ESP_OK)
        {
            e = esp_wifi_scan_get_ap_num(&result);
            if(e== ESP_OK)
                break;
            Debug_printf("esp_wifi_scan_get_ap_num returned error %d\n", e);
        }
        else
        {
            Debug_printf("esp_wifi_scan_start returned error %d\n", e);
        }
        
    } while (++retries <= FNWIFI_RECONNECT_RETRIES);

    Debug_printf("esp_wifi_scan returned %d\n", result);

    // Boundary check
    if (result > maxresults)
        result = maxresults;

    // Allocate memory to store the results
    if(result > 0)
    {
        uint16_t numloaded = result;
        _scan_records = (wifi_ap_record_t *)malloc(result * sizeof(wifi_ap_record_t));

        e = esp_wifi_scan_get_ap_records(&numloaded, _scan_records);
        if(e != ESP_OK)
        {
            Debug_printf("esp_wifi_scan_get_ap_records returned error %d\n", e);
            if(_scan_records != nullptr)
                free(_scan_records);
            _scan_record_count = 0;
            return 0;
        }
        _scan_record_count = numloaded;
        return _scan_record_count;
    }

    return 0;
}

int WiFiManager::get_scan_result(uint8_t index, char ssid[32], uint8_t *rssi, uint8_t *channel, char bssid[18], uint8_t *encryption)
{
    if(index > _scan_record_count)
        return -1;

    wifi_ap_record_t * ap = &_scan_records[index];

    if (ssid != nullptr)
        strncpy(ssid, (const char *)ap->ssid, 32);

    if (bssid != nullptr)
        _mac_to_string(bssid, ap->bssid);

    if (rssi != nullptr)
        *rssi = ap->rssi;
    if (channel != nullptr)
        *channel = ap->primary;
    if (encryption != nullptr)
        *encryption = ap->authmode;

    return 0;
}

std::string WiFiManager::get_current_ssid()
{
    wifi_ap_record_t apinfo;
    esp_err_t e = esp_wifi_sta_get_ap_info(&apinfo);

    if (ESP_OK == e)
        return std::string((char *)apinfo.ssid);

    return std::string();
}

int WiFiManager::get_mac(uint8_t mac[6])
{
    esp_err_t e = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    return e;
}

char *WiFiManager::_mac_to_string(char dest[18], uint8_t mac[6])
{
    if (dest != NULL)
        sprintf(dest, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return dest;
}

std::string WiFiManager::get_mac_str()
{
    std::string result;
    uint8_t mac[6];
    char macStr[18] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    result += _mac_to_string(macStr, mac);
    return result;
}

int WiFiManager::get_current_bssid(uint8_t bssid[6])
{
    wifi_ap_record_t apinfo;
    esp_err_t e = esp_wifi_sta_get_ap_info(&apinfo);

    if (ESP_OK == e)
        memcpy(bssid, apinfo.bssid, 6);

    return e;
}

std::string WiFiManager::get_current_bssid_str()
{
    wifi_ap_record_t apinfo;
    esp_err_t e = esp_wifi_sta_get_ap_info(&apinfo);

    if (ESP_OK == e)
    {
        char mac[18] = {0};
        return std::string(_mac_to_string(mac, apinfo.bssid));
    }

    return std::string();
}

void WiFiManager::_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                             int32_t event_id, void *event_data)
{
    Debug_printf("_wifi_event_handler base: %d event: %d\n", event_base, event_id);

    // Get a pointer to our fnWiFi object
    WiFiManager *pFnWiFi = (WiFiManager *)arg;
    esp_err_t e;
    __IGNORE_UNUSED_VAR(e);

    // IP_EVENT NOTIFICATIONS
    if(event_base == IP_EVENT)
    {
        switch (event_id)
        {
            case IP_EVENT_STA_GOT_IP:
                Debug_println("IP_EVENT_STA_GOT_IP");
                pFnWiFi->_connected = true;
                break;
            case IP_EVENT_STA_LOST_IP:
                Debug_println("IP_EVENT_STA_LOS_IP");
                break;
            case IP_EVENT_ETH_GOT_IP:
                Debug_println("IP_EVENT_ETH_GOT_IP");
                break;

        }

    }
    // WIFI_EVENT NOTIFICATIONS
    else if(event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_WIFI_READY:
            Debug_println("WIFI_EVENT_WIFI_READ");
            break;
        case WIFI_EVENT_SCAN_DONE:
            Debug_println("WIFI_EVENT_SCAN_DONE");
            break;
        case WIFI_EVENT_STA_START:
            Debug_println("WIFI_EVENT_STA_START");
            break;
        case WIFI_EVENT_STA_STOP:
            Debug_println("WIFI_EVENT_STA_STOP");
            break;
        case WIFI_EVENT_STA_CONNECTED:
            Debug_println("WIFI_EVENT_STA_CONNECTED");
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            Debug_println("WIFI_EVENT_STA_DISCONNECTED");
            pFnWiFi->_connected = false;
            break;
        case WIFI_EVENT_STA_AUTHMODE_CHANGE:
            Debug_println("WIFI_EVENT_STA_AUTHMODE_CHANGE");
            break;
        /*
        case SYSTEM_EVENT_STA_GOT_IP:
            Debug_printf("fnwifi_event_handler SYSTEM_EVENT_STA_GOT_IP: %s\n", ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
            //pFnWiFi->retries = 0;
            //xEventGroupSetBits(pFnWiFi->_wifi_event_group, FNWIFI_BIT_CONNECTED);
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            Debug_println("fnwifi_event_handler SYSTEM_EVENT_STA_DISCONNECTED");
            {
                if (pFnWiFi->retries < FNWIFI_RECONNECT_RETRIES)
                {
                    xEventGroupClearBits(pFnWiFi->_wifi_event_group, FNWIFI_BIT_CONNECTED);
                    esp_wifi_connect();
                    pFnWiFi->retries++;
                    Debug_printf("Attempting WiFi reconnect %d/%d", pFnWiFi->retries, FNWIFI_RECONNECT_RETRIES); 
                }
                Debug_println("Max WiFi reconnects exhausted");
                break;
            }
            break;
        */
        }

    }
}
