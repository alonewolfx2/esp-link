/*
Cgi/template routines for the /wifi url.
*/

/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain
 * this notice you can do whatever you want with this stuff. If we meet some day,
 * and you think this stuff is worth it, you can buy me a beer in return.
 * ----------------------------------------------------------------------------
 * Heavily modified and enhanced by Thorsten von Eicken in 2015
 * ----------------------------------------------------------------------------
 */


#include <esp8266.h>
#include "cgiwifi.h"
#include "cgi.h"
#include "status.h"
#include "log.h"

//Enable this to disallow any changes in AP settings
//#define DEMO_MODE

#define SLEEP_MODE LIGHT_SLEEP_T

// ===== wifi status change callback

uint8_t wifiState = wifiIsDisconnected;
// reasons for which a connection failed
uint8_t wifiReason = 0;
static char *wifiReasons[] = {
	"", "unspecified", "auth_expire", "auth_leave", "assoc_expire", "assoc_toomany", "not_authed",
	"not_assoced", "assoc_leave", "assoc_not_authed", "disassoc_pwrcap_bad", "disassoc_supchan_bad",
	"ie_invalid", "mic_failure", "4way_handshake_timeout", "group_key_update_timeout",
	"ie_in_4way_differs", "group_cipher_invalid", "pairwise_cipher_invalid", "akmp_invalid",
	"unsupp_rsn_ie_version", "invalid_rsn_ie_cap", "802_1x_auth_failed", "cipher_suite_rejected",
	"beacon_timeout", "no_ap_found" };

static char *wifiMode[] = { 0, "STA", "AP", "AP+STA" };
static char *wifiPhy[]  = { 0, "11b", "11g", "11n" };

static char* ICACHE_FLASH_ATTR wifiGetReason(void) {
	if (wifiReason <= 24) return wifiReasons[wifiReason];
	if (wifiReason >= 200 && wifiReason <= 201) return wifiReasons[wifiReason-200+25];
	return wifiReasons[1];
}

// handler for wifi status change callback coming in from espressif library
static void ICACHE_FLASH_ATTR wifiHandleEventCb(System_Event_t *evt) {
	switch (evt->event) {
	case EVENT_STAMODE_CONNECTED:
		wifiState = wifiIsConnected;
		wifiReason = 0;
		os_printf("Wifi connected to ssid %s, ch %d\n", evt->event_info.connected.ssid,
				evt->event_info.connected.channel);
		statusWifiUpdate(wifiState);
		break;
	case EVENT_STAMODE_DISCONNECTED:
		wifiState = wifiIsDisconnected;
		wifiReason = evt->event_info.disconnected.reason;
		os_printf("Wifi disconnected from ssid %s, reason %s\n",
				evt->event_info.disconnected.ssid, wifiGetReason());
		statusWifiUpdate(wifiState);
		break;
	case EVENT_STAMODE_AUTHMODE_CHANGE:
		os_printf("Wifi auth mode: %d -> %d\n",
				evt->event_info.auth_change.old_mode, evt->event_info.auth_change.new_mode);
		break;
	case EVENT_STAMODE_GOT_IP:
		wifiState = wifiGotIP;
		wifiReason = 0;
		os_printf("Wifi got ip:" IPSTR ",mask:" IPSTR ",gw:" IPSTR "\n",
				IP2STR(&evt->event_info.got_ip.ip), IP2STR(&evt->event_info.got_ip.mask),
				IP2STR(&evt->event_info.got_ip.gw));
		statusWifiUpdate(wifiState);
		break;
	case EVENT_SOFTAPMODE_STACONNECTED:
		os_printf("Wifi AP: station " MACSTR " joined, AID = %d\n",
				MAC2STR(evt->event_info.sta_connected.mac), evt->event_info.sta_connected.aid);
		break;
	case EVENT_SOFTAPMODE_STADISCONNECTED:
		os_printf("Wifi AP: station " MACSTR " left, AID = %d\n",
				MAC2STR(evt->event_info.sta_disconnected.mac), evt->event_info.sta_disconnected.aid);
		break;
	default:
		break;
	}
}

// ===== wifi scanning

//WiFi access point data
typedef struct {
	char ssid[32];
	sint8 rssi;
	char enc;
} ApData;

//Scan result
typedef struct {
	char scanInProgress; //if 1, don't access the underlying stuff from the webpage.
	ApData **apData;
	int noAps;
} ScanResultData;

//Static scan status storage.
static ScanResultData cgiWifiAps;

//Callback the code calls when a wlan ap scan is done. Basically stores the result in
//the cgiWifiAps struct.
void ICACHE_FLASH_ATTR wifiScanDoneCb(void *arg, STATUS status) {
	int n;
	struct bss_info *bss_link = (struct bss_info *)arg;

	if (status!=OK) {
		os_printf("wifiScanDoneCb status=%d\n", status);
		cgiWifiAps.scanInProgress=0;
		return;
	}

	//Clear prev ap data if needed.
	if (cgiWifiAps.apData!=NULL) {
		for (n=0; n<cgiWifiAps.noAps; n++) os_free(cgiWifiAps.apData[n]);
		os_free(cgiWifiAps.apData);
	}

	//Count amount of access points found.
	n=0;
	while (bss_link != NULL) {
		bss_link = bss_link->next.stqe_next;
		n++;
	}
	//Allocate memory for access point data
	cgiWifiAps.apData=(ApData **)os_malloc(sizeof(ApData *)*n);
	cgiWifiAps.noAps=n;
	os_printf("Scan done: found %d APs\n", n);

	//Copy access point data to the static struct
	n=0;
	bss_link = (struct bss_info *)arg;
	while (bss_link != NULL) {
		if (n>=cgiWifiAps.noAps) {
			//This means the bss_link changed under our nose. Shouldn't happen!
			//Break because otherwise we will write in unallocated memory.
			os_printf("Huh? I have more than the allocated %d aps!\n", cgiWifiAps.noAps);
			break;
		}
		//Save the ap data.
		cgiWifiAps.apData[n]=(ApData *)os_malloc(sizeof(ApData));
		cgiWifiAps.apData[n]->rssi=bss_link->rssi;
		cgiWifiAps.apData[n]->enc=bss_link->authmode;
		strncpy(cgiWifiAps.apData[n]->ssid, (char*)bss_link->ssid, 32);
		os_printf("bss%d: %s (%d)\n", n+1, (char*)bss_link->ssid, bss_link->rssi);

		bss_link = bss_link->next.stqe_next;
		n++;
	}
	//We're done.
	cgiWifiAps.scanInProgress=0;
}

static ETSTimer scanTimer;
static void ICACHE_FLASH_ATTR scanStartCb(void *arg) {
	os_printf("Starting a scan\n");
	wifi_station_scan(NULL, wifiScanDoneCb);
}

static int ICACHE_FLASH_ATTR cgiWiFiStartScan(HttpdConnData *connData) {
	jsonHeader(connData, 200);
	if (!cgiWifiAps.scanInProgress) {
		cgiWifiAps.scanInProgress = 1;
		os_timer_disarm(&scanTimer);
		os_timer_setfn(&scanTimer, scanStartCb, NULL);
		os_timer_arm(&scanTimer, 1000, 0);
	}
	return HTTPD_CGI_DONE;
}

static int ICACHE_FLASH_ATTR cgiWiFiGetScan(HttpdConnData *connData) {
	char buff[2048];
	int len;

	jsonHeader(connData, 200);

	if (cgiWifiAps.scanInProgress==1) {
		//We're still scanning. Tell Javascript code that.
		len = os_sprintf(buff, "{\n \"result\": { \n\"inProgress\": \"1\"\n }\n}\n");
		httpdSend(connData, buff, len);
		return HTTPD_CGI_DONE;
	}

	len = os_sprintf(buff, "{\"result\": {\"inProgress\": \"0\", \"APs\": [\n");
	for (int pos=0; pos<cgiWifiAps.noAps; pos++) {
		len += os_sprintf(buff+len, "{\"essid\": \"%s\", \"rssi\": %d, \"enc\": \"%d\"}%s\n",
				cgiWifiAps.apData[pos]->ssid, cgiWifiAps.apData[pos]->rssi,
				cgiWifiAps.apData[pos]->enc, (pos==cgiWifiAps.noAps-1)?"":",");
	}
	len += os_sprintf(buff+len, "]}}\n");
	//os_printf("Sending %d bytes: %s\n", len, buff);
	httpdSend(connData, buff, len);
	return HTTPD_CGI_DONE;
}

int ICACHE_FLASH_ATTR cgiWiFiScan(HttpdConnData *connData) {
	if (connData->requestType == HTTPD_METHOD_GET) {
		return cgiWiFiGetScan(connData);
	} else if (connData->requestType == HTTPD_METHOD_POST) {
		return cgiWiFiStartScan(connData);
	} else {
		jsonHeader(connData, 404);
		return HTTPD_CGI_DONE;
	}
}

// ===== timers to change state and rescue from failed associations

#if 0
static ETSTimer deepTimer;
static void ICACHE_FLASH_ATTR deepSleepCb(void *arg) {
	system_deep_sleep_set_option(2);
	system_deep_sleep(100*1000);
}
#endif

//#define CONNTRY_IDLE 0
//#define CONNTRY_WORKING 1
//#define CONNTRY_SUCCESS 2
//#define CONNTRY_FAIL 3
//Connection result var
//static int connTryStatus=CONNTRY_IDLE;

// reset timer changes back to STA+AP if we can't associate
#define RESET_TIMEOUT (15000) // 15 seconds
static ETSTimer resetTimer;

//This routine is ran some time after a connection attempt to an access point. If
//the connect succeeds, this gets the module in STA-only mode.
static void ICACHE_FLASH_ATTR resetTimerCb(void *arg) {
	int x = wifi_station_get_connect_status();
	int m = wifi_get_opmode() & 0x3;
	os_printf("Wifi check: mode=%s status=%d\n", wifiMode[m], x);

	if (x == STATION_GOT_IP) {
		if (m != 1) {
			// We're happily connected, go to STA mode
			os_printf("Wifi got IP. Going into STA mode..\n");
			wifi_set_opmode(1);
			wifi_set_sleep_type(SLEEP_MODE);
			os_timer_arm(&resetTimer, RESET_TIMEOUT, 0);
			//os_timer_disarm(&deepTimer);
			//os_timer_setfn(&deepTimer, deepSleepCb, NULL);
			//os_timer_arm(&deepTimer, 1000, 1);
		}
		log_uart(false);
		// no more resetTimer at this point, gotta use physical reset to recover if in trouble
	} else {
		if (m != 3) {
			os_printf("Wifi connect failed. Going into STA+AP mode..\n");
			wifi_set_opmode(3);
		}
		log_uart(true);
		os_printf("Enabling/continuing uart log\n");
		os_timer_arm(&resetTimer, RESET_TIMEOUT, 0);
	}
}

// Temp store for new ap info.
static struct station_config stconf;
// Reassociate timer to delay change of association so the original request can finish
static ETSTimer reassTimer;

// Callback actually doing reassociation
static void ICACHE_FLASH_ATTR reassTimerCb(void *arg) {
	os_printf("Wifi changing association\n");
	wifi_station_disconnect();
	wifi_station_set_config(&stconf);
	wifi_station_connect();
	// Schedule check
	os_timer_disarm(&resetTimer);
	os_timer_setfn(&resetTimer, resetTimerCb, NULL);
	os_timer_arm(&resetTimer, RESET_TIMEOUT, 0);
}

// This cgi uses the routines above to connect to a specific access point with the
// given ESSID using the given password.
int ICACHE_FLASH_ATTR cgiWiFiConnect(HttpdConnData *connData) {
	char essid[128];
	char passwd[128];

	if (connData->conn==NULL) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}

	int el = httpdFindArg(connData->getArgs, "essid", essid, sizeof(essid));
	int pl = httpdFindArg(connData->getArgs, "passwd", passwd, sizeof(passwd));

	if (el > 0 && pl >= 0) {
		//Set to 0 if you want to disable the actual reconnecting bit
#ifndef DEMO_MODE
		os_strncpy((char*)stconf.ssid, essid, 32);
		os_strncpy((char*)stconf.password, passwd, 64);
		os_printf("Wifi try to connect to AP %s pw %s\n", essid, passwd);

		//Schedule disconnect/connect
		os_timer_disarm(&reassTimer);
		os_timer_setfn(&reassTimer, reassTimerCb, NULL);
		os_timer_arm(&reassTimer, 1000, 0);
#endif
		jsonHeader(connData, 200);
	} else {
		jsonHeader(connData, 400);
		httpdSend(connData, "Cannot parse ssid or password", -1);
	}
	return HTTPD_CGI_DONE;
}

//This cgi changes the operating mode: STA / AP / STA+AP
int ICACHE_FLASH_ATTR cgiWiFiSetMode(HttpdConnData *connData) {
	int len;
	char buff[1024];

	if (connData->conn==NULL) {
		// Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}

	len=httpdFindArg(connData->getArgs, "mode", buff, sizeof(buff));
	if (len!=0) {
		int m = atoi(buff);
		os_printf("Wifi switching to mode %d\n", m);
#ifndef DEMO_MODE
		wifi_set_opmode(m&3);
		if (m == 1) {
			wifi_set_sleep_type(SLEEP_MODE);
			// STA-only mode, reset into STA+AP after a timeout
			os_timer_disarm(&resetTimer);
			os_timer_setfn(&resetTimer, resetTimerCb, NULL);
			os_timer_arm(&resetTimer, RESET_TIMEOUT, 0);
		}
		jsonHeader(connData, 200);
#endif
	} else {
	  jsonHeader(connData, 400);
	}
	return HTTPD_CGI_DONE;
}

static char *connStatuses[] = { "idle", "connecting", "wrong password", "AP not found",
                         "failed", "got IP address" };

static char *wifiWarn[] = { 0,
	"Switch to <a href=\\\"#\\\" onclick=\\\"changeWifiMode(3)\\\">STA+AP mode</a>",
	"<b>Can't scan in this mode!</b> Switch to <a href=\\\"#\\\" onclick=\\\"changeWifiMode(3)\\\">STA+AP mode</a>",
	"Switch to <a href=\\\"#\\\" onclick=\\\"changeWifiMode(1)\\\">STA mode</a>",
};

// print various Wifi information into json buffer
int ICACHE_FLASH_ATTR printWifiInfo(char *buff) {
	int len;

	struct station_config stconf;
	wifi_station_get_config(&stconf);

	uint8_t op = wifi_get_opmode() & 0x3;
	char *mode = wifiMode[op];
	char *status = "unknown";
	int st = wifi_station_get_connect_status();
	if (st > 0 && st < sizeof(connStatuses)) status = connStatuses[st];
	int p = wifi_get_phy_mode();
	char *phy = wifiPhy[p&3];
	char *warn = wifiWarn[op];
	sint8 rssi = wifi_station_get_rssi();
	if (rssi > 0) rssi = 0;
	uint8 mac_addr[6];
	wifi_get_macaddr(0, mac_addr);

	len = os_sprintf(buff,
		"\"mode\": \"%s\", \"ssid\": \"%s\", \"status\": \"%s\", \"phy\": \"%s\", "
		"\"rssi\": \"%ddB\", \"warn\": \"%s\", \"passwd\": \"%s\", "
		"\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"",
		mode, (char*)stconf.ssid, status, phy, rssi, warn, (char*)stconf.password,
		mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

	struct ip_info info;
	if (wifi_get_ip_info(0, &info)) {
		len += os_sprintf(buff+len, ", \"ip\": \"%d.%d.%d.%d\"",
			(info.ip.addr>>0)&0xff, (info.ip.addr>>8)&0xff,
			(info.ip.addr>>16)&0xff, (info.ip.addr>>24)&0xff);
	} else {
		len += os_sprintf(buff+len, ", \"ip\": \"-none-\"");
	}

	return len;
}

int ICACHE_FLASH_ATTR cgiWiFiConnStatus(HttpdConnData *connData) {
	char buff[1024];
	int len;
	int st=wifi_station_get_connect_status();

	jsonHeader(connData, 200);

	len = os_sprintf(buff, "{\"status\": \"%s\", ",
			st > 0 && st < sizeof(connStatuses) ? connStatuses[st] : "unknown");

	len += printWifiInfo(buff+len);
	len += os_sprintf(buff+len, ", ");

	if (wifiReason != 0) {
		len += os_sprintf(buff+len, "\"reason\": \"%s\", ", wifiGetReason());
	}

	if (st == STATION_GOT_IP) {
		if (wifi_get_opmode() != 1) {
			//Reset into AP-only mode sooner.
			os_timer_disarm(&resetTimer);
			os_timer_setfn(&resetTimer, resetTimerCb, NULL);
			os_timer_arm(&resetTimer, 1000, 0);
		}
	}

	len += os_sprintf(buff+len, "\"x\":0}\n");

	os_printf("  -> %s\n", buff);
	httpdSend(connData, buff, len);
	return HTTPD_CGI_DONE;
}

// Cgi to return various Wifi information
int ICACHE_FLASH_ATTR cgiWifiInfo(HttpdConnData *connData) {
	char buff[1024];

	if (connData->conn==NULL) {
		return HTTPD_CGI_DONE; // Connection aborted
	}

	os_strcpy(buff, "{");
	printWifiInfo(buff+1);
	os_strcat(buff, "}");

	jsonHeader(connData, 200);
	httpdSend(connData, buff, -1);
	return HTTPD_CGI_DONE;
}

// Init the wireless, which consists of setting a timer if we expect to connect to an AP
// so we can revert to STA+AP mode if we can't connect.
void ICACHE_FLASH_ATTR wifiInit() {
	wifi_station_set_hostname("esp-link");
	int x = wifi_get_opmode() & 0x3;
	os_printf("Wifi init, mode=%s\n", wifiMode[x]);
	wifi_set_phy_mode(2);
	wifi_set_event_handler_cb(wifiHandleEventCb);
	// check on the wifi in a few seconds to see whether we need to switch mode
	os_timer_disarm(&resetTimer);
	os_timer_setfn(&resetTimer, resetTimerCb, NULL);
	os_timer_arm(&resetTimer, RESET_TIMEOUT, 0);
}

