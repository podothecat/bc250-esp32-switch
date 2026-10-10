"""Run the actual early-boot adapter against a deterministic SDK shim."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = '\n'.join(line for line in (root / 'src/bt_host.cpp').read_text().splitlines()
                   if not line.startswith('#include'))
shim = r'''
#define BC250_BLUEPAD32 1
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
using BtHostIncoming = void(*)(const char*);
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
using esp_err_t = int; using esp_bt_mode_t = int; using nvs_handle_t = int;
constexpr int ESP_OK=0, NVS_READONLY=0, BOARD_SENSE=34, ADC_11db=3;
constexpr int SENSE_OVERSAMPLE=16, SENSE_HIGH_MV=2000, HCI_STATE_WORKING=2;
using bd_addr_t = uint8_t[6]; using hci_link_type_t = int;
struct btstack_timer_source_t { void(*handler)(btstack_timer_source_t*); };
std::string saved="24:12:34:56:00:00";
uint8_t force=0, ps=0, baseMac[6]{}, factory[6]={0x24,1,2,3,4,5};
int voltage=0, nvsError=0, macWrites=0, realRequests=0, acceptedCallbacks=0;
int connect=-1, discover=-1, scanWrites=0, state=0, addrReads=0;
bool bredr=true, attenuated=false, initialized=false;
int (*filter)(bd_addr_t,hci_link_type_t)=nullptr;
esp_err_t esp_efuse_mac_get_default(uint8_t*p){memcpy(p,factory,6);return 0;}
esp_err_t nvs_flash_init(){return nvsError;}
esp_err_t nvs_open(const char*,int,nvs_handle_t*h){*h=1;return 0;}
esp_err_t nvs_get_str(int,const char*,char*p,size_t*n){if(saved.size()+1>*n)return 1;strcpy(p,saved.c_str());return 0;}
esp_err_t nvs_get_u8(int,const char*k,uint8_t*p){*p=strcmp(k,"forceSetup")==0?force:ps;return 0;}
void nvs_close(int){}
void analogSetPinAttenuation(int,int){attenuated=true;}
int analogReadMilliVolts(int){assert(attenuated);return voltage;}
esp_err_t esp_base_mac_addr_set(const uint8_t*p){memcpy(baseMac,p,6);++macWrites;return 0;}
void bd_addr_copy(uint8_t*a,const uint8_t*b){memcpy(a,b,6);}
int bd_addr_cmp(const uint8_t*a,const uint8_t*b){return memcmp(a,b,6);}
void gap_connectable_control(uint8_t v){connect=v;++scanWrites;}
void gap_discoverable_control(uint8_t v){discover=v;++scanWrites;}
void gap_local_bd_addr(uint8_t*p){memcpy(p,baseMac,6);++addrReads;}
int hci_get_state(){return state;}
void btstack_run_loop_set_timer(btstack_timer_source_t*,int ms){assert(ms==200);}
void btstack_run_loop_add_timer(btstack_timer_source_t*){}
void btstack_run_loop_set_timer_handler(btstack_timer_source_t*t,void(*fn)(btstack_timer_source_t*)){t->handler=fn;}
void gap_register_classic_connection_filter(int(*fn)(bd_addr_t,hci_link_type_t)){assert(initialized);filter=fn;}
void uni_bt_bredr_set_enabled(bool v){bredr=v;}
void gap_set_local_name(const char*n){assert(strcmp(n,"BC250 Power Setup")==0);}
void gap_set_class_of_device(uint32_t v){assert(v==0x002508);}
void hci_event_connection_request_get_bd_addr(const uint8_t*p,uint8_t*a){for(int i=0;i<6;++i)a[i]=p[7-i];}
struct {template<class... T>void printf(const char*,T...){} } Serial;
extern "C" uint8_t __real_btstack_init(){initialized=true;return 7;}
extern "C" esp_err_t __real_esp_bt_controller_enable(esp_bt_mode_t){assert(macWrites==1);return 0;}
extern "C" void __real_uni_bt_bredr_setup(){connect=discover=1;}
extern "C" void __real_uni_bt_bredr_on_hci_connection_request(uint16_t,const uint8_t*,uint16_t){++realRequests;}
'''
checks = r'''
void capture(const char*a){assert(strcmp(a,"aa:bb:cc:dd:ee:ff")==0);++acceptedCallbacks;}
int main(int argc,char**argv){
 std::string scenario=argv[1];
 if(scenario=="board")voltage=3000;
 if(scenario=="setup")force=1;
 if(scenario=="ps"){force=1;ps=1;}
 if(scenario=="invalid")saved="zz:12:34:56:78:90";
 if(scenario=="multicast")saved="25:12:34:56:78:90";
 if(scenario=="borrow")saved="24:00:00:00:00:01";
 if(scenario=="nvs")nvsError=1;
 bool spoof=scenario=="normal"||scenario=="ps";
 assert(__wrap_btstack_init()==7);
 assert(btHostSpoofed()==spoof);
 assert(btHostIdentityReason()[0]);
 if(spoof){
   uint8_t expected[]={0x24,0x12,0x34,0x55,0xff,0xfe};
   assert(memcmp(baseMac,expected,6)==0&&!bredr);
   assert(__wrap_esp_bt_controller_enable(0)==0);
   assert(memcmp(baseMac,factory,6)==0&&macWrites==2);
 }else assert(macWrites==0);
 __wrap_uni_bt_bredr_setup();assert(connect==!spoof&&discover==0);
 uint8_t addr[]={0xaa,0xbb,0xcc,0xdd,0xee,0xff};
 uint8_t packet[]={4,10,0xff,0xee,0xdd,0xcc,0xbb,0xaa,0,0,0,1};
 btHostSetIncoming(capture);
 assert(filter(addr,1)==0); // every incoming Classic connection is rejected
 __wrap_uni_bt_bredr_on_hci_connection_request(0,packet,sizeof(packet));
 assert(realRequests==0&&acceptedCallbacks==0);
 int previous=scanWrites;
 btHostAllowIncoming(true);btHostSetLearning(true);
 assert(scanWrites==previous); // Public API must never call BTstack.
 timer.handler(&timer);assert(connect==1&&discover==!spoof);
 assert(filter(addr,1)==0);
 __wrap_uni_bt_bredr_on_hci_connection_request(0,packet,sizeof(packet));
 assert(realRequests==0&&acceptedCallbacks==spoof);
 // Only the address the filter just rejected is suppressed; any other incoming
 // request still reaches Bluepad32's handler.
 filter(addr,1);
 uint8_t other[]={4,10,6,5,4,3,2,1,0,0,0,1};
 __wrap_uni_bt_bredr_on_hci_connection_request(0,other,sizeof(other));
 assert(realRequests==1);
 char out[18];assert(btHostLearnedAddr(out)==!spoof);
 if(!spoof)assert(strcmp(out,"aa:bb:cc:dd:ee:ff")==0);
 btHostClearLearned();assert(!btHostLearnedAddr(out));
 btHostSetLearning(false);btHostAllowIncoming(false);
 timer.handler(&timer);assert(connect==!spoof&&discover==0);
 state=HCI_STATE_WORKING;timer.handler(&timer);timer.handler(&timer);assert(addrReads==1);
 btHostLogStatus();assert(addrReads==1);timer.handler(&timer);assert(addrReads==2);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(shim + source + checks)
    subprocess.run(['c++', '-std=c++17', str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    for scenario in ('normal', 'board', 'setup', 'ps', 'invalid', 'multicast', 'borrow', 'nvs'):
        subprocess.run([str(path / 'test'), scenario], check=True)
print('PASS: identity, MAC borrow/restore, rejection, learning, deferred scan and logging')
