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
#include "httpd.h"
#include "httpdespfs.h"
#include "cgi.h"
#include "cgiwifi.h"
#include "cgipins.h"
#include "cgiflash.h"
#include "auth.h"
#include "espfs.h"
#include "uart.h"
#include "serbridge.h"
#include "status.h"
#include "serled.h"
#include "console.h"
#include "config.h"
#include "log.h"
#define MCU_RESET 12
#define MCU_ISP   13
#include <gpio.h>

//#define SHOW_HEAP_USE

//Function that tells the authentication system what users/passwords live on the system.
//This is disabled in the default build; if you want to try it, enable the authBasic line in
//the builtInUrls below.
int myPassFn(HttpdConnData *connData, int no, char *user, int userLen, char *pass, int passLen) {
	if (no==0) {
		os_strcpy(user, "admin");
		os_strcpy(pass, "s3cr3t");
		return 1;
//Add more users this way. Check against incrementing no for each user added.
//	} else if (no==1) {
//		os_strcpy(user, "user1");
//		os_strcpy(pass, "something");
//		return 1;
	}
	return 0;
}


/*
This is the main url->function dispatching data struct.
In short, it's a struct with various URLs plus their handlers. The handlers can
be 'standard' CGI functions you wrote, or 'special' CGIs requiring an argument.
They can also be auth-functions. An asterisk will match any url starting with
everything before the asterisks; "*" matches everything. The list will be
handled top-down, so make sure to put more specific rules above the more
general ones. Authorization things (like authBasic) act as a 'barrier' and
should be placed above the URLs they protect.
*/
HttpdBuiltInUrl builtInUrls[]={
	{"/", cgiRedirect, "/home.tpl"},
	{"/flash/next", cgiGetFirmwareNext, NULL},
	{"/flash/upload", cgiUploadFirmware, NULL},
	{"/flash/reboot", cgiRebootFirmware, NULL},
	{"/home.tpl", cgiEspFsHtml, NULL},
	//{"/help.tpl", cgiEspFsTemplate, tplCounter},
	{"/log.tpl", cgiEspFsHtml, NULL},
	{"/log/text", ajaxLog, NULL},
	{"/console.tpl", cgiEspFsHtml, NULL},
	{"/console/reset", ajaxConsoleReset, NULL},
	{"/console/baud", ajaxConsoleBaud, NULL},
	{"/console/text", ajaxConsole, NULL},
	{"/help.tpl", cgiEspFsHtml, NULL},

	//Routines to make the /wifi URL and everything beneath it work.

//Enable the line below to protect the WiFi configuration with an username/password combo.
//	{"/wifi/*", authBasic, myPassFn},

	{"/wifi", cgiRedirect, "/wifi/wifi.tpl"},
	{"/wifi/", cgiRedirect, "/wifi/wifi.tpl"},
	{"/wifi/wifi.tpl", cgiEspFsHtml, NULL},
	{"/wifi/info", cgiWifiInfo, NULL},
	{"/wifi/scan", cgiWiFiScan, NULL},
	{"/wifi/connect", cgiWiFiConnect, NULL},
	{"/wifi/connstatus", cgiWiFiConnStatus, NULL},
	{"/wifi/setmode", cgiWiFiSetMode, NULL},
	{"/pins", cgiPins, NULL},

	{"*", cgiEspFsHook, NULL}, //Catch-all cgi function for the filesystem
	{NULL, NULL, NULL}
};


#ifdef SHOW_HEAP_USE
static ETSTimer prHeapTimer;

static void ICACHE_FLASH_ATTR prHeapTimerCb(void *arg) {
	os_printf("Heap: %ld\n", (unsigned long)system_get_free_heap_size());
}
#endif

void user_rf_pre_init(void) {
}

// address of espfs binary blob
extern uint32_t _binary_espfs_img_start;

static char *rst_codes[] = {
	"normal", "wdt reset", "exception", "soft wdt", "restart", "deep sleep", "???",
};

//Main routine. Initialize stdout, the I/O, filesystem and the webserver and we're done.
void user_init(void) {
	// init gpio pins used to reset&reprogram attached microcontrollers
	gpio_init();
	// put MCU into reset in case it interferes with serial-programming of the esp8266
	//GPIO_OUTPUT_SET(MCU_RESET, 0);
	// init UART
	uart_init(BIT_RATE_115200, BIT_RATE_115200);
	// say hello (leave some time to cause break in TX after boot loader's msg
	os_delay_us(10000L);
# define VERS_STR_STR(V) #V
# define VERS_STR(V) VERS_STR_STR(V)
	os_printf("\n\nInitializing esp-link\n" VERS_STR(VERSION) "\n");
	//configWipe();
	if (configRestore()) os_printf("Flash config restored\n");
	else os_printf("*** Flash config restore failed, using defaults ***\n");
	// Status LEDs
	statusInit();
	serledInit();
	logInit();
	// Wifi
	wifiInit();
	// init the flash filesystem with the html stuff
	EspFsInitResult res = espFsInit(&_binary_espfs_img_start);
	os_printf("espFsInit(0x%08lx) returned %d\n", (uint32_t)&_binary_espfs_img_start, res);
	// mount the http handlers
	httpdInit(builtInUrls, 80);
	// init the wifi-serial transparent bridge (port 23)
	serbridgeInit(23);
	uart_add_recv_cb(&serbridgeUartCb);
#ifdef SHOW_HEAP_USE
	os_timer_disarm(&prHeapTimer);
	os_timer_setfn(&prHeapTimer, prHeapTimerCb, NULL);
	os_timer_arm(&prHeapTimer, 3000, 1);
#endif

	struct rst_info *rst_info = system_get_rst_info();
	os_printf("Reset cause: %d=%s\n", rst_info->reason, rst_codes[rst_info->reason]);
	os_printf("exccause=%d epc1=0x%x epc2=0x%x epc3=0x%x excvaddr=0x%x depc=0x%x\n",
			rst_info->exccause, rst_info->epc1, rst_info->epc2, rst_info->epc3,
			rst_info->excvaddr, rst_info->depc);

	os_printf("** esp-link ready\n");
}
