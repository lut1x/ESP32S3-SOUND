const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const script = fs.readFileSync(path.join(__dirname, '../main/index.html'), 'utf8')
  .match(/<script>([\s\S]*?)<\/script>/)[1];

function element(tag = '') {
  return {
    tag, disabled: false, hidden: false, textContent: '', files: [], children: [], value: '',
    append(...items) { this.children.push(...items); },
    replaceChildren(...items) { this.children = items; },
    querySelectorAll(tag) {
      return this.children.flatMap(child => [
        ...(child.tag === tag ? [child] : []), ...child.querySelectorAll(tag),
      ]);
    },
  };
}
async function browser(options = {}) {
  const elements = new Map();
  const get = id => {
    if (!elements.has(id)) elements.set(id, element());
    return elements.get(id);
  };
  const state = {
    mounted: true, detected: true, formatting: false, uploading: false,
    playing: false, paused: false, capacity_mb: 32000, bus_width: 4,
    volume: 25, position: 0, duration: 0, ...options.state,
  };
  state.lines = options.state?.lines || [
    { id: 1, track: 'test.wav', volume: 25, playing: state.playing, paused: state.paused, autoplay: state.playing, loops: 0 },
    { id: 2, track: '', volume: 25, playing: false, paused: false, autoplay: false, loops: 0 },
  ];
  state.idle = options.state?.idle ?? !state.lines.some(line => line.playing);
  const calls = [], transfers = [];
  class XHR {
    constructor() { this.upload = {}; this.headers = {}; transfers.push(this); }
    open(method, url) { this.method = method; this.url = url; }
    setRequestHeader(name, value) { this.headers[name] = value; }
    send(file) {
      this.body = file;
      this.upload.onprogress({ lengthComputable: true, loaded: 50, total: 100 });
      if (options.pending) return;
      queueMicrotask(() => {
        this.status = options.uploadStatus || 201;
        this.responseText = JSON.stringify(this.status === 201
          ? { ok: true, file: file.name } : { error: 'Файл уже существует' });
        this.onload();
      });
    }
    abort() { this.onabort(); }
  }
  const context = vm.createContext({
    document: { getElementById: get, createElement: element, activeElement: null },
    fetch: async (url, init) => {
      calls.push({ url, init });
      if (url === '/api/control') {
        const body=JSON.parse(init.body),line=state.lines[(body.line||1)-1];
        if (body.action === 'format') state.formatting = true;
        if (body.action === 'play') {line.track=body.file;line.playing=true;line.autoplay=true;line.paused=false}
        if (body.action === 'stop') {line.playing=false;line.autoplay=false;line.paused=false}
        if (body.action === 'pause') line.paused=!line.paused;
        if (body.action === 'volume') line.volume=body.value;
        state.idle=!state.lines.some(line=>line.playing);
        return { ok: true, json: async () => ({ ok: true }) };
      }
      return { ok: true, json: async () => url === '/api/status'
        ? JSON.parse(JSON.stringify(state)) : { files: options.files || ['test.wav', 'Тест 1.wav', 'a.wav'], other_files: options.otherFiles || [] } };
    },
    prompt: () => options.answer ?? null,
    XMLHttpRequest: XHR,
    setInterval() {},
  });
  vm.runInContext(script, context);
  await new Promise(resolve => setImmediate(resolve));
  return { get, state, calls, transfers, poll: context.status };
}

test('paused playback blocks formatting and uploading', async () => {
  const b = await browser({ state: { playing: true, paused: true } });
  b.get('upload-file').files = [{ name: 'a.wav' }];
  b.get('upload-file').onchange();
  assert.equal(b.get('format').disabled, true);
  assert.equal(b.get('upload').disabled, true);
});
test('unmounted but detected card can be formatted, not uploaded to', async () => {
  const b = await browser({ state: { mounted: false } });
  assert.equal(b.get('format').disabled, false);
  assert.equal(b.get('upload').disabled, true);
});
test('cancelled format confirmation sends no command', async () => {
  const b = await browser({ answer: 'нет' });
  await b.get('format').onclick();
  assert.equal(b.calls.filter(c => c.init?.method === 'POST').length, 0);
});
test('confirmed format sends explicit confirmation and locks controls', async () => {
  const b = await browser({ answer: 'СТЕРЕТЬ' });
  await b.get('format').onclick();
  const body = JSON.parse(b.calls.find(c => c.url === '/api/control').init.body);
  assert.deepEqual(body, { action: 'format', confirmation: 'ERASE_SD_CARD' });
  assert.equal(b.get('format').disabled, true);
  assert.equal(b.get('refresh').disabled, true);
});
test('upload sends raw file and encoded filename, then reports success', async () => {
  const b = await browser();
  const file = { name: 'Тест 1.wav', size: 1024 };
  b.get('upload-file').files = [file]; b.get('upload-file').onchange();
  await b.get('upload').onclick();
  const xhr = b.transfers[0];
  assert.equal(xhr.url, '/api/upload');
  assert.equal(xhr.headers['X-Filename'], encodeURIComponent(file.name));
  assert.equal(xhr.body, file);
  assert.equal(b.get('upload-progress').value, 100);
  assert.match(b.get('upload-state').textContent, /Записано на карту/);
});
test('upload conflict is shown without reporting success', async () => {
  const b = await browser({ uploadStatus: 409 });
  b.get('upload-file').files = [{ name: 'a.wav', size: 1024 }]; b.get('upload-file').onchange();
  await b.get('upload').onclick();
  assert.match(b.get('upload-state').textContent, /уже существует/);
});
test('active upload disables format and supports cancellation', async () => {
  const b = await browser({ pending: true });
  b.get('upload-file').files = [{ name: 'a.wav', size: 1024 }]; b.get('upload-file').onchange();
  const completed = b.get('upload').onclick();
  assert.equal(b.get('format').disabled, true);
  assert.equal(b.get('cancel-upload').hidden, false);
  b.get('cancel-upload').onclick();
  await completed;
  assert.match(b.get('upload-state').textContent, /отменена/);
  assert.equal(b.get('cancel-upload').hidden, true);
});
test('empty WAV list shows other filenames instead of hiding them', async () => {
  const b = await browser({ files: [], otherFiles: ['music.mp3'] });
  assert.match(b.get('files').textContent, /music\.mp3/);
});
test('success response with missing track is reported explicitly', async () => {
  const b = await browser({ files: [] });
  b.get('upload-file').files = [{ name: 'a.wav', size: 1024 }];
  b.get('upload-file').onchange();
  await b.get('upload').onclick();
  assert.match(b.get('upload-state').textContent, /не попал в список/);
});
test('line 2 can play a different sound without changing line 1', async () => {
  const b=await browser();
  b.get('select-2').value='a.wav';b.get('select-2').onchange();
  await b.get('play-2').onclick();
  const body=JSON.parse(b.calls.find(c=>c.url==='/api/control').init.body);
  assert.deepEqual(body,{action:'play',line:2,file:'a.wav'});
  assert.equal(b.get('track-1').textContent,'test.wav');
  assert.equal(b.get('track-2').textContent,'a.wav');
  assert.match(b.get('state-2').textContent,/по кругу/);
});
test('volume and stop commands target their own line and retain selection', async () => {
  const b=await browser();
  b.get('volume-2').value='73';await b.get('volume-2').onchange();
  assert.equal(b.state.lines[0].volume,25);assert.equal(b.state.lines[1].volume,73);
  await b.get('stop-1').onclick();
  assert.equal(b.state.lines[0].track,'test.wav');
  assert.equal(b.state.lines[0].autoplay,false);
});
test('restored stopped selection is displayed and can restart', async () => {
  const b=await browser();
  assert.equal(b.get('select-1').value,'test.wav');
  assert.equal(b.get('play-1').disabled,false);
  assert.match(b.get('memory-1').textContent,/Автозапуск выключен/);
});
test('a paused second line blocks card formatting and uploads', async () => {
  const b=await browser({state:{lines:[
    {id:1,track:'test.wav',volume:25,playing:false,autoplay:false},
    {id:2,track:'a.wav',volume:40,playing:true,paused:true,autoplay:true},
  ]}});
  assert.equal(b.get('format').disabled,true);
  assert.equal(b.get('upload').disabled,true);
  assert.equal(b.get('pause-2').textContent,'Продолжить');
  assert.match(b.get('memory-2').textContent,/на паузе/);
});
test('periodic status preserves an unsubmitted selection and shows loop count', async () => {
  const b=await browser();
  b.get('select-2').value='a.wav';b.get('select-2').onchange();
  b.state.lines[0].playing=true;b.state.lines[0].loops=4;
  await b.poll();
  assert.equal(b.get('select-2').value,'a.wav');
  assert.match(b.get('state-1').textContent,/4/);
});
