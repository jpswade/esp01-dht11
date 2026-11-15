const fs = require('fs');
const vm = require('vm');
const assert = require('assert');
const source = fs.readFileSync(__dirname + '/../HumiditySensor/HumiditySensor.ino', 'utf8');
const script = source.match(/<script>([\s\S]*?)<\/script>/)[1]
  .replace('refresh();refreshHistory();', '');
const elements = new Map();
function element() {
  return {children: [], attrs: {}, textContent: '',
    replaceChildren() { this.children = []; },
    append(child) { this.children.push(child); },
    setAttribute(key, value) { this.attrs[key] = String(value); }};
}
const document = {
  getElementById(id) { if (!elements.has(id)) elements.set(id, element()); return elements.get(id); },
  createElementNS() { return element(); }
};
let clock = 0;
const context = vm.createContext({document, Date, Number, Math, Object,
  performance: {now: () => clock}, setInterval() {}});
vm.runInContext(script, context);
function draw(points) {
  context.chart('chart', 'range', points, 'humidity_percent', '% RH', '#147b80', Date.now());
  const children = elements.get('chart').children;
  for (const child of children)
    for (const value of Object.values(child.attrs)) assert(!/NaN|Infinity/.test(value));
  return children;
}
draw([]);
assert(elements.get('chart').children[0].textContent.includes('No valid readings'));
draw([{age_ms: 0, humidity_percent: 0}]);
draw([{age_ms: 0, humidity_percent: 100}]);
const children = draw([
  {age_ms: 900000, humidity_percent: 43},
  {age_ms: 600000, humidity_percent: null},
  {age_ms: 300000, humidity_percent: 45},
  {age_ms: 0, humidity_percent: 46}
]);
const path = children.find(child => child.attrs.d !== undefined).attrs.d;
assert.equal((path.match(/M/g) || []).length, 2, 'Missing sample must break the line');
assert(elements.get('range').textContent.includes('43.0% RH'));
console.log('PASS: empty chart, boundary humidity values, missing-data gaps, range labels');

vm.runInContext('deviceUptime=90061000;uptimeReceivedAt=0;showUptime();',context);
assert.equal(elements.get('uptime').textContent,'Uptime 1d 1h 1m 1s');
clock=2000;
context.showUptime();
assert.equal(elements.get('uptime').textContent,'Uptime 1d 1h 1m 3s');
clock=16000;
context.showUptime();
assert.equal(elements.get('uptime').textContent,'Uptime 1d 1h 1m 1s (last confirmed)');
vm.runInContext('deviceUptime=2000;uptimeReceivedAt=performance.now();showUptime();',context);
assert.equal(elements.get('uptime').textContent,'Uptime 0h 0m 2s');
console.log('PASS: uptime formatting, ticking, stale readings and restart resynchronisation');
