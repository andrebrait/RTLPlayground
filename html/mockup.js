// Throwaway mock-up: Ports page layouts that fold in EEE and flow control.
// Load after app.js, then call mock("A"|"B"|"C"|"D"). Not for merging.
LANG.en.nav_fc="Flow control";LANG.en.fc_title="Flow control (802.3x pause)";LANG.en.fc_neg="Negotiated";
var FC={}, EEE={};
function fakeData(){
  S.ports.forEach(function(p){if(p.link>0)p.link=p.isSFP?5:[6,6,3,2][p.portNum%4]});
  // fc bits as in PR #652: 0 adv pause, 1 adv asym, 2-3 partner, 4 TX, 5 RX negotiated
  S.ports.forEach(function(p){
    var i=p.portNum, up=p.enabled&&p.link>0;
    FC[i]=p.isSFP?(up?0x30:0):(i===4?0:(1|(up?(i===3?0x08:0x34):0)));
    EEE[i]={adv:p.isSFP?"000":(i===2?"000":"111"),lp:up&&!p.isSFP?"110":"000",active:up&&!p.isSFP&&i!==3};
  });
}
function sw(on,dis){
  var l=h("label",{class:"switch"},[h("input",{type:"checkbox"}),h("i")]);
  l.firstChild.checked=!!on;if(dis)l.firstChild.disabled=true;
  return l;
}
function fcNeg(p){var n=FC[p.portNum]>>4&3;return n?badge(["","TX","RX","TX/RX"][n],"ok"):badge(t("c_off"))}
function eeeSt(p){var e=EEE[p.portNum];return p.isSFP?badge(t("e_na")):e.active?badge(t("e_active"),"ok"):badge(t("e_idle"))}
function cellWith(tr,kids){var c=tr.insertCell();c.style.whiteSpace="nowrap";kids.forEach(function(k){typeof k==="string"?c.insertAdjacentHTML("beforeend",k):c.appendChild(k)});return c}

function baseRow(tb,p,opt){
  var i=p.portNum-1;
  var spd=h("select",{class:"in",id:"pspd"+i});
  (p.isSFP?SFPRATES:SPEEDS).forEach(function(o){spd.appendChild(h("option",{value:o[0],text:speedLabel(o[1])}))});
  var tr=tb.insertRow();
  tr.insertCell().textContent=p.portNum+(p.isSFP?" (SFP)":"");
  tr.insertCell().appendChild(h("input",{class:"in",id:"pname"+i,size:"9",maxlength:"15",value:p.name||"",placeholder:"-"}));
  var lk=tr.insertCell();lk.id="plink"+i;lk.style.whiteSpace="nowrap";
  var mc=tr.insertCell();mc.id="pmac"+i;mc.className="mono";mc.textContent="-";
  tr.insertCell().appendChild(spd);
  var e=sw(p.enabled);e.firstChild.id="pen"+i;tr.insertCell().appendChild(e);
  tr.insertCell().appendChild(h("input",{class:"in sm",id:"pmtu"+i,type:"number",min:"64",max:"16383"}));
  if(opt&&opt.before)opt.before(tr,p);
  tr.insertCell().appendChild(h("button",{class:"ctl",text:t("c_apply")}));
  return tr;
}
function setHead(cells,extra){
  var th=$("ptable").tHead;th.innerHTML="";
  var r=th.insertRow();
  cells.forEach(function(c){var e=h("th",{text:c[0]});if(c[1])for(var k in c[1])e.setAttribute(k,c[1][k]);r.appendChild(e)});
  if(extra){var r2=th.insertRow();extra.forEach(function(c){r2.appendChild(h("th",{text:c}))})}
}
var BASE=[t("c_port"),t("c_name"),t("c_link"),t("pt_conn"),t("pt_speed"),t("c_enabled"),t("pt_mtu")];
function clearExtras(){
  var sec=$("ptable").closest("section");
  sec.querySelectorAll(".mockx").forEach(function(e){e.remove()});
  $("ptable").tBodies[0].innerHTML="";
}

var VARIANTS={
  // A: two columns, toggle plus live state; details stay in the port pop-up
  A:function(){
    setHead(BASE.map(function(s){return[s]}).concat([[t("nav_fc")],[t("nav_eee")],[""]]));
    S.ports.forEach(function(p){
      baseRow($("ptable").tBodies[0],p,{before:function(tr){
        cellWith(tr,p.isSFP?[badge(t("e_na"))," ",fcNeg(p)]:[sw(FC[p.portNum]&1)," ",fcNeg(p)]);
        cellWith(tr,p.isSFP?[badge(t("e_na"))]:[sw(parseInt(EEE[p.portNum].adv,2))," ",eeeSt(p)]);
      }});
    });
  },
  // B: grouped header, separate state/enable sub-columns
  B:function(){
    setHead(BASE.map(function(s){return[s,{rowspan:2}]}).concat([[t("nav_fc"),{colspan:2,style:"text-align:center;border-bottom:0"}],[t("nav_eee"),{colspan:2,style:"text-align:center;border-bottom:0"}],["",{rowspan:2}]]),
      [t("fc_neg"),t("c_enabled"),t("e_active"),t("c_enabled")]);
    S.ports.forEach(function(p){
      baseRow($("ptable").tBodies[0],p,{before:function(tr){
        cellWith(tr,[fcNeg(p)]);
        cellWith(tr,[p.isSFP?h("span",{class:"mut",text:"-"}):sw(FC[p.portNum]&1)]);
        cellWith(tr,[eeeSt(p)]);
        cellWith(tr,[p.isSFP?h("span",{class:"mut",text:"-"}):sw(parseInt(EEE[p.portNum].adv,2))]);
      }});
    });
  },
  // C: Ports page keeps its table; a second card on the same page merges the EEE and flow control pages
  C:function(){
    setHead(BASE.map(function(s){return[s]}).concat([[""]]));
    S.ports.forEach(function(p){baseRow($("ptable").tBodies[0],p)});
    var tbl=h("table",{class:"t"});
    var th=tbl.createTHead(),r=th.insertRow(),r2=th.insertRow();
    [[t("c_port"),{rowspan:2}],[t("nav_fc"),{colspan:4}],[t("nav_eee"),{colspan:4}]].forEach(function(c){
      var e=h("th",{text:c[0]});for(var k in c[1])e.setAttribute(k,c[1][k]);if(c[1].colspan)e.style.cssText="text-align:center;border-left:1px solid var(--bd);border-bottom:0";r.appendChild(e);
    });
    [t("e_adv"),t("e_lp"),t("fc_neg"),t("c_enabled"),t("e_adv"),t("e_lp"),t("e_active"),t("c_enabled")].forEach(function(s,i){
      var e=h("th",{text:s});if(i%4===0)e.style.borderLeft="1px solid var(--bd)";r2.appendChild(e);
    });
    var tb=tbl.createTBody();
    S.ports.forEach(function(p){
      var tr=tb.insertRow(),f=FC[p.portNum],e=EEE[p.portNum];
      tr.insertCell().textContent=p.portNum+(p.isSFP?" (SFP)":"");
      var c=tr.insertCell();c.style.borderLeft="1px solid var(--bd)";c.textContent=p.isSFP?"-":fcFlags(f&3);
      tr.insertCell().textContent=p.isSFP?"-":fcFlags(f>>2&3);
      cellWith(tr,[fcNeg(p)]);
      cellWith(tr,[p.isSFP?h("span",{class:"mut",text:t("e_na")}):sw(f&1)]);
      c=tr.insertCell();c.style.borderLeft="1px solid var(--bd)";c.textContent=p.isSFP?"-":eeeFlags(e.adv);
      tr.insertCell().textContent=p.isSFP?"-":eeeFlags(e.lp);
      cellWith(tr,[eeeSt(p)]);
      cellWith(tr,[p.isSFP?h("span",{class:"mut",text:t("e_na")}):sw(parseInt(e.adv,2))]);
    });
    var card=h("div",{class:"card mockx"},[h("h2",{html:esc(t("nav_fc"))+" &amp; "+esc(t("nav_eee"))+'<span class="hint">'+esc(t("fc_title"))+", 802.3az</span>"}),h("div",{class:"scrollx"},[tbl]),
      h("p",{class:"small mut",style:"margin-top:8px",text:"Toggles apply immediately and renegotiate the link."})]);
    $("ptable").closest(".card").after(card);
  },
  // D: one row per port as today, plus an expandable detail row with both features
  D:function(){
    setHead([[""]].concat(BASE.map(function(s){return[s]})).concat([[""]]));
    S.ports.forEach(function(p,idx){
      var tb=$("ptable").tBodies[0],tr=baseRow(tb,p);
      var open=idx===0||idx===3;
      var tg=tr.insertCell(0);tg.appendChild(h("button",{class:"ctl",text:open?"\u25bc":"\u25b6",style:"padding:2px 7px;font-size:11px"}));
      if(!open)return;
      var dr=tb.insertRow(),dc=dr.insertCell();dc.colSpan=tr.cells.length;dc.style.background="var(--panel2)";
      var f=FC[p.portNum],e=EEE[p.portNum];
      function block(title,on,rows,state){
        var g=h("div",{style:"display:grid;grid-template-columns:auto auto;gap:4px 14px;align-items:center"});
        rows.forEach(function(r){g.appendChild(h("span",{class:"mut small",text:r[0]}));g.appendChild(h("span",{text:r[1]}))});
        return h("div",{style:"min-width:260px"},[
          h("div",{style:"display:flex;gap:10px;align-items:center;margin-bottom:6px;font-weight:600"},[sw(on),h("span",{text:title}),h("span",{html:state})]),g]);
      }
      dc.appendChild(h("div",{style:"display:flex;gap:40px;flex-wrap:wrap;padding:4px 0 4px 36px"},[
        block(t("nav_fc"),f&1,[[t("e_adv"),fcFlags(f&3)],[t("e_lp"),fcFlags(f>>2&3)]],fcNeg(p)),
        block(t("nav_eee"),parseInt(e.adv,2),[[t("e_adv"),eeeFlags(e.adv)],[t("e_lp"),eeeFlags(e.lp)]],eeeSt(p)),
      ]));
    });
  },
};
function fcFlags(b){return["Pause","Asym"].filter(function(s,i){return b&(1<<i)}).join(", ")||"-"}
function mockStatus(){
  fakeData();updateStrip();
  S.ports.forEach(function(p){
    var i=p.portNum-1,el=$("plink"+i),mc=$("pmac"+i);
    if(mc)mc.textContent=parseInt(p.macs,16)&0x1fff;
    if(el)el.innerHTML=linkBadge(p);
  });
}
function mock(v){
  fakeData();clearExtras();VARIANTS[v]();mockStatus();loadMtu();
  tabHooks.ports.status=mockStatus;
  buildPorts=function(){};
}
