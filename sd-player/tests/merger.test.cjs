const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const html=fs.readFileSync(path.join(__dirname,'../audio-merger.html'),'utf8');
const context=vm.createContext({document:{getElementById(){return {}}}});
vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1],context);
const audio=channels=>({length:channels[0].length,numberOfChannels:channels.length,getChannelData:i=>channels[i]});
const samples=buffer=>Array.from({length:(buffer.byteLength-44)/4},(_,i)=>{
  const v=new DataView(buffer);return [v.getInt16(44+i*4,true),v.getInt16(46+i*4,true)];
});
test('each source is averaged to mono without doubling volume',()=>{
  assert.deepEqual(Array.from(context.toMono(audio([[1,-1,0.5],[-1,1,0.5]]))),[0,0,0.5]);
});
test('first recording is left, second is right, output is PCM16 stereo 44100 Hz',()=>{
  const output=context.buildStereoWav([0.5,-1],[1,0],false),v=new DataView(output);
  assert.deepEqual(samples(output),[[16384,32767],[-32768,0]]);
  assert.equal(Buffer.from(output,0,4).toString(),'RIFF');
  assert.equal(Buffer.from(output,8,4).toString(),'WAVE');
  assert.equal(v.getUint32(4,true),output.byteLength-8);
  assert.equal(v.getUint16(20,true),1);assert.equal(v.getUint16(22,true),2);
  assert.equal(v.getUint32(24,true),44100);assert.equal(v.getUint32(28,true),176400);
  assert.equal(v.getUint16(32,true),4);assert.equal(v.getUint16(34,true),16);
});
test('short left recording is padded with silence',()=>{
  assert.deepEqual(samples(context.buildStereoWav([1],[0,0.5,-1],false)),
    [[32767,0],[0,16384],[0,-32768]]);
});
test('short right recording repeats without crossing channels',()=>{
  assert.deepEqual(samples(context.buildStereoWav([0,0.5,-1],[1],true)),
    [[0,32767],[16384,32767],[-32768,32767]]);
});
test('invalid samples cannot overflow PCM16 and empty recordings are rejected',()=>{
  assert.deepEqual(samples(context.buildStereoWav([2,NaN],[-2,Infinity],false)),
    [[32767,-32768],[0,0]]);
  assert.throws(()=>context.buildStereoWav([],[1],false));
});
