#include "http_home_page.h"

#include <stdio.h>

#ifndef FLASHMEM
#if defined(__IMXRT1062__)
#define FLASHMEM __attribute__((section(".flashmem")))
#else
#define FLASHMEM
#endif
#endif

/* THE CONTROL SURFACE IS THE SD-HOSTED TREE, NOT THIS PAGE: this document is
 * the fallback for a card-absent board, and it says so in its own body. It
 * lives outside optical_main.cpp so the host browser contract exercises the
 * exact generator the device serves. */
FLASHMEM size_t aius_http_home_page(char *body, size_t capacity, const char *git_revision, const char *build_utc)
{
	int length = snprintf(body, capacity,
		"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Aius</title>"
		"<style>:root{color-scheme:dark}body{max-width:72rem;margin:0 auto;padding:1rem;background:#101217;color:#e8edf2;font:1rem/1.5 ui-monospace,monospace;overflow-wrap:anywhere}code{font:inherit}input,button{min-height:2.75rem;font:inherit}button,a:focus-visible,input:focus-visible{outline:3px solid #f4c95d;outline-offset:2px}a{color:#8bc5ff}pre{max-width:100%%;overflow:auto;white-space:pre-wrap}</style></head>"
		"<body data-mr-ui-contract=\"accessible-2026\"><h1>Aius optical firmware</h1><p>Git revision: %s<br>Build UTC: %s</p>"
		"<h2>Control surface</h2><p>The user interface is served from the SD card. This page is the fallback for when the card is absent.</p>"
		"<ul><li><a href=\"/sd/www/index.html\">DSP editor</a> - input trim and EQ, downmix, output EQ, limiter, live meters</li>"
		"<li><a href=\"/sd/www/replays.html\">Replay manager</a></li><li><a href=\"/sd/www/status.html\">Device status</a></li>"
		"<li><a href=\"/sd/www/\">Browse /sd/www/</a> - the served SD web tree</li></ul><p id=sd></p>"
		"<h2>API</h2><ul><li><a href=\"/api/status\">GET /api/status</a></li><li><a href=\"/api/events\">GET /api/events</a></li>"
		"<li><code>POST /api/start {}</code></li><li><code>POST /api/stop {}</code></li><li><code>POST /api/clear {}</code></li><li><code>POST /api/reboot {}</code></li>"
		"<li>Those four POST routes require <code>X-Aius-SD-Auth</code>.</li><li><a href=\"/api/sd\">GET /api/sd</a></li>"
		"<li><code>PUT /sd/www/{path}</code> raw body with Content-Length and X-Aius-SD-Auth</li><li><code>DELETE /sd/www/{path}</code> with X-Aius-SD-Auth</li>"
		"<li><code>PUT /api/config/public</code> JSON feature flags, no auth</li></ul>"
		"<h2>DSP configuration</h2><p><a href=\"/api/dsp/config\">GET /api/dsp/config</a> reports the single effective config: source, sample_rate_hz, features, input channels, output EQ, matrix, and limiter.</p><pre id=dsp>Loading DSP configuration...</pre>"
		"<h2>SD web upload</h2><p>Get the token over USB with <code>AIUS AUTH</code>. The picker preserves relative paths and sends authenticated standard HTTP PUT requests.</p>"
		"<label>SD auth token <input id=t size=16 maxlength=16 autocomplete=off></label> <input id=f type=file multiple webkitdirectory directory><button onclick=upload()>Upload selected files</button><pre id=u></pre>"
		"<h2>Control</h2><p>These four send the token from the field above.</p><p><button title=\"Enable test processing state\" onclick=go('/api/start')>Start</button> <button title=\"Disable test processing state\" onclick=go('/api/stop')>Stop</button> <button title=\"Clear audio and event counters\" onclick=go('/api/clear')>Clear counters</button> <button title=\"Restart the application without entering HalfKay\" onclick=reboot()>Reboot</button></p>"
		"<pre id=s>Loading status...</pre><p id=e></p></body><script>"
		"const s=document.getElementById('s'),e=document.getElementById('e'),u=document.getElementById('u'),d=document.getElementById('dsp');"
		"async function get(){try{const j=await (await fetch('/api/status')).json();s.textContent=JSON.stringify(j,null,2);document.getElementById('sd').textContent=j.sd_present?'':'No SD card is mounted, so the three links above will not resolve.';e.textContent=''}catch(a){e.textContent=a}}"
		"async function getDsp(){try{const r=await fetch('/api/dsp/config');if(!r.ok)throw Error('HTTP '+r.status);if(d)d.textContent=JSON.stringify(await r.json(),null,2)}catch(a){if(d)d.textContent='DSP model unavailable: '+a}}"
		"async function go(a,b){try{await fetch(a,{method:'POST',headers:{'Content-Type':'application/json','X-Aius-SD-Auth':document.getElementById('t').value||'-'},body:JSON.stringify(b||{})});get()}catch(c){e.textContent=c}}"
		"async function reboot(){e.textContent='Rebooting...';try{await go('/api/reboot');setTimeout(get,2000)}catch(a){}}"
		"async function upload(){u.textContent='';const token=document.getElementById('t').value;const files=[...(document.getElementById('f').files||[])];for(const file of files){const rel=file.webkitRelativePath||file.name;const path=rel.split('/').slice(1).join('/')||rel;if(path.split('/').pop()==='config.json'){u.textContent+='refused '+path+'\\n';continue}try{const r=await fetch('/sd/www/'+path.split('/').map(encodeURIComponent).join('/'),{method:'PUT',headers:{'X-Aius-SD-Auth':token,'Content-Type':'application/octet-stream'},body:file});u.textContent+=r.status+' '+path+'\\n'}catch(a){u.textContent+='error '+path+' '+a+'\\n'}}}"
		"get();getDsp();setInterval(get,2000);setInterval(getDsp,2000)</script></html>",
		git_revision, build_utc);
	return length < 0 || (size_t)length >= capacity ? 0u : (size_t)length;
}
