/* Isolated Prometheus card renderer and saved-command filter; no browser. */
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const src=fs.readFileSync('html/app.js','utf8');
const render=src.slice(src.indexOf('var promPending ='),src.indexOf('$("sy-prom-toggle").addEventListener('));
const conf=src.slice(src.indexOf('var CONF_CMDS ='),src.indexOf('];',src.indexOf('var CONF_CMDS ='))+2);
const nodes={};const $=id=>nodes[id]??={style:{},textContent:'',checked:false,disabled:false};
const context={$,location:{host:'192.0.2.9'}};vm.createContext(context);
vm.runInContext(render+conf+';this.CONF_CMDS=CONF_CMDS;',context);
context.renderPrometheus(1);
assert.equal($('sy-prom').style.display,'');assert.equal($('sy-prom-toggle').checked,true);
assert.equal($('sy-prom-url').textContent,'http://192.0.2.9/metrics');
context.renderPrometheus(0);
assert.equal($('sy-prom-toggle').checked,false);assert.equal($('sy-prom-url').textContent,'');
context.renderPrometheus(undefined);assert.equal($('sy-prom').style.display,'none');
const saved=c=>context.CONF_CMDS.some(r=>r.test(c));
assert.ok(saved('prometheus on')&&saved('prometheus off')&&!saved('prometheus maybe')&&!saved('prometheus show'));
console.log('PASS Prometheus card: toggle state, scrape URL, hidden without firmware support; on/off saved');
/* Save to flash keeps one "prometheus" line however often it is toggled. */
const mc={};vm.createContext(mc);
vm.runInContext(src.slice(src.indexOf('var CONF_CMDS ='),src.indexOf('function setDirty(')),mc);
vm.runInContext(src.slice(src.indexOf('var CONF_OVERWRITE ='),src.indexOf('function writeConfig(')),mc);
const once=mc.mergeConf(['ip 192.0.2.10','prometheus on'],['prometheus off\nprometheus on\nprometheus off']);
assert.deepEqual(once,['ip 192.0.2.10','prometheus off']);
assert.deepEqual(mc.mergeConf(once,['prometheus on']),['ip 192.0.2.10','prometheus on']);
console.log('PASS Prometheus on/off: saves keep one line, the latest setting wins');
