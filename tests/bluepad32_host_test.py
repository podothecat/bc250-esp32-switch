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
#include <atomic>
int bleConnectionHandlers=0;
extern "C" void __real_uni_bt_le_on_gap_event_advertising_report(const uint8_t*,uint16_t){++bleConnectionHandlers;}
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
struct Controller {bool gamepad=true,data=true;int idx=0;int index(){return idx;}bool isGamepad(){return gamepad;}bool hasData(){return data;}Properties getProperties(){return {};}std::string getModelName(){return "DualSense";}};
using ControllerPtr=Controller*;
using ControllerAdvertisement=void(*)(const char*,const char*,int,bool);
ControllerAdvertisement advertisementCallback=nullptr;
void controllerDiagBegin(ControllerAdvertisement callback){advertisementCallback=callback;}void controllerDiagPoll(bool){}
struct SerialFake {template<class... T>void printf(const char*,T...){}void println(const char*){}}Serial;
struct BP {std::function<void(ControllerPtr)> connected,disconnected;bool enabled=true;int setups=0,updates=0;void setup(auto a,auto b){connected=a;disconnected=b;++setups;}void update(){++updates;}}BP32;
bool uni_bt_enable_new_connections_is_enabled(){return BP32.enabled;}
void uni_bt_enable_new_connections_unsafe(bool v){BP32.enabled=v;}
int events=0;std::string address;void capture(const char*a,const char*,int,bool){++events;address=a;}
using BtHostIncoming=void(*)(const char*);
BtHostIncoming hostIncoming=nullptr;
void btHostSetIncoming(BtHostIncoming cb){hostIncoming=cb;}
bool spoofedStub=false;
bool btHostSpoofed(){return spoofedStub;}
'''
checks=r'''
int main(){
 uint8_t advertisement[12]{};
 __wrap_uni_bt_le_on_gap_event_advertising_report(advertisement,sizeof(advertisement));
 assert(bleConnectionHandlers==0); // no auto-connect before Arduino setup
 for(int i=0;i<4;++i)devices[i].conn.handle=i;
 controllerScanPrepare();assert(queue.size()==1&&BP32.setups==1);pump();assert(!BP32.enabled);
 Controller pad;BP32.connected(&pad);assert(events==0&&devices[0].releases==0);pump();assert(devices[0].releases==1&&devices[0].conn.connected&&devices[0].conn.handle==0);hciComplete(0);
 assert(controllerScanBegin(capture,true,true));clockMs+=499;controllerScanLoop();assert(queue.empty());
 ++clockMs;controllerScanLoop();assert(queue.size()==1);pump();assert(controllerScanRunning());
 __wrap_uni_bt_le_on_gap_event_advertising_report(advertisement,sizeof(advertisement));
 assert(bleConnectionHandlers==1); // setup delegates to Bluepad32
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
 // Bluepad32 can announce readiness before it has copied the first report.
 hciComplete(0);BP32.disconnected(&pad);
 devices[0].conn.connected=true;pad.gamepad=false;pad.data=false;
 int before=events,releases=devices[0].releases;
 BP32.connected(&pad);pump();assert(events==before&&devices[0].releases==releases);
 clockMs+=100;controllerScanLoop();pump();assert(devices[0].releases==releases);
 pad.gamepad=true;pad.data=true;controllerScanLoop();pump();
 assert(events==before+1&&devices[0].releases==releases+1);
 controllerScanLoop();pump();assert(events==before+1); // report only once
 hciComplete(0);BP32.disconnected(&pad);
 // A disconnect while waiting must not leave a stale pending controller.
 devices[0].conn.connected=true;pad.data=false;BP32.connected(&pad);
 hciComplete(0);BP32.disconnected(&pad);pad.data=true;
 controllerScanLoop();pump();assert(events==before+1);
 // A silent device eventually releases its slot without being listed.
 devices[0].conn.connected=true;pad.data=false;BP32.connected(&pad);
 clockMs+=10000;controllerScanLoop();pump();
 assert(events==before+1&&devices[0].releases==releases+2);
 hciComplete(0);BP32.disconnected(&pad);
 // Pausing discovery while waiting must never trigger a late wake.
 devices[0].conn.connected=true;BP32.connected(&pad);controllerScanStop();
 pad.data=true;controllerScanLoop();pump();
 assert(events==before+1&&devices[0].releases==releases+3);
 // Registered-address filtering belongs to main.cpp; raw BLE sightings must
 // reach it in normal mode even when no HID connection succeeds.
 int sightings=events;
 assert(controllerScanBegin(capture,false,true));
 clockMs+=500;controllerScanLoop();pump();assert(controllerScanRunning());
 __wrap_uni_bt_le_on_gap_event_advertising_report(advertisement,sizeof(advertisement));
 assert(bleConnectionHandlers==1); // normal scan never starts BLE HID
 advertisementCallback("0c:35:26:59:9a:6c","",-60,false);
 assert(events==sightings+1&&address=="0c:35:26:59:9a:6c"); // normal: gamepad flag ignored
 controllerScanStop();advertisementCallback("0c:35:26:59:9a:6c","",-60,false);
 assert(events==sightings+1);
 controllerScanBegin(capture,true,true);
 advertisementCallback("0c:35:26:59:9a:6c","",-60,true);
 assert(events==sightings+1); // non-spoofed picker lists HID-verified pads only
 spoofedStub=true;
 advertisementCallback("aa:bb:cc:dd:ee:ff","",-60,false);
 assert(events==sightings+1); // spoofed picker ignores non-gamepad advertisers
 advertisementCallback("aa:bb:cc:dd:ee:ff","Xbox Wireless Controller",-60,true);
 assert(events==sightings+2&&address=="aa:bb:cc:dd:ee:ff"); // spoofed picker lists gamepads by address
 spoofedStub=false;
 __wrap_uni_bt_le_on_gap_event_advertising_report(advertisement,sizeof(advertisement));
 assert(bleConnectionHandlers==2); // setup can pair again
 controllerScanStop();
 __wrap_uni_bt_le_on_gap_event_advertising_report(advertisement,sizeof(advertisement));
 assert(bleConnectionHandlers==2); // no late pairing while paused
 // A Classic page captured by the host-identity filter is reported as a Classic
 // sighting while scanning, and ignored once stopped.
 assert(hostIncoming);
 controllerScanBegin(capture,false,true);
 int base=events;
 hostIncoming("0a:1b:2c:3d:4e:5f");
 assert(events==base+1&&address=="0a:1b:2c:3d:4e:5f");
 controllerScanStop();
 hostIncoming("0a:1b:2c:3d:4e:5f");
 assert(events==base+1);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(shim+s+checks)
 subprocess.run(['c++','-std=c++20',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: BLE connection gate (startup/normal/setup/stop), advertisement delivery, asynchronous disconnect/scan collision, first report, restart')
