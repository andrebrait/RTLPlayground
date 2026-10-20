// Throwaway mock-up: Ports page layouts that fold in EEE and flow control.
// Load after app.js, then call mock("A"|"B"|"C"|"D"). Not for merging.
LANG.en.nav_fc="Flow control";LANG.en.fc_title="Flow control (802.3x pause)";LANG.en.fc_neg="Negotiated";
var FC={}, EEE={};
function fakeData(){
  S.ports.forEach(function(p){if(p.link>0)p.link=p.isSFP?5:({1:6,3:2,5:3,7:2}[p.portNum]||6);if(!p.isSFP)p.adv=p.portNum===7?"011111":"111111"});
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
// Off = port or feature switched off; Inactive = switched on but not in use on this link. Labels are capitalised alike.
LANG.en.e_inactive="Inactive";LANG.de.e_inactive="Inaktiv";LANG.en.st_off="Off";LANG.de.st_off="Aus";LANG.en.st_down="Down";LANG.de.st_down="Getrennt";
// Off looks like a disabled port tile in the strip (faded, dashed); Inactive and Down like a port without link (solid outline)
function dash(){return badge(t("st_off"),"st-off")}
function inact(s){return badge(s,"st-in")}
// Link badge in the colour of this port's tile in the strip (LINKC per speed), as the link pop-up already does
function linkUp(p){var pc="var("+(LINKC[p.link]||"--s1000")+")";return'<span class="badge" style="color:'+pc+';background:color-mix(in srgb,'+pc+' 10%,var(--panel2))">'+esc(LINKS[p.link])+"</span>"}
// flow control shows the negotiated direction or Inactive
function fcNeg(p){var f=FC[p.portNum],n=f>>4&3;return!p.enabled||(!p.isSFP&&!(f&1))?dash():n?badge(["","TX","RX","TX/RX"][n],"ok"):inact(t("e_inactive"))}
function eeeSt(p){var e=EEE[p.portNum];return p.isSFP?badge(t("e_na")):!p.enabled||!parseInt(e.adv,2)?dash():e.active?badge(t("e_active"),"ok"):inact(t("e_inactive"))}
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

// Final round (PR #652 feedback): overview switch + state, details behind an Advanced control
function advSpeeds(p){
  var b=parseInt(p.adv||"0",2),n=["10M "+t("c_half"),"10M "+t("c_full"),"100M "+t("c_half"),"100M "+t("c_full"),"1G","2.5G"];
  return n.filter(function(_,i){return b&(1<<i)}).reverse().join(", ")||"-";
}
function fcSw(p){return p.isSFP?h("span",{html:badge(t("e_na"))}):sw(FC[p.portNum]&1)}
function eeeSw(p){return p.isSFP?h("span",{html:badge(t("e_na"))}):sw(parseInt(EEE[p.portNum].adv,2))}
function feat(p){
  var f=FC[p.portNum],e=EEE[p.portNum];
  return[
    {name:t("nav_fc"),sw:fcSw(p),st:fcNeg(p),adv:p.isSFP?"-":fcFlags(f&3),lp:p.isSFP?"-":fcFlags(f>>2&3)},
    {name:t("nav_eee"),sw:eeeSw(p),st:eeeSt(p),adv:p.isSFP?"-":eeeFlags(e.adv),lp:p.isSFP?"-":eeeFlags(e.lp)},
  ];
}
// Desktop details: one row per feature, labels in the header
function detailsTable(p,withSw){
  var tb=h("table",{class:"t",style:"width:auto"}),r=tb.createTHead().insertRow();
  [""].concat(withSw?[t("c_enabled")]:[]).concat(["Status",t("e_adv"),t("e_lp")]).forEach(function(s){r.appendChild(h("th",{text:s}))});
  var b=tb.createTBody();
  feat(p).forEach(function(x){
    var tr=b.insertRow();
    var c=tr.insertCell();c.style.fontWeight="600";c.textContent=x.name;
    if(withSw){c=tr.insertCell();if(x.sw)c.appendChild(x.sw)}
    cellWith(tr,[x.st]);
    tr.insertCell().textContent=x.adv;tr.insertCell().textContent=x.lp;
  });
  return tb;
}
// Phone details: each feature is a row (switch, name, state), with label/value pairs side by side underneath
function detailsList(p,withSw){
  var d=h("div");
  feat(p).forEach(function(x,i){
    var g=h("div",{style:"display:grid;grid-template-columns:7.5em 1fr;gap:4px 10px;margin:6px 0 0"});
    [[t("e_adv"),x.adv]].concat(x.lp!==""?[[t("e_lp"),x.lp]]:[]).forEach(function(kv){
      g.appendChild(h("span",{class:"mut small",text:kv[0]}));g.appendChild(h("span",{text:kv[1]}));
    });
    d.appendChild(h("div",{style:"padding:8px 0;"+(i?"border-top:1px solid var(--bd)":"")},[
      h("div",{style:"display:flex;gap:8px;align-items:center"},[h("b",{text:x.name}),h("span",{html:x.st}),h("span",{style:"flex:1"})].concat(withSw&&x.sw?[x.sw]:[])),g]));
  });
  return d;
}
function toggleBtn(open,label){return h("button",{class:"ctl",style:"white-space:nowrap",text:(label?label+" ":"")+(open?"\u25b4":"\u25be")})}
function advBtn(p){return h("button",{class:"ctl",text:"Advanced\u2026",onclick:function(){modal(t("c_port")+" "+p.portNum+": Advanced",detailsList(p,true),[h("button",{class:"ctl pri",text:t("c_apply")})])}})}
// Phone card: one row per setting, label left of value
function card(p,extra){
  var spd=h("select",{class:"in",style:"width:100%"});
  (p.isSFP?SFPRATES:SPEEDS).forEach(function(o){spd.appendChild(h("option",{value:o[0],text:speedLabel(o[1])}))});
  var g=h("div",{style:"display:grid;grid-template-columns:7.5em 1fr;gap:6px 10px;align-items:center"});
  [[t("c_name"),h("input",{class:"in",style:"width:100%;box-sizing:border-box",placeholder:"-"})],[t("pt_speed"),spd],
   [t("pt_mtu"),h("input",{class:"in",type:"number",style:"width:100%;box-sizing:border-box",value:p.portNum%2?"16383":"1522"})],
   [t("pt_conn"),h("span",{text:String(parseInt(p.macs,16)&0x1fff)})]].forEach(function(kv){
    g.appendChild(h("span",{class:"mut small",text:kv[0]}));g.appendChild(kv[1]);
  });
  return h("div",{style:"border:1px solid var(--bd);border-radius:8px;padding:10px 12px"},[
    h("div",{style:"display:flex;gap:8px;align-items:center;margin-bottom:8px"},[
      h("b",{style:"white-space:nowrap",text:t("c_port")+" "+p.portNum+(p.isSFP?" (SFP)":"")}),h("span",{style:"white-space:nowrap"},[h("span",{html:linkBadge(p)}),infoBtn(p)]),
      h("span",{style:"flex:1"}),sw(p.enabled),h("button",{class:"ctl",text:t("c_apply")})]),g].concat(extra));
}
function cardList(fn){
  var wrap=$("ptable").closest(".scrollx");wrap.style.display="none";
  var list=h("div",{class:"mockx",style:"display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:10px"});
  S.ports.forEach(function(p,i){list.appendChild(card(p,fn(p,i)))});
  wrap.after(list);
}
// F: overview columns (switch + state); a per-port toggle expands a details row
VARIANTS.F=function(){
  setHead([[""]].concat(BASE.map(function(s){return[s]})).concat([[t("nav_fc")],[t("nav_eee")],[""]]));
  S.ports.forEach(function(p,idx){
    var tb=$("ptable").tBodies[0],open=idx===0||idx===S.ports.length-1;
    var tr=baseRow(tb,p,{before:function(tr){cellWith(tr,[fcSw(p)," ",fcNeg(p)]);cellWith(tr,[eeeSw(p)," ",eeeSt(p)])}});
    tr.insertCell(0).appendChild(toggleBtn(open));
    if(!open)return;
    var dc=tb.insertRow().insertCell();dc.colSpan=tr.cells.length;dc.style.cssText="background:var(--panel2);padding:6px 8px 10px 52px";
    dc.appendChild(detailsTable(p,false));
  });
};
// G: no extra columns; an Advanced button per port opens a modal with switches and details
VARIANTS.G=function(){
  setHead(BASE.map(function(s){return[s]}).concat([[""],[""]]));
  S.ports.forEach(function(p){baseRow($("ptable").tBodies[0],p,{before:function(tr){tr.insertCell().appendChild(advBtn(p))}})});
  modal(t("c_port")+" 1: Advanced",(function(){var d=h("div");d.appendChild(detailsTable(S.ports[0],true));return d})(),[h("button",{class:"ctl pri",text:t("c_apply")})]);
};
// M: phone version of F: flow control and EEE as rows, details expand in the card
VARIANTS.M=function(){
  cardList(function(p,i){
    var rows=[[t("nav_fc"),fcSw(p),fcNeg(p)],[t("nav_eee"),eeeSw(p),eeeSt(p)]].map(function(r){
      return h("div",{style:"display:flex;gap:8px;align-items:center;padding:6px 0;border-top:1px solid var(--bd)"},[h("span",{text:r[0]}),h("span",{html:r[2]}),h("span",{style:"flex:1"}),r[1]]);
    });
    rows[0].style.marginTop="8px";
    var open=i===0,more=h("div",{style:"border-top:1px solid var(--bd);padding-top:6px"},[toggleBtn(open,"Advanced")]);
    if(open)more.appendChild(h("div",{style:"background:var(--panel2);border-radius:6px;padding:0 10px;margin-top:6px"},[detailsList(p,false)]));
    return rows.concat([more]);
  });
};
// N: phone version of G: Advanced button opens the modal
VARIANTS.N=function(){
  cardList(function(p){return[h("div",{style:"border-top:1px solid var(--bd);padding-top:8px;margin-top:8px"},[advBtn(p)])]});
  modal(t("c_port")+" 1: Advanced",detailsList(S.ports[0],true),[h("button",{class:"ctl pri",text:t("c_apply")})]);
};
// Link speed details: separate from flow control/EEE, behind an info button next to the link badge
var LINKINFO=false;
function linkTable(p){
  var d=h("div"),up=p.enabled&&p.link>0,pc="var("+(LINKC[p.link]||"--s1000")+")";
  if(p.isSFP){
    d.appendChild(h("p",{class:"small mut",style:"max-width:16em",text:"SFP ports do not advertise speeds."}));
    return d;
  }
  // one row per speed, like the port strip: duplex is not shown; adv bits 0-1 = 10M, 2-3 = 100M, 4 = 1G, 5 = 2.5G
  var a=parseInt(p.adv||"0",2),n=["10M","100M","1G","2.5G"],b=[a&3,a&12,a&16,a&32],link={1:0,2:1,3:2,6:3};
  var tb=h("table",{class:"t",style:"width:auto"}),r=tb.createTHead().insertRow();
  [t("pt_speed"),t("e_adv")].forEach(function(s){r.appendChild(h("th",{text:s}))});
  var bd=tb.createTBody(),cur=up?link[p.link]:-1;
  for(var i=n.length-1;i>=0;i--){
    var tr=bd.insertRow(),c0=tr.insertCell(),c1=tr.insertCell();
    c0.textContent=n[i];
    if(i===cur){
      // same look as the port strip tile for this link speed
      c0.style.cssText="font-weight:650;color:"+pc;
      c1.innerHTML='<span class="badge" title="Current link" style="color:'+pc+';border:1px solid '+pc+';background:color-mix(in srgb,'+pc+' 10%,var(--panel2))">\u2713 '+esc(t("c_link"))+'</span>';
    }else c1.innerHTML=b[i]?badge("\u2713","ok"):'<span class="mut">-</span>';
  }
  d.appendChild(tb);
  return d;
}
var _modal=modal;
modal=function(a,b,c){$("modal").style.width="";$("modal").style.minWidth="";_modal(a,b,c)};
function linkModal(p){modal(t("c_port")+" "+p.portNum+": "+t("c_link"),linkTable(p));$("modal").style.width="fit-content"}
function infoBtn(p){
  return h("button",{title:"Link details",style:"background:none;border:0;padding:0 0 0 6px;cursor:pointer;color:var(--ac);font-size:15px;vertical-align:middle",text:"\u24d8",
    onclick:function(){linkModal(p)}});
}
// H: Advanced button (as in G), but it expands the row instead of opening a modal
VARIANTS.H=function(){
  setHead(BASE.map(function(s){return[s]}).concat([[""],[""]]));
  S.ports.forEach(function(p,idx){
    var tb=$("ptable").tBodies[0],open=idx===0||idx===S.ports.length-1;
    var tr=baseRow(tb,p,{before:function(tr){tr.insertCell().appendChild(toggleBtn(open,"Advanced"))}});
    if(!open)return;
    var dc=tb.insertRow().insertCell();dc.colSpan=tr.cells.length;dc.style.cssText="background:var(--panel2);padding:6px 8px 10px 52px";
    dc.appendChild(detailsTable(p,true));
  });
};
// HM: phone version of H: the Advanced button expands the card
VARIANTS.HM=function(){
  cardList(function(p,i){
    var open=i===0,more=h("div",{style:"border-top:1px solid var(--bd);padding-top:8px;margin-top:8px"},[toggleBtn(open,"Advanced")]);
    if(open)more.appendChild(h("div",{style:"background:var(--panel2);border-radius:6px;padding:0 10px;margin-top:6px"},[detailsList(p,true)]));
    return[more];
  });
};
// G2/H2: G and H plus read-only status columns; switches and flags stay behind Advanced
function statusHead(){setHead(BASE.map(function(s){return[s]}).concat([[t("nav_fc")],[t("nav_eee")],[""],[""]]))}
function statusCells(tr,p){cellWith(tr,[fcNeg(p)]);cellWith(tr,[eeeSt(p)])}
VARIANTS.G2=function(){
  statusHead();
  S.ports.forEach(function(p){baseRow($("ptable").tBodies[0],p,{before:function(tr){statusCells(tr,p);tr.insertCell().appendChild(advBtn(p))}})});
  modal(t("c_port")+" 1: Advanced",(function(){var d=h("div");d.appendChild(detailsTable(S.ports[0],true));return d})(),[h("button",{class:"ctl pri",text:t("c_apply")})]);
};
VARIANTS.H2=function(){
  statusHead();
  S.ports.forEach(function(p,idx){
    var tb=$("ptable").tBodies[0],open=idx===0||idx===S.ports.length-1;
    var tr=baseRow(tb,p,{before:function(tr){statusCells(tr,p);tr.insertCell().appendChild(toggleBtn(open,"Advanced"))}});
    if(!open)return;
    var dc=tb.insertRow().insertCell();dc.colSpan=tr.cells.length;dc.style.cssText="background:var(--panel2);padding:6px 8px 10px 52px";
    dc.appendChild(detailsTable(p,true));
  });
};
function statusRows(p){
  return[[t("nav_fc"),fcNeg(p)],[t("nav_eee"),eeeSt(p)]].map(function(r,i){
    return h("div",{style:"display:grid;grid-template-columns:7.5em 1fr;gap:10px;align-items:center;padding:4px 0"+(i?"":";margin-top:6px")},[h("span",{class:"mut small",text:r[0]}),h("span",{html:r[1]})]);
  });
}
VARIANTS.G2M=function(){
  cardList(function(p){return statusRows(p).concat([h("div",{style:"border-top:1px solid var(--bd);padding-top:8px;margin-top:6px"},[advBtn(p)])])});
  modal(t("c_port")+" 1: Advanced",detailsList(S.ports[0],true),[h("button",{class:"ctl pri",text:t("c_apply")})]);
};
VARIANTS.H2M=function(){
  cardList(function(p,i){
    var open=i===0,more=h("div",{style:"border-top:1px solid var(--bd);padding-top:8px;margin-top:6px"},[toggleBtn(open,"Advanced")]);
    if(open)more.appendChild(h("div",{style:"background:var(--panel2);border-radius:6px;padding:0 10px;margin-top:6px"},[detailsList(p,true)]));
    return statusRows(p).concat([more]);
  });
};
["F","G","H","M","N","HM","G2","H2","G2M","H2M"].forEach(function(v){var f=VARIANTS[v];VARIANTS[v]=function(){LINKINFO=true;f()}});
VARIANTS.G2L=function(){VARIANTS.G2();linkModal(S.ports[2])};
VARIANTS.G2ML=function(){VARIANTS.G2M();linkModal(S.ports[0])};
VARIANTS.G2C=function(){VARIANTS.G2();closeModal()};
VARIANTS.G2MC=function(){VARIANTS.G2M();closeModal()};
VARIANTS.FL=function(){VARIANTS.F();linkModal(S.ports[0])};
VARIANTS.ML=function(){VARIANTS.M();linkModal(S.ports[0])};
VARIANTS.FL3=function(){VARIANTS.F();linkModal(S.ports[2])};

// J: Enabled | Port | Name | Link (i) | Speed | MTU | Flow control (i) | EEE (i), one Apply for the whole page.
// Every control is a draft; changed ports are highlighted and only they are sent on Apply.
function infoModal(title,body){modal(title,body);$("modal").style.width="fit-content";$("modal").style.minWidth="260px"}
function tick(on){return on?badge("\u2713","ok"):'<span class="mut">-</span>'}
// Two plain rows, label left: what this port advertises and what the link partner advertises
function kvTable(rows){
  var tb=h("table",{class:"t",style:"width:auto"}),b=tb.createTBody();
  rows.forEach(function(x){var tr=b.insertRow(),c=tr.insertCell();c.className="mut";c.textContent=x[0];tr.insertCell().textContent=x[1]});
  return tb;
}
function sfpNote(){return h("p",{class:"small mut",style:"max-width:16em",text:"SFP ports do not advertise this."})}
function fcInfo(p){
  var f=FC[p.portNum];
  infoModal(t("c_port")+" "+p.portNum+": "+t("nav_fc"),p.isSFP?sfpNote():kvTable([[t("e_adv"),fcFlags(f&3)],[t("e_lp"),fcFlags(f>>2&3)]]));
}
function eeeInfo(p){
  var e=EEE[p.portNum];
  infoModal(t("c_port")+" "+p.portNum+": "+t("nav_eee"),p.isSFP?sfpNote():kvTable([[t("e_adv"),eeeFlags(e.adv)],[t("e_lp"),eeeFlags(e.lp)]]));
}
function iBtn(fn){return h("button",{title:"Details",style:"background:none;border:0;padding:0 0 0 4px;cursor:pointer;color:var(--ac);font-size:15px;vertical-align:middle",text:"\u24d8",onclick:fn})}
var J={orig:[],rows:[]};
function jControls(p){
  var i=p.portNum-1,r={};
  r.en=sw(p.enabled).firstChild;
  r.name=h("input",{class:"in",size:"9",maxlength:"15",value:p.name||"",placeholder:"-"});
  r.spd=h("select",{class:"in"});
  (p.isSFP?SFPRATES:SPEEDS).forEach(function(o){r.spd.appendChild(h("option",{value:o[0],text:speedLabel(o[1])}))});
  r.mtu=h("input",{class:"in",id:"pmtu"+i,type:"number",min:"64",max:"16383",style:"width:5.5em"});
  r.fc=p.isSFP?null:sw(FC[p.portNum]&1).firstChild;
  r.eee=p.isSFP?null:sw(parseInt(EEE[p.portNum].adv,2)).firstChild;
  [r.en,r.name,r.spd,r.mtu,r.fc,r.eee].forEach(function(c){if(c){c.addEventListener("input",jUpdate);c.addEventListener("change",jUpdate)}});
  J.rows[i]=r;
  return r;
}
// Every status badge in a column is as wide as the widest label that column can show in the current
// language, so the ⓘ that follows lines up without right-aligning it
function fitBadges(){
  var sets={fitlink:LINKS.slice(1).concat([t("st_off"),t("st_down")]),fitfc:["TX","RX","TX/RX",t("e_inactive"),t("st_off"),t("e_na")],fiteee:[t("e_active"),t("e_inactive"),t("st_off"),t("e_na")]};
  var probe=h("div",{style:"position:absolute;visibility:hidden"});document.body.appendChild(probe);
  var css="";
  for(var k in sets){
    probe.innerHTML=sets[k].map(function(s){return badge(s,"ok")}).join("");
    var w=0;[].forEach.call(probe.children,function(b){b.style.display="inline-block";w=Math.max(w,b.offsetWidth)});
    css+="."+k+" .badge{display:inline-block;box-sizing:border-box;text-align:center;min-width:"+Math.ceil(w)+"px}";
  }
  probe.remove();
  // Off and Inactive/Down follow the port strip tiles: disabled (faded, dashed) and no link (solid outline)
  css+=".badge.st-off{opacity:.45;outline:1px dashed var(--fg2);outline-offset:-1px}.badge.st-in{outline:1px solid var(--bd);outline-offset:-1px}";
  var st=$("fitcss")||document.head.appendChild(h("style",{id:"fitcss"}));st.textContent=css;
}
function jFeat(ctl,st,info,p){
  // the n/a badge takes the switch's place, so it gets the switch's width
  return h("span",{style:"white-space:nowrap;display:inline-flex;gap:6px;align-items:center"},
    [ctl?ctl.parentNode:h("span",{style:"width:34px;flex:none",html:badge(t("e_na"))}),
     h("span",{class:info===fcInfo?"fitfc":"fiteee",html:st}),iBtn(function(){info(p)})]);
}
function jRead(i){var r=J.rows[i];return{en:r.en.checked,name:r.name.value.trim(),spd:r.spd.value,mtu:r.mtu.value,fc:r.fc&&r.fc.checked,eee:r.eee&&r.eee.checked}}
function jSnapshot(){J.rows.forEach(function(_,i){J.orig[i]=jRead(i)})}
// Full desired state of one port: what the screen shows, whatever the switch had before
function jCmds(p,s){
  var n=p.portNum,c=[];
  if(s.name)c.push("port "+n+" name "+s.name);
  if(s.en){
    if(p.isSFP){c.push("port "+n+" on");c.push("sfp "+S.sfpSlot[n-1]+" "+s.spd)}
    else c.push("port "+n+" "+s.spd);
  }
  if(s.mtu)c.push("mtu "+n+" "+s.mtu);
  if(!p.isSFP){c.push("port "+n+" fc "+(s.fc?"on":"off"));c.push("eee "+(s.eee?"on":"off")+" "+n)}
  if(!s.en)c.push("port "+n+" off");
  return c;
}
function jUpdate(){
  var ports=0,cmds=[];
  J.rows.forEach(function(r,i){
    var a=J.orig[i],b=jRead(i),row=r.row,dirty=false;
    if(!a)return;
    ["en","name","spd","mtu","fc","eee"].forEach(function(k){
      var ch=a[k]!==b[k],el=r[k];if(ch)dirty=true;
      if(!el)return;
      var tgt=el.type==="checkbox"?el.nextSibling:el;
      tgt.style.outline=ch?"2px solid var(--ac)":"";tgt.style.outlineOffset=ch?"2px":"";
    });
    row.style.background=dirty?"var(--sel)":"";
    if(r.apply)r.apply.disabled=!dirty;
    if(dirty){ports++;cmds=cmds.concat(jCmds(S.ports[i],b))}
  });
  if(!$("jcount"))return;
  $("jcount").textContent=ports?ports+" port"+(ports>1?"s":"")+" changed":"No changes";
  $("jprev").textContent=cmds.join("  \u00b7  ");
  $("japply").disabled=!ports;
}
// ROWAPPLY: an Apply button per row (enabled only while that port has changes) instead of the page-wide bar
var ROWAPPLY=false;
function rowApply(r){r.apply=h("button",{class:"ctl",text:t("c_apply")});return r.apply}
// Copper and SFP speed lists differ in length; give every Speed select the widest one's width
function evenSelects(){
  var s=J.rows.map(function(r){return r.spd}),w=0;
  s.forEach(function(e){w=Math.max(w,e.offsetWidth)});
  s.forEach(function(e){e.style.width=w+"px"});
}
function jBar(){
  return h("div",{class:"mockx",style:"position:sticky;bottom:0;z-index:5;display:flex;gap:12px;align-items:center;background:var(--panel);border-top:1px solid var(--bd);padding:10px 2px 2px;margin-top:8px"},[
    h("b",{id:"jcount",style:"white-space:nowrap",text:"No changes"}),
    h("span",{id:"jprev",class:"mono mut",style:"flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap"}),
    h("button",{class:"ctl pri",id:"japply",text:t("c_apply")})]);
}
// Edits shown in the screenshots: port 2 renamed, port 3 flow control off, port 5 MTU 9000
function jDemo(){
  var r=J.rows;
  r[1].name.value="nas";r[2].fc.checked=false;r[4].mtu.value="9000";
  jUpdate();
}
VARIANTS.J=function(){
  J={orig:[],rows:[]};fitBadges();
  setHead([[t("c_port")],[t("c_enabled")],[t("c_name")],[t("c_link")],[t("pt_speed")],[t("pt_mtu")],[t("nav_fc")],[t("nav_eee")]].concat(ROWAPPLY?[[""]]:[]));
  var tb=$("ptable").tBodies[0];
  S.ports.forEach(function(p){
    var i=p.portNum-1,r=jControls(p),tr=tb.insertRow();r.row=tr;
    tr.insertCell().textContent=p.portNum+(p.isSFP?" (SFP)":"");
    tr.insertCell().appendChild(r.en.parentNode);
    tr.insertCell().appendChild(r.name);
    var lk=tr.insertCell();lk.id="plink"+i;lk.style.whiteSpace="nowrap";
    tr.insertCell().appendChild(r.spd);
    tr.insertCell().appendChild(r.mtu);
    tr.insertCell().appendChild(jFeat(r.fc,fcNeg(p),fcInfo,p));
    tr.insertCell().appendChild(jFeat(r.eee,eeeSt(p),eeeInfo,p));
    if(ROWAPPLY)tr.insertCell().appendChild(rowApply(r));
  });
  evenSelects();
  if(!ROWAPPLY)$("ptable").closest(".scrollx").after(jBar());
  loadMtu().then(function(){jSnapshot();jUpdate();if(JDEMO)setTimeout(jDemo,300)});
};
// JM: phone cards with the same controls and the same page-wide Apply bar
VARIANTS.JM=function(){
  J={orig:[],rows:[]};fitBadges();
  var wrap=$("ptable").closest(".scrollx");wrap.style.display="none";
  var list=h("div",{class:"mockx",style:"display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:10px"});
  S.ports.forEach(function(p){
    var i=p.portNum-1,r=jControls(p);
    r.name.style.cssText="width:100%;box-sizing:border-box";r.spd.style.width="100%";
    var g=h("div",{style:"display:grid;grid-template-columns:7.5em 1fr;gap:6px 10px;align-items:center"});
    [[t("c_name"),r.name],[t("c_link"),h("div",{id:"plink"+i})],[t("pt_speed"),r.spd],[t("pt_mtu"),r.mtu],
     [t("nav_fc"),jFeat(r.fc,fcNeg(p),fcInfo,p)],[t("nav_eee"),jFeat(r.eee,eeeSt(p),eeeInfo,p)]].forEach(function(kv){
      g.appendChild(h("span",{class:"mut small",text:kv[0]}));g.appendChild(h("div",null,[kv[1]]));
    });
    r.row=h("div",{style:"border:1px solid var(--bd);border-radius:8px;padding:10px 12px"},[
      h("div",{style:"display:flex;gap:10px;align-items:center;margin-bottom:8px"},[h("b",{text:t("c_port")+" "+p.portNum+(p.isSFP?" (SFP)":"")}),r.en.parentNode].concat(ROWAPPLY?[h("span",{style:"flex:1"}),rowApply(r)]:[])),g]);
    list.appendChild(r.row);
  });
  wrap.after(list);
  if(!ROWAPPLY)list.after(jBar());
  loadMtu().then(function(){jSnapshot();jUpdate();if(JDEMO)setTimeout(jDemo,300)});
};
// JDDE: same as JD in German, to show the badge widths follow the language
VARIANTS.JDDE=function(){rtlLang="de";LANG.de.nav_fc="Flusskontrolle";i18nApply();VARIANTS.JD()};
var JDEMO=false;
["J","JM"].forEach(function(v){var f=VARIANTS[v];VARIANTS[v]=function(){LINKINFO=true;f()};VARIANTS[v+"D"]=function(){JDEMO=true;VARIANTS[v]()}});
VARIANTS.JRD=function(){ROWAPPLY=true;VARIANTS.JD()};
VARIANTS.JRMD=function(){ROWAPPLY=true;VARIANTS.JMD()};
// JRDC: per-row Apply with cell padding 8px -> 5px left/right, the change it takes to fit at 1100 px
VARIANTS.JRDC=function(){document.head.appendChild(h("style",{text:"#ptable th,#ptable td{padding-left:5px;padding-right:5px}"}));VARIANTS.JRD()};
VARIANTS.JFC=function(){VARIANTS.J();fcInfo(S.ports[2])};
VARIANTS.JEEE=function(){VARIANTS.J();eeeInfo(S.ports[0])};
VARIANTS.JL=function(){VARIANTS.J();linkModal(S.ports[2])};
VARIANTS.JML=function(){VARIANTS.JM();linkModal(S.ports[2])};
VARIANTS.JMFC=function(){VARIANTS.JM();fcInfo(S.ports[2])};
VARIANTS.JMEEE=function(){VARIANTS.JM();eeeInfo(S.ports[0])};
// ST: Devices learned moves to the Statistics page (appended after RX bad, since statsStatus fills cells 0-6)
VARIANTS.ST=function(){
  showTab("stats");statsStatus();
  var th=$("stable").tHead.rows[0];th.insertBefore(h("th",{class:"num",text:t("pt_conn")}),th.lastElementChild);
  S.ports.forEach(function(p){var r=$("stable").tBodies[0].rows[p.portNum-1];var c=r.insertCell(7);c.className="num";c.textContent=parseInt(p.macs,16)&0x1fff});
};
function fcFlags(b){return["Pause","Asym"].filter(function(s,i){return b&(1<<i)}).join(", ")||"-"}
function mockStatus(){
  fakeData();updateStrip();
  S.ports.forEach(function(p){
    var i=p.portNum-1,el=$("plink"+i),mc=$("pmac"+i);
    if(mc)mc.textContent=parseInt(p.macs,16)&0x1fff;
    if(el){el.innerHTML=!LINKINFO?linkBadge(p):!p.enabled?dash():p.link>0?linkUp(p):inact(t("st_down"));if(LINKINFO){var w=h("span",{style:"display:inline-flex;align-items:center;white-space:nowrap"},[h("span",{class:"fitlink",html:el.innerHTML}),infoBtn(p)]);el.innerHTML="";el.appendChild(w)}}
  });
}
function mock(v){
  fakeData();clearExtras();VARIANTS[v]();mockStatus();loadMtu();
  tabHooks.ports.status=mockStatus;
  buildPorts=function(){};
}
