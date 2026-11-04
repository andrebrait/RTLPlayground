/* Save to flash leaves out settings at their board default: pruneDefaults() of html/app.js. */
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const src=fs.readFileSync('html/app.js','utf8'),context={};
vm.createContext(context);
vm.runInContext(src.slice(src.indexOf('function pruneDefaults('),src.indexOf('function writeConfig(')),context);
const prune=(conf,d)=>Array.from(context.pruneDefaults(conf,d));
const D={lldp:0,ports:[{portNum:1,lldp:1,en:1,eee:1},{portNum:2,lldp:1,en:1,eee:1},{portNum:9,lldp:1}]};

assert.deepEqual(prune(['port 1 off','port 1 on'],D),[]);
assert.deepEqual(prune(['port 1 on','port 1 off'],D),['port 1 on','port 1 off']);
assert.deepEqual(prune(['port 1 off','port 1 1g','port 1 on'],D),['port 1 off','port 1 1g','port 1 on']);
assert.deepEqual(prune(['port 1 off full','port 1 on'],D),['port 1 off full','port 1 on']);
assert.deepEqual(prune(['port 1 on half','port 1 off','port 1 on'],D),['port 1 on half','port 1 off','port 1 on']);
assert.deepEqual(prune(['eee off 1','eee on 1'],{lldp:0,ports:[{portNum:1,lldp:1,en:1}]}),['eee off 1','eee on 1']);
assert.deepEqual(prune(['lldp on extra','lldp off'],D),['lldp on extra','lldp off']);
assert.deepEqual(prune(['port 1 lldp block extra','port 1 lldp permit'],D),['port 1 lldp block extra','port 1 lldp permit']);
assert.deepEqual(prune(['port 01 off','port 1 on'],D),['port 01 off','port 1 on']);
assert.deepEqual(prune(['eee off 01','eee on 1'],D),['eee off 01','eee on 1']);
assert.deepEqual(prune(['eee off 1','eee on 1','eee off 2'],D),['eee off 2']);
assert.deepEqual(prune(['eee off','eee on 1'],D),['eee off','eee on 1']);
assert.deepEqual(prune(['lldp on','lldp off','port 2 lldp block','port 2 lldp permit'],D),[]);
assert.deepEqual(prune(['port 9 off','port 1 fc on','mtu 1 16368'],D),['port 9 off','port 1 fc on','mtu 1 16368']);
assert.deepEqual(prune(['port 1 on'],null),['port 1 on']);
console.log('PASS settings at their board default are left out, with every earlier line for them');
