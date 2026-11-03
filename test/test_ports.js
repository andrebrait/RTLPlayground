/* Duplex changes must retain the speed command they modify when saving. */
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');
const source = fs.readFileSync('../html/app.js', 'utf8');
const context = vm.createContext({});
vm.runInContext(source.slice(source.indexOf('var CONF_CMDS = ['), source.indexOf('function setDirty(')), context);
vm.runInContext(source.slice(source.indexOf('var CONF_OVERWRITE = ['), source.indexOf('function writeConfig(')), context);
const plain = x => Array.from(x);
let saved = context.mergeConf(['port 1 100m full', 'port 8 auto'], ['port 1 duplex half']);
assert.deepEqual(plain(saved), ['port 1 100m full', 'port 8 auto', 'port 1 duplex half']);
saved = context.mergeConf(saved, ['port 1 duplex full']);
assert.deepEqual(plain(saved), ['port 1 100m full', 'port 8 auto', 'port 1 duplex full']);
saved = context.mergeConf(saved, ['port 1 auto']);
assert.deepEqual(plain(saved), ['port 8 auto', 'port 1 auto']);
console.log('Port speed/duplex configuration replay order passed');
