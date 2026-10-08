"""Exercise the actual adapter with asynchronous BT-task commands."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/controller_scan.cpp').read_text()
s=s.split('#if defined(BC250_BLUEPAD32)')[0]+s.split('#if defined(BC250_BLUEPAD32)')[1].split('#elif')[0]
s='\n'.join(x for x in s.splitlines() if not x.startswith('#include'))
shim=r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>
#include <functional>
#include <vector>
#include <algorithm>
uint32_t clockMs=0;uint32_t millis(){return clockMs;}void delay(uint32_t ms){clockMs+=ms;}
using ControllerFound=void(*)(const char*,const char*,int,bool);
struct ControllerScanStatus {bool classicSupported,bleRunning,classicRunning;int bleError,classicError;};
bool controllerScanStart();
#define BP32_MAX_CONTROLLERS 4
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
struct btstack_context_callback_registration_t {void(*callback)(void*);void*context;};
std::vector<btstack_context_callback_registration_t*> queue;
void btstack_run_loop_execute_on_main_thread(btstack_context_callback_registration_t*r){assert(std::find(queue.begin(),queue.end(),r)==queue.end());queue.push_back(r);}
void pump(){while(!queue.empty()){auto*r=queue.front();queue.erase(queue.begin());r->callback(r->context);}}
struct Device {struct {bool connected=true;int handle=0;} conn;int releases=0;};
Device devices[4];Device*uni_hid_device_get_instance_for_idx(int i){return &devices[i];}
// ACL close requests preserve both handle and connected state until HCI ends.
void gap_disconnect(int handle){++devices[handle].releases;}
void hciComplete(int i){assert(devices[i].conn.handle==i);devices[i].conn.connected=false;}
struct Properties {uint8_t btaddr[6]={1,2,3,4,5,6};};
struct Controller {bool gamepad=true;int idx=0;int index(){return idx;}bool isGamepad(){return gamepad;}Properties getProperties(){return {};}std::string getModelName(){return "DualSense";}};
using ControllerPtr=Controller*;
struct SerialFake {template<class... T>void printf(const char*,T...){}void println(const char*){}}Serial;
struct BP {std::function<void(ControllerPtr)> connected;bool enabled=true;int setups=0,updates=0;void setup(auto a,auto){connected=a;++setups;}void update(){++updates;}}BP32;
bool uni_bt_enable_new_connections_is_enabled(){return BP32.enabled;}
void uni_bt_enable_new_connections_unsafe(bool v){BP32.enabled=v;}
int events=0;std::string address;void capture(const char*a,const char*,int r,bool){++events;address=a;assert(r==-127);}
'''
checks=r'''
int main(){
 for(int i=0;i<4;++i)devices[i].conn.handle=i;
 controllerScanPrepare();assert(queue.size()==1&&BP32.setups==1);pump();assert(!BP32.enabled);
 Controller pad;BP32.connected(&pad);assert(events==0&&devices[0].releases==0);pump();assert(devices[0].releases==1&&devices[0].conn.connected&&devices[0].conn.handle==0);hciComplete(0);
 assert(controllerScanBegin(capture,true,true));clockMs+=499;controllerScanLoop();assert(queue.empty());
 ++clockMs;controllerScanLoop();assert(queue.size()==1);pump();assert(controllerScanRunning());
 devices[0].conn.connected=true;BP32.connected(&pad);BP32.connected(&pad);assert(events==2&&address=="01:02:03:04:05:06"&&queue.size()==1);
 // Same loop turn: disconnect and discovery stop must use separate objects.
 controllerScanStop();assert(queue.size()==2);pump();assert(devices[0].releases==2&&!BP32.enabled);hciComplete(0);
 // Device mapping remains available until the real HCI disconnect event.
 assert(uni_hid_device_get_instance_for_idx(0)==&devices[0]);
 devices[0].conn.connected=true;BP32.connected(&pad);pump();assert(events==2&&devices[0].releases==3);
 Controller mouse;mouse.gamepad=false;mouse.idx=1;BP32.connected(&mouse);pump();assert(events==2&&devices[1].releases==1);
 assert(controllerScanStart());clockMs+=500;controllerScanLoop();pump();assert(controllerScanRunning());
 devices[0].conn.connected=true;BP32.connected(&pad);pump();assert(events==3&&devices[0].releases==4);
 // A pending opposite request cannot overwrite its callback context.
 controllerScanStop();controllerScanStart();clockMs+=500;controllerScanLoop();pump();assert(!BP32.enabled);
 clockMs+=500;controllerScanLoop();pump();assert(BP32.enabled);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(shim+s+checks)
 subprocess.run(['c++','-std=c++20',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: asynchronous disconnect/scan collision, duplicate enqueue suppression, retained mapping, paused wake, restart, settle timing')
