"""Exercise the actual probe scheduler/event parser without Bluetooth hardware."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/controller_name_probe.cpp').read_text().split('void setup()')[0]
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#'))
shim = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
using bd_addr_t=uint8_t[6]; using hci_link_type_t=int; using uni_error_t=int;
const int UNI_ERROR_IGNORE_DEVICE=5, MAX_WAKE_CONTROLLERS=4;
const int HCI_EVENT_PACKET=4, HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE=7;
struct btstack_timer_source_t{};
struct btstack_packet_callback_registration_t{void(*callback)(uint8_t,uint16_t,uint8_t*,uint16_t);};
struct btstack_context_callback_registration_t{void(*callback)(void*);void*context;};
uint32_t now=0; uint32_t millis(){return now;}
struct {template<class... T> void printf(const char*,T...){} void println(const char*){}} Serial;
void hci_event_remote_name_request_complete_get_bd_addr(uint8_t*p,bd_addr_t a){memcpy(a,p+3,6);}
uint8_t hci_event_remote_name_request_complete_get_status(uint8_t*p){return p[2];}
const char*bd_addr_to_str(const bd_addr_t){return "test";}
int requests=0,result=0;
int gap_remote_name_request(const bd_addr_t,uint8_t mode,uint16_t offset){assert(mode==2&&offset==0);++requests;return result;}
extern "C" void __real_uni_bt_bredr_setup(){}
void gap_register_classic_connection_filter(int(*)(bd_addr_t,hci_link_type_t)){}
void gap_set_bondable_mode(int n){assert(n==0);}
void gap_connectable_control(int n){assert(n==0);}
void gap_set_page_timeout(int n){assert(n==0x1000);}
void uni_bt_enable_new_connections_unsafe(bool n){assert(!n);}
void hci_add_event_handler(btstack_packet_callback_registration_t*){}
void btstack_run_loop_set_timer(btstack_timer_source_t*,int){}
void btstack_run_loop_add_timer(btstack_timer_source_t*){}
void btstack_run_loop_set_timer_handler(btstack_timer_source_t*,void(*)(btstack_timer_source_t*)){}
'''
checks = r'''
int main(){
 assert(rejectConnection(currentTarget,1)==0);
 assert(__wrap_uni_hid_device_on_device_discovered(currentTarget,"pad",0,0)==UNI_ERROR_IGNORE_DEVICE);
 startProbe(nullptr); tick(nullptr); assert(requests==0);
 targetCount=2; targets[0][0]=0x24; targets[1][0]=0x0c;
 now=2000; tick(nullptr); assert(requests==1&&pending&&currentTarget[0]==0x24);
 now+=1000; tick(nullptr); assert(requests==1); // never overlap
 uint8_t packet[10]={7,8,0,0x0c,0,0,0,0,0,'X'};
 observed(4,0,packet,10); assert(pending); // unrelated response
 packet[3]=0x24;
 observed(4,0,packet,8); assert(pending); // truncated
 observed(4,0,packet,10); assert(!pending); // non-NUL name is bounded
 tick(nullptr); assert(requests==1);
 now+=1000; tick(nullptr); assert(requests==2&&currentTarget[0]==0x0c);
 packet[3]=0x0c; packet[2]=4; observed(4,0,packet,9); assert(!pending);
 now+=1000; result=12; tick(nullptr); assert(requests==3&&!pending);
 now+=1000; result=0; tick(nullptr); assert(requests==4&&pending);
 now+=15000; tick(nullptr); assert(stalled&&requests==4);
 now+=20000; tick(nullptr); assert(requests==4); // watchdog never starts a second request
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(shim + source + checks)
    subprocess.run(['c++', '-std=c++17', str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: no pairing, delayed start, serialized round-robin, address/length checks, errors, watchdog')
