// Exercise the production link callbacks with deterministic IDF responses; no radio.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/links.c"
TickType_t mock_ticks;
int mock_discovers, mock_connects, mock_disconnects, mock_scans, mock_rssi_reads;
esp_err_t mock_connect_result;
static int up_count;
esp_err_t bridge_config_load(bridge_slot_t slot, bridge_device_t *out) { return ESP_OK; }
esp_err_t bridge_config_save(bridge_slot_t slot, const bridge_device_t *d) { return ESP_OK; }
esp_err_t bridge_config_forget(bridge_slot_t slot) { return ESP_OK; }
const char *bridge_config_slot_name(bridge_slot_t slot) { return slot ? "2" : "1"; }
uint8_t pipeline_link_up(bridge_slot_t slot) { up_count++; return 1; }
void pipeline_link_down(bridge_slot_t slot) {}
void pipeline_spp_write_done(bridge_slot_t slot,bool ok,bool cong) {}
void pipeline_spp_uncongested(bridge_slot_t slot) {}
void ble_server_send_control(const char *line) {}
static char output[8192];
static void reply(const char *line) { strlcat(output,line,sizeof(output)); strlcat(output,"\n",sizeof(output)); }
static void reset(void) {
 memset(s_links,0,sizeof(s_links)); memset(&s_cache,0,sizeof(s_cache));
 s_dialing=-1; s_dial_handle=0; s_dial_cancelled=false; s_dial_warned=false; s_next_slot=0;
 s_scan_pending=false; s_scan_running=false; s_name_next=-1; s_scan_reply=NULL;
 s_scan_seen_count=0; mock_ticks=0; mock_discovers=mock_connects=mock_disconnects=mock_scans=mock_rssi_reads=up_count=0; s_rssi_polled=0;
 mock_connect_result=ESP_OK; output[0]=0;
 s_links[0].device.valid=true; s_links[0].device.mac[5]=1;
 s_links[1].device.valid=true; s_links[1].device.mac[5]=2;
}
static void discovered(void) {
 esp_spp_cb_param_t p={0}; p.disc_comp.scn_num=1; p.disc_comp.scn[0]=7;
 spp_cb(ESP_SPP_DISCOVERY_COMP_EVT,&p);
}
static void opened(uint32_t h) {
 esp_spp_cb_param_t p={0}; p.cl_init.handle=h; spp_cb(ESP_SPP_CL_INIT_EVT,&p);
 memset(&p,0,sizeof(p)); p.open.handle=h; memcpy(p.open.rem_bda,s_dial_mac,6); spp_cb(ESP_SPP_OPEN_EVT,&p);
}
int main(void) {
 reset(); manager_tick(); assert(mock_discovers==1 && s_dialing==0);
 mock_ticks=31000; manager_tick(); assert(s_dialing==0 && mock_discovers==1 && s_dial_warned);
 discovered(); opened(11); assert(s_links[0].state==LINK_CONNECTED && up_count==1);
 esp_spp_cb_param_t p={0}; p.close.handle=11; p.close.async=true; spp_cb(ESP_SPP_CLOSE_EVT,&p);
 s_links[1].device.valid=false; mock_ticks+=5001; manager_tick();
 assert(mock_connects==2 && mock_discovers==1); // reuse successful RFCOMM channel
 p=(esp_spp_cb_param_t){0}; p.cl_init.status=1; spp_cb(ESP_SPP_CL_INIT_EVT,&p);
 assert(s_dialing==-1 && s_links[0].scn==0); // initialization failure is terminal
 mock_ticks+=5001; manager_tick(); assert(mock_discovers==2);
 mock_connect_result=ESP_FAIL; discovered(); assert(s_dialing==-1); // immediate API failure
 reset(); manager_tick(); bridge_device_t replacement={.valid=true,.mac={0,0,0,0,0,3}};
 links_set_device(0,&replacement); discovered(); assert(mock_connects==0 && s_dialing==-1); // old SDP cannot dial new MAC
 reset(); manager_tick(); discovered(); opened(22); links_scan(reply);
 assert(mock_disconnects==1 && mock_scans==0); // wait for actual close before inquiry
 p=(esp_spp_cb_param_t){0}; p.close.handle=22; spp_cb(ESP_SPP_CLOSE_EVT,&p);
 assert(mock_scans==1);
 reset(); bridge_seen_device_t d={.mac={0,0,0,0,0,1},.rssi=-80,.cod=0x1f00,.spp=true,.seen_ms=1000};
 strcpy(d.name,"instrument"); bridge_cache_observe(&s_cache,&d); mock_ticks=6000; links_cached(reply);
 assert(strstr(output,"devices begin 1\n") && strstr(output,"seen 00:00:00:00:00:01 5000\n"));
 assert(mock_scans==0 && mock_disconnects==0 && mock_connects==0); // periodic replay never touches radio
 d.seen_ms=7000; d.rssi=-60; d.name[0]=0; d.spp=false; bridge_cache_observe(&s_cache,&d);
 assert(s_cache.count==1 && s_cache.devices[0].spp && !strcmp(s_cache.devices[0].name,"instrument"));
 for (int i=2;i<=25;i++) { d.mac[5]=i; d.seen_ms=7000+i; bridge_cache_observe(&s_cache,&d); }
 assert(s_cache.count==24); uint8_t old[6]={0,0,0,0,0,1}; assert(!bridge_cache_find(&s_cache,old));
 reset(); manager_tick(); discovered();
 p=(esp_spp_cb_param_t){0}; p.cl_init.handle=40; spp_cb(ESP_SPP_CL_INIT_EVT,&p);
 p=(esp_spp_cb_param_t){0}; p.close.handle=39; spp_cb(ESP_SPP_CLOSE_EVT,&p);
 assert(s_dialing==0 && s_links[0].state==LINK_CONNECTING); // unrelated close cannot abort current attempt
 p=(esp_spp_cb_param_t){0}; p.close.handle=40; spp_cb(ESP_SPP_CLOSE_EVT,&p);
 manager_tick(); assert(s_dialing==1); // second slot gets a turn after first failure
 reset(); s_scan_running=true; s_scan_seen_count=2; s_name_next=2;
 s_scan_seen[0].bda[5]=1; s_scan_seen[1].bda[5]=2;
 esp_bt_gap_cb_param_t gap={0}; gap.read_rmt_name.bda[5]=1;
 name_result(&gap); assert(s_scan_running && s_name_next==2); // late name cannot advance the next peer
 reset(); s_scan_running=true;
 int8_t rssi=-75; esp_bt_gap_dev_prop_t prop={.type=ESP_BT_GAP_DEV_PROP_RSSI,.val=&rssi};
 gap=(esp_bt_gap_cb_param_t){0}; gap.disc_res.bda[5]=1; gap.disc_res.num_prop=1; gap.disc_res.prop=&prop;
 scan_result(&gap); prop=(esp_bt_gap_dev_prop_t){.type=ESP_BT_GAP_DEV_PROP_BDNAME,.len=5,.val="Laser"};
 scan_result(&gap); assert(s_cache.count==1 && s_cache.devices[0].rssi==-75 && !strcmp(s_cache.devices[0].name,"Laser"));
 reset(); mock_ticks=1000; manager_tick(); discovered(); opened(50); links_info_t info;
 links_info(0,&info); assert(!info.has_rssi); // no reading yet
 mock_ticks=4000; manager_tick(); assert(mock_rssi_reads==1); // only the connected slot is polled
 gap=(esp_bt_gap_cb_param_t){0}; gap.read_rssi_delta.bda[5]=1; gap.read_rssi_delta.rssi_delta=-7;
 gap_cb(ESP_BT_GAP_READ_RSSI_DELTA_EVT,&gap); links_info(0,&info); assert(info.has_rssi && info.rssi_delta==-7);
 mock_ticks=4500; manager_tick(); assert(mock_rssi_reads==1); // at most every 2 s
 mock_ticks=15000; links_info(0,&info); assert(!info.has_rssi); // stale after 10 s
 p=(esp_spp_cb_param_t){0}; p.close.handle=50; spp_cb(ESP_SPP_CLOSE_EVT,&p);
 gap_cb(ESP_BT_GAP_READ_RSSI_DELTA_EVT,&gap); links_info(0,&info); assert(!info.has_rssi); // late answer after close ignored
 puts("links/cache: 12 scenarios passed");
}
