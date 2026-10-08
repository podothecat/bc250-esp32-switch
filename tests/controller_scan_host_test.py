"""Host regression checks for WROOM GAP events and portal discovery cache.
Run: python3 tests/controller_scan_host_test.py (requires c++ and node).
Hardware/radio behavior must still be tested with the actual controllers.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/controller_scan.cpp').read_text()
# Compile actual WROOM implementation with fake SDK calls, not a reimplementation.
source = source.split('#if defined(CONFIG_IDF_TARGET_ESP32)')[0] + source.split(
    '#if defined(CONFIG_IDF_TARGET_ESP32)')[1].split('\n#else')[0]
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
shim = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
using ControllerFound=void(*)(const char*,const char*,int,bool);
struct ControllerScanStatus {bool classicSupported,bleRunning,classicRunning;int bleError,classicError;};
bool controllerScanStart();
const int BLE_SCAN_INTERVAL_COEX_MS=320,BLE_SCAN_WINDOW_COEX_MS=48;
const int BLE_SCAN_INTERVAL_SOLO_MS=160,BLE_SCAN_WINDOW_SOLO_MS=80;
uint32_t clockMs=100;uint32_t millis(){return clockMs;}
using esp_err_t=int;using esp_gap_ble_cb_event_t=int;using esp_bt_gap_cb_event_t=int;
enum {ESP_OK=0,ESP_FAIL=-1,ESP_BT_STATUS_SUCCESS=0,
 BT_MODE_BTDM=1,ESP_BLUEDROID_STATUS_UNINITIALIZED=0,ESP_BLUEDROID_STATUS_INITIALIZED=1,
 ESP_BLUEDROID_STATUS_ENABLED=2,ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT=10,
 ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT,
 ESP_GAP_BLE_SCAN_RESULT_EVT,ESP_GAP_SEARCH_INQ_RES_EVT,ESP_GAP_SEARCH_INQ_CMPL_EVT,
 ESP_BT_GAP_DISC_STATE_CHANGED_EVT,ESP_BT_GAP_DISCOVERY_STARTED,ESP_BT_GAP_DISC_RES_EVT,
 ESP_BT_GAP_DEV_PROP_RSSI,ESP_BT_GAP_DEV_PROP_BDNAME,ESP_BT_GAP_DEV_PROP_EIR,
 ESP_BT_NON_CONNECTABLE,ESP_BT_NON_DISCOVERABLE,BLE_SCAN_TYPE_ACTIVE,BLE_SCAN_TYPE_PASSIVE,
 BLE_ADDR_TYPE_PUBLIC,BLE_SCAN_FILTER_ALLOW_ALL,BLE_SCAN_DUPLICATE_DISABLE,
 ESP_BT_INQ_MODE_GENERAL_INQUIRY};
struct Completion {int status=0;};
struct esp_ble_gap_cb_param_t {
 Completion scan_param_cmpl,scan_start_cmpl,scan_stop_cmpl;
 struct {int search_evt=0;uint8_t bda[6]={};uint8_t ble_adv[62]={};uint8_t adv_data_len=0,scan_rsp_len=0;int rssi=-50;}scan_rst;
};
struct Property {int type,len;void *val;};
struct esp_bt_gap_cb_param_t {
 struct {int state;}disc_st_chg;
 struct {uint8_t bda[6]={};int num_prop;Property *prop;}disc_res;
};
struct esp_ble_scan_params_t {int scan_type,own_addr_type,scan_filter_policy,scan_interval,scan_window,scan_duplicate;};
int bleRequests=0,classicRequests=0,classicFailure=0,bleFailure=0;
esp_ble_scan_params_t lastParams{};
bool btStartMode(int){return true;}
int hostStatus=0;int esp_bluedroid_get_status(){return hostStatus;}
int esp_bluedroid_init(){hostStatus=1;return 0;}int esp_bluedroid_enable(){hostStatus=2;return 0;}
int esp_ble_gap_register_callback(void(*)(int,esp_ble_gap_cb_param_t*)){return 0;}
int esp_bt_gap_register_callback(void(*)(int,esp_bt_gap_cb_param_t*)){return 0;}
int esp_bt_gap_set_scan_mode(int,int){return 0;}
int esp_ble_gap_start_scanning(int){return 0;}int esp_ble_gap_stop_scanning(){return 0;}
int esp_ble_gap_set_scan_params(esp_ble_scan_params_t *p){++bleRequests;lastParams=*p;return bleFailure;}
int esp_bt_gap_start_discovery(int,int duration,int responses){assert(duration==4&&responses==0);++classicRequests;return classicFailure;}
int esp_bt_gap_cancel_discovery(){return 0;}
'''
tests = r'''
int sightings=0;std::string seenAddr,seenName;bool seenClassic;int seenRssi;
void capture(const char*a,const char*n,int r,bool c){++sightings;seenAddr=a;seenName=n;seenRssi=r;seenClassic=c;}
int main(){
 char name[5]={};uint8_t adv[]={2,8,'S',4,9,'P','a','d'};
 readName(adv,sizeof(adv),name,sizeof(name));assert(!strcmp(name,"Pad"));
 uint8_t bad[]={8,9,'x'};readName(bad,sizeof(bad),name,sizeof(name));assert(!strcmp(name,"Pad"));
 assert(controllerScanBegin(capture,true,true));assert(bleRequests==1&&classicRequests==1);
 assert(lastParams.scan_type==BLE_SCAN_TYPE_ACTIVE&&lastParams.scan_interval==800&&lastParams.scan_window==72);
 esp_ble_gap_cb_param_t ble{};bleCallback(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT,&ble);
 bleCallback(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,&ble);assert(controllerScanStatus().bleRunning);
 ble.scan_rst.search_evt=ESP_GAP_SEARCH_INQ_RES_EVT;ble.scan_rst.bda[5]=42;
 uint8_t packet[]={4,9,'P','a','d'};memcpy(ble.scan_rst.ble_adv,packet,sizeof(packet));ble.scan_rst.adv_data_len=sizeof(packet);
 bleCallback(ESP_GAP_BLE_SCAN_RESULT_EVT,&ble);assert(sightings==1&&seenAddr=="00:00:00:00:00:2a"&&seenName=="Pad"&&!seenClassic);
 esp_bt_gap_cb_param_t bt{};bt.disc_res.bda[5]=43;int8_t rssi=-60;
 Property props[]={{ESP_BT_GAP_DEV_PROP_BDNAME,18,(void*)"Wireless Controller"},{ESP_BT_GAP_DEV_PROP_RSSI,1,&rssi}};
 bt.disc_res.num_prop=2;bt.disc_res.prop=props;
 classicCallback(ESP_BT_GAP_DISC_RES_EVT,&bt);assert(sightings==2&&seenClassic&&seenRssi==-60&&seenAddr=="00:00:00:00:00:2b");
 clockMs=200;bt.disc_st_chg.state=0;classicCallback(ESP_BT_GAP_DISC_STATE_CHANGED_EVT,&bt);
 clockMs=3199;controllerScanLoop();assert(classicRequests==1);
 classicFailure=-5;clockMs=3200;controllerScanLoop();assert(classicRequests==2&&controllerScanStatus().classicError==-5);
 clockMs=3201;controllerScanLoop();assert(classicRequests==2);
 classicFailure=0;clockMs=6200;controllerScanLoop();assert(classicRequests==3);
 assert(controllerScanStop());assert(!controllerScanRunning());
 classicCallback(ESP_BT_GAP_DISC_RES_EVT,&bt);bleCallback(ESP_GAP_BLE_SCAN_RESULT_EVT,&ble);assert(sightings==2);
 bleCallback(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT,&ble);bt.disc_st_chg.state=0;classicCallback(ESP_BT_GAP_DISC_STATE_CHANGED_EVT,&bt);
 assert(controllerScanStart());assert(controllerScanRunning());
 ble.scan_start_cmpl.status=7;bleCallback(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,&ble);assert(!controllerScanStatus().bleRunning);
 clockMs+=3000;controllerScanLoop();assert(bleRequests==3);
}
'''
with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    (tmp / 'test.cpp').write_text(shim + source + tests)
    subprocess.run(['c++', '-std=c++11', str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True)
    portal = (ROOT / 'src/portal.cpp').read_text()
    cache = portal[portal.index('struct BleDev {'):portal.index('// --- WiFi network list')]
    callback = portal[portal.index('static void setupControllerFound'):portal.index('// --- Helpers ---')]
    main = (ROOT / 'src/main.cpp').read_text()
    wake = main[main.index('static void controllerFound'):main.index('static bool bleScanning')]
    cache_shim = r"""
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
struct {unsigned wakeAddrCount=4;std::string wakeAddrs[4]={
 "aa:bb:cc:dd:ee:01","aa:bb:cc:dd:ee:02","aa:bb:cc:dd:ee:03","aa:bb:cc:dd:ee:04"};}config;
unsigned long bleLastSeen=0;bool bleSeenEver=false;
struct {void printf(const char*,...){}} Serial;
"""
    cache_tests = r"""
int count(){int n=0;for(auto &d:devs)n+=d.used;return n;}
bool has(unsigned n){char addr[18];snprintf(addr,sizeof(addr),"aa:bb:cc:dd:ee:%02x",n);for(auto &d:devs)if(d.used&&!strcmp(d.addr,addr))return true;return false;}
void see(unsigned n,bool classic){char addr[18];snprintf(addr,sizeof(addr),"aa:bb:cc:dd:ee:%02x",n);setupControllerFound(addr,"pad",-50,classic);}
int main(){
 for(unsigned i=0;i<48;++i){clockMs=100+i;see(i,i%2);}assert(count()==48);
 clockMs=200;see(0,false);clockMs=201;see(48,true);
 assert(count()==48&&has(48)&&has(0)&&!has(1)&&bleEvictions==1);
 setupControllerFound("aa:bb:cc:dd:ee:31","",-127,true);
 for(auto &d:devs)if(d.used&&!strcmp(d.addr,"aa:bb:cc:dd:ee:31"))assert(d.rssi==-127);
 expireBleDevices(30201);assert(count()==0);
 clockMs=UINT32_MAX-100;see(0,true);expireBleDevices(100);assert(count()==1);expireBleDevices(30000);assert(count()==0);
 clockMs=100;
 controllerFound("aa:bb:cc:dd:ee:ff","",-50,true);assert(!bleSeenEver);
 for(unsigned i=1;i<=4;++i){char addr[18];snprintf(addr,sizeof(addr),"aa:bb:cc:dd:ee:%02x",i);bleSeenEver=false;controllerFound(addr,"",-50,i%2);assert(bleSeenEver&&bleLastSeen==clockMs);}
}
"""
    (tmp / 'cache.cpp').write_text(shim + cache_shim + cache + callback + wake + cache_tests)
    subprocess.run(['c++', '-std=c++11', str(tmp / 'cache.cpp'), '-o', str(tmp / 'cache')], check=True)
    subprocess.run([str(tmp / 'cache')], check=True)
    service = portal[portal.index('static void servicePortalBleScan()'):portal.index('static void handleBleSelect(')]
    service_shim = r"""
int clients=0,starts=0,stops=0,begins=0;bool scannerRunning=false;
struct {int softAPgetStationNum(){return clients;}} WiFi;
void controllerScanLoop(){}
bool controllerScanRunning(){return scannerRunning;}
bool controllerScanBegin(ControllerFound,bool,bool){++begins;return scannerRunning=true;}
bool controllerScanStart(){++starts;return scannerRunning=true;}
bool controllerScanStop(){++stops;scannerRunning=false;return true;}
"""
    service_tests = r"""
int main(){
 clockMs=10000;servicePortalBleScan();assert(begins==0&&starts==0&&stops==0);
 blePickerRequested=true;blePickerLastRequest=clockMs;servicePortalBleScan();assert(begins==0); // no AP client
 clients=1;servicePortalBleScan();assert(begins==1&&bleScannerRunning);
 clients=0;servicePortalBleScan();assert(stops==1&&!bleScannerRunning);
 clients=1;clockMs+=3000;blePickerLastRequest=clockMs;servicePortalBleScan();assert(starts==1&&bleScannerRunning);
 clockMs+=12000;servicePortalBleScan();assert(stops==2&&!bleScannerRunning);
 blePickerLastRequest=clockMs;bleRescanRequested=true;devs[0].used=true;servicePortalBleScan();assert(starts==2&&!devs[0].used&&!bleRescanRequested);
 assert(config.wakeAddrCount==4); // rescan does not modify registrations
 clockMs=UINT32_MAX-100;blePickerLastRequest=clockMs;servicePortalBleScan();
 clockMs=50;servicePortalBleScan();assert(bleScannerRunning); // picker lease survives millis rollover
}
"""
    service_mock = cache_shim.replace('struct {void printf(const char*,...){}} Serial;',
        'struct {void printf(const char*,...){} void println(const char*){}} Serial;')
    (tmp / 'service.cpp').write_text(shim + service_mock + cache + callback + service_shim + service + service_tests)
    subprocess.run(['c++', '-std=c++11', str(tmp / 'service.cpp'), '-o', str(tmp / 'service')], check=True)
    subprocess.run([str(tmp / 'service')], check=True)
    js = (ROOT / 'app/index.html').read_text().split('<script>')[1].split('</script>')[0]
    (tmp / 'portal.js').write_text(js)
    subprocess.run(['node', '--check', str(tmp / 'portal.js')], check=True)
print('PASS: names/TLV bounds, BLE/Classic sightings, scan timing, failure retry, stop/restart, mixed cache expiry/eviction, all four wake addresses, deferred picker scan/pause/resume, portal JS syntax')
