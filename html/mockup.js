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
  // A: one column per feature: switch plus live state, advertised/partner flags underneath
  A:function(){
    setHead(BASE.map(function(s){return[s]}).concat([[t("nav_fc")],[t("nav_eee")],[""]]));
    function sub(a,b){return h("div",{class:"small mut",style:"margin-top:3px;line-height:1.35",html:esc(t("e_adv"))+": "+esc(a)+"<br>"+esc(t("e_lp"))+": "+esc(b)})}
    S.ports.forEach(function(p){
      var f=FC[p.portNum],e=EEE[p.portNum];
      baseRow($("ptable").tBodies[0],p,{before:function(tr){
        var c=cellWith(tr,p.isSFP?[badge(t("e_na"))," ",fcNeg(p)]:[sw(f&1)," ",fcNeg(p)]);
        if(!p.isSFP)c.appendChild(sub(fcFlags(f&3),fcFlags(f>>2&3)));
        c=cellWith(tr,p.isSFP?[badge(t("e_na"))]:[sw(parseInt(e.adv,2))," ",eeeSt(p)]);
        if(!p.isSFP)c.appendChild(sub(eeeFlags(e.adv),eeeFlags(e.lp)));
      }});
    });
  },
  // B: grouped headers, every flag in its own column (both current pages side by side)
  B:function(){
    var g={colspan:4,style:"text-align:center;border-bottom:0;border-left:1px solid var(--bd)"};
    setHead(BASE.map(function(s){return[s,{rowspan:2}]}).concat([[t("nav_fc"),g],[t("nav_eee"),g],["",{rowspan:2}]]),
      [t("e_adv"),t("e_lp"),t("fc_neg"),t("c_enabled"),t("e_adv"),t("e_lp"),t("e_active"),t("c_enabled")]);
    var r2=$("ptable").tHead.rows[1].cells;r2[0].style.borderLeft=r2[4].style.borderLeft="1px solid var(--bd)";
    S.ports.forEach(function(p){
      var f=FC[p.portNum],e=EEE[p.portNum],dash=function(){return h("span",{class:"mut",text:"-"})};
      baseRow($("ptable").tBodies[0],p,{before:function(tr){
        cellWith(tr,[p.isSFP?"-":fcFlags(f&3)]).style.borderLeft="1px solid var(--bd)";
        cellWith(tr,[p.isSFP?"-":fcFlags(f>>2&3)]);
        cellWith(tr,[fcNeg(p)]);
        cellWith(tr,[p.isSFP?dash():sw(f&1)]);
        cellWith(tr,[p.isSFP?"-":eeeFlags(e.adv)]).style.borderLeft="1px solid var(--bd)";
        cellWith(tr,[p.isSFP?"-":eeeFlags(e.lp)]);
        cellWith(tr,[eeeSt(p)]);
        cellWith(tr,[p.isSFP?dash():sw(parseInt(e.adv,2))]);
      }});
    });
  },
  // E: one card per port instead of a table row; aimed at phones
  E:function(){
    setHead(BASE.map(function(s){return[s]}).concat([[""]]));
    var wrap=$("ptable").closest(".scrollx");wrap.style.display="none";
    var list=h("div",{class:"mockx",style:"display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:10px"});
    function kv(k,v){return[h("span",{class:"mut small",text:k}),typeof v==="string"?h("span",{html:v}):v]}
    function grid(rows,cols){var g=h("div",{style:"display:grid;grid-template-columns:"+(cols||"auto 1fr")+";gap:6px 10px;align-items:center"});rows.forEach(function(r){kv(r[0],r[1]).forEach(function(e){g.appendChild(e)})});return g}
    function sec(title,on,state,rows,sfp){
      var body=h("div",{style:"line-height:1.35"});
      rows.forEach(function(r){body.appendChild(h("div",{class:"mut small",text:r[0]}));body.appendChild(h("div",{style:"margin-bottom:4px",text:r[1]}))});
      return h("div",{style:"min-width:0"},[
        h("div",{style:"display:flex;gap:6px;align-items:center;flex-wrap:wrap;font-weight:600;margin-bottom:4px"},[sfp?h("span",{html:badge(t("e_na"))}):sw(on),h("span",{text:title}),h("span",{html:state})]),
        body]);
    }
    S.ports.forEach(function(p){
      var i=p.portNum-1,f=FC[p.portNum],e=EEE[p.portNum];
      var spd=h("select",{class:"in"});
      (p.isSFP?SFPRATES:SPEEDS).forEach(function(o){spd.appendChild(h("option",{value:o[0],text:speedLabel(o[1])}))});
      var mtu=h("input",{class:"in",type:"number",style:"width:100%;box-sizing:border-box",value:i%2?"1522":"16383"});
      spd.style.width="100%";
      list.appendChild(h("div",{style:"border:1px solid var(--bd);border-radius:8px;padding:10px 12px"},[
        h("div",{style:"display:flex;gap:8px;align-items:center;margin-bottom:8px"},[
          h("b",{style:"white-space:nowrap",text:t("c_port")+" "+p.portNum+(p.isSFP?" (SFP)":"")}),h("span",{html:linkBadge(p)}),
          h("span",{class:"mut small",text:t("pt_conn")+": "+(parseInt(p.macs,16)&0x1fff)}),
          h("span",{style:"flex:1"}),sw(p.enabled),h("button",{class:"ctl",text:t("c_apply")})]),
        grid([[t("c_name"),h("input",{class:"in",style:"width:100%;box-sizing:border-box",placeholder:"-"})],[t("pt_speed"),spd],[t("pt_mtu"),mtu]]),
        h("div",{style:"display:grid;grid-template-columns:1fr 1fr;gap:12px;border-top:1px solid var(--bd);padding-top:8px;margin-top:8px"},[
          sec(t("nav_fc"),f&1,fcNeg(p),[[t("e_adv"),p.isSFP?"-":fcFlags(f&3)],[t("e_lp"),p.isSFP?"-":fcFlags(f>>2&3)]],p.isSFP),
          sec(t("nav_eee"),parseInt(e.adv,2),eeeSt(p),[[t("e_adv"),p.isSFP?"-":eeeFlags(e.adv)],[t("e_lp"),p.isSFP?"-":eeeFlags(e.lp)]],p.isSFP)]),
      ]));
    });
    wrap.after(list);
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
