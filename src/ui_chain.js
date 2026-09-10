/* LoopBox v0.2.1 — Chain UI — Main/Control/Loop pages */
import{MoveMainKnob,MoveBack,LightGrey}from'/data/UserData/schwung/shared/constants.mjs';
import{decodeDelta}from'/data/UserData/schwung/shared/input_filter.mjs';
import{drawMenuHeader as drawHeader,drawMenuFooter as drawFooter}from'/data/UserData/schwung/shared/menu_layout.mjs';
const W=128,H=64,CC_JOG_CLICK=3,KNOB_CC_BASE=71;
const PAGES=[
{name:"Main",params:[
{key:"globalSat",name:"Sat",min:0,max:1,step:0.01,fmt:v=>`${(v*100).toFixed(0)}%`},
{key:"masterComp",name:"Comp",min:0,max:1,step:0.01,fmt:v=>`${(v*100).toFixed(0)}%`},
{key:"masterLoCut",name:"LoCut",min:20,max:500,step:1,fmt:v=>`${Math.round(v)}Hz`},
{key:"masterHiCut",name:"HiCut",min:1000,max:20000,step:50,fmt:v=>v>=10000?`${(v/1000).toFixed(1)}k`:`${Math.round(v)}Hz`},
{key:"clock",name:"Clock",min:0,max:1,step:0.01,fmt:v=>{let spd=0.25*Math.pow(8,v);return`x${spd.toFixed(2)}`}},
{key:"delayRate",name:"DlyRt",min:0.01,max:1,step:0.01,fmt:v=>`${(v*100).toFixed(0)}%`},
{key:"delayFeedback",name:"DlyFb",min:0,max:0.95,step:0.01,fmt:v=>`${(v*100).toFixed(0)}%`},
{key:"reverb",name:"Reverb",min:0,max:1,step:0.01,fmt:v=>`${(v*100).toFixed(0)}%`}]},
{name:"Control",params:[
{key:"preamp",name:"Preamp",min:0,max:11,step:1,enum:["Clean","Cass1","Cass2","VHS1","VHS2","Reel15","Reel7","Reel3","4trk","Porta","Dub","Warp"]},
{key:"overdubMode",name:"OdMode",min:0,max:2,step:1,enum:["Replace","Multiply","Disint"]},
{key:"stability",name:"Stabil",min:0,max:1,step:0.01,fmt:v=>`${(v*100).toFixed(0)}%`}]},
{name:"Loop",params:[
{key:"v_start",name:"Start",min:0,max:1,step:0.01,fmt:v=>`${(v*100).toFixed(0)}%`},
{key:"v_end",name:"End",min:0,max:1,step:0.01,fmt:v=>`${(v*100).toFixed(0)}%`},
{key:"v_reverse",name:"Rev",min:0,max:1,step:1,enum:["Normal","Reverse"]},
{key:"v_sat",name:"Sat",min:0,max:1,step:0.01,fmt:v=>`${(v*100).toFixed(0)}%`},
{key:"v_wowflut",name:"W/Flut",min:0,max:1,step:0.01,fmt:v=>`${(v*100).toFixed(0)}%`},
{key:"v_send",name:"Send",min:0,max:1,step:0.01,fmt:v=>`${(v*100).toFixed(0)}%`},
{key:"v_glitch",name:"Glitch",min:0,max:1,step:0.01,fmt:v=>{if(v<0.01)return"Off";if(v<0.2)return"1/4";if(v<0.35)return"1/8";if(v<0.5)return"1/16";if(v<0.65)return"1/32";if(v<0.8)return`Glt${Math.round(v*100)}`;return`Crsh${Math.round((v-0.6)/0.4*100)}`}},
{key:"v_tilt",name:"Tilt",min:-1,max:1,step:0.02,fmt:v=>{if(Math.abs(v)<0.02)return"Flat";return v>0?`Brt+${(v*6).toFixed(1)}`:`Wrm${(v*6).toFixed(1)}`}},
{key:"v_eqBass",name:"Bass",min:-1,max:1,step:0.02,fmt:v=>`${v>=0?"+":""}${(v*15).toFixed(1)}dB`},
{key:"v_eqPresFrq",name:"MidF",min:0,max:1,step:0.01,fmt:v=>{let f=150*Math.pow(7000/150,v);return f>=1000?`${(f/1000).toFixed(1)}k`:`${Math.round(f)}Hz`}},
{key:"v_eqPresAmt",name:"MidG",min:-1,max:1,step:0.02,fmt:v=>`${v>=0?"+":""}${(v*11).toFixed(1)}dB`},
{key:"v_eqTreble",name:"Treble",min:-1,max:1,step:0.02,fmt:v=>`${v>=0?"+":""}${(v*15).toFixed(1)}dB`},
{key:"v_pitch",name:"Pitch",min:-2,max:2,step:0.05,fmt:v=>`${v>=0?"+":""}${v.toFixed(2)}`},
{key:"v_filter",name:"Filter",min:0,max:1,step:0.01,fmt:v=>{if(v<0.45)return`LP`;if(v>0.55)return`HP`;return"Flat"}},
{key:"v_pan",name:"Pan",min:-1,max:1,step:0.02,fmt:v=>{if(Math.abs(v)<0.02)return"C";return v<0?`L${Math.round(Math.abs(v)*50)}`:`R${Math.round(v*50)}`}},
{key:"v_volume",name:"Vol",min:0,max:1,step:0.01,fmt:v=>`${(v*100).toFixed(0)}%`},
{key:"v_decay",name:"Decay",min:0,max:1,step:0.01,fmt:v=>v>0.99?"Inf":`${(v*100).toFixed(0)}%`}]}];

let selectedPage=0,insidePage=false,selectedParam=0,editMode=false,needsRedraw=true,values={},selTrack=1;
function formatValue(p,v){if(p.enum)return p.enum[Math.round(v)]||"?";return p.fmt(v)}
function clampValue(p,v){if(p.enum){v=Math.round(v);if(v>p.max)v=p.max;if(v<p.min)v=p.min}else{v=Math.max(p.min,Math.min(p.max,v))}return v}
function setParam(p,value){value=clampValue(p,value);values[p.key]=value;let valStr;if(p.enum)valStr=String(Math.round(value));else if(p.step>=1)valStr=String(Math.round(value));else valStr=value.toFixed(4);host_module_set_param(p.key,valStr);needsRedraw=true}
function fetchAllParams(){const rt=host_module_get_param("selTrack");if(rt)selTrack=parseInt(rt)||1;for(const page of PAGES)for(const p of page.params){const raw=host_module_get_param(p.key);if(raw!=null&&raw!=undefined&&raw!=""){if(p.enum){const idx=p.enum.indexOf(raw);values[p.key]=idx>=0?idx:(parseFloat(raw)||0)}else{const num=parseFloat(raw);if(!isNaN(num))values[p.key]=num}}}values["_v_state"]=host_module_get_param("v_state")||"Empty";values["_v_loopLen"]=host_module_get_param("v_loopLen")||"0.0"}
function drawRootView(){clear_screen();drawHeader("LoopBox");const st=values["_v_state"]||"Empty";const info=`T${selTrack} ${st}`;print(W-info.length*6-2,1,info,1);const lh=11,y0=24;for(let i=0;i<PAGES.length;i++){const y=y0+i*lh;const sel=i===selectedPage;if(sel)fill_rect(0,y-1,W,lh,1);const color=sel?0:1;print(2,y,`${sel?"> ":"  "}${PAGES[i].name}`,color);const fp=PAGES[i].params[0];const v=values[fp.key];if(v!==undefined){const vs=formatValue(fp,v);print(W-vs.length*6-4,y,vs,color)}}drawFooter({left:"Jog:page",right:"Click:enter"})}
function drawPageView(){clear_screen();const page=PAGES[selectedPage];drawHeader(`LoopBox: ${page.name}`);const st=values["_v_state"]||"Empty";print(W-(`T${selTrack} ${st}`).length*6-2,1,`T${selTrack} ${st}`,1);const lh=11,y0=16,visible=4;let startIdx=Math.max(0,Math.min(selectedParam-1,page.params.length-visible));for(let vi=0;vi<visible;vi++){const i=startIdx+vi;if(i>=page.params.length)break;const y=y0+vi*lh;const p=page.params[i];const sel=i===selectedParam;if(sel)fill_rect(0,y-1,W,lh,1);const color=sel?0:1;print(2,y,`${sel?(editMode?"* ":"> "):"  "}${p.name}`,color);const v=values[p.key];if(v!==undefined){const vs=formatValue(p,v);print(W-vs.length*6-4,y,vs,color)}}if(editMode)drawFooter({left:"Jog:value",right:"Click:done"});else drawFooter({left:"Jog:param",right:"Back:pages"})}
function draw(){if(insidePage)drawPageView();else drawRootView();needsRedraw=false}
function handleKnob(knobIdx,delta){const page=PAGES[selectedPage];if(knobIdx>=page.params.length)return;const p=page.params[knobIdx];const v=values[p.key]!==undefined?values[p.key]:(p.min+p.max)/2;setParam(p,v+delta*p.step)}
function init(){fetchAllParams();needsRedraw=true}
function tick(){fetchAllParams();if(needsRedraw)draw()}
function onMidiMessageInternal(data){const status=data[0]&0xF0,d1=data[1],d2=data[2];if(status!==0xB0)return;if(d1===MoveMainKnob){const delta=decodeDelta(d2);if(delta===0)return;if(!insidePage){selectedPage=Math.max(0,Math.min(PAGES.length-1,selectedPage+delta));needsRedraw=true}else if(editMode){const page=PAGES[selectedPage];const p=page.params[selectedParam];const v=values[p.key]!==undefined?values[p.key]:(p.min+p.max)/2;setParam(p,v+delta*p.step)}else{const page=PAGES[selectedPage];selectedParam=Math.max(0,Math.min(page.params.length-1,selectedParam+delta));needsRedraw=true}return}if(d1===CC_JOG_CLICK&&d2>=64){if(!insidePage){insidePage=true;selectedParam=0;editMode=false}else if(editMode)editMode=false;else editMode=true;needsRedraw=true;return}if(d1===MoveBack&&d2>=64){if(editMode){editMode=false;needsRedraw=true}else if(insidePage){insidePage=false;needsRedraw=true}return}const knobIdx=d1-KNOB_CC_BASE;if(knobIdx>=0&&knobIdx<8){const delta=decodeDelta(d2);if(delta!==0)handleKnob(knobIdx,delta);return}}
globalThis.chain_ui={init,tick,onMidiMessageInternal};
